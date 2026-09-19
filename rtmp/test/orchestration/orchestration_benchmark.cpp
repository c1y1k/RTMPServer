#include "rtmp.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fcntl.h>
#include <iomanip>
#include <iostream>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

namespace {

constexpr size_t kDefaultChunkSize = 128;

bool set_nonblocking(int fd) {
    const int flags = fcntl(fd, F_GETFL, 0);
    return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

bool send_all(int fd, const uint8_t* data, size_t length) {
    size_t sent = 0;
    while (sent < length) {
        const ssize_t result = send(fd, data + sent, length - sent, MSG_NOSIGNAL);
        if (result > 0) {
            sent += static_cast<size_t>(result);
            continue;
        }
        if (result < 0 && errno == EINTR) {
            continue;
        }
        return false;
    }
    return true;
}

bool receive_exact(int fd, uint8_t* data, size_t length) {
    size_t received = 0;
    while (received < length) {
        const ssize_t result = recv(fd, data + received, length - received, 0);
        if (result > 0) {
            received += static_cast<size_t>(result);
            continue;
        }
        if (result < 0 && errno == EINTR) {
            continue;
        }
        return false;
    }
    return true;
}

void append_be24(std::vector<uint8_t>& output, uint32_t value) {
    output.push_back(static_cast<uint8_t>((value >> 16) & 0xffU));
    output.push_back(static_cast<uint8_t>((value >> 8) & 0xffU));
    output.push_back(static_cast<uint8_t>(value & 0xffU));
}

void append_le32(std::vector<uint8_t>& output, uint32_t value) {
    output.push_back(static_cast<uint8_t>(value & 0xffU));
    output.push_back(static_cast<uint8_t>((value >> 8) & 0xffU));
    output.push_back(static_cast<uint8_t>((value >> 16) & 0xffU));
    output.push_back(static_cast<uint8_t>((value >> 24) & 0xffU));
}

std::vector<uint8_t> encode_message(const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> wire;
    wire.reserve(payload.size() + payload.size() / kDefaultChunkSize + 12);
    wire.push_back(3);
    append_be24(wire, 100);
    append_be24(wire, static_cast<uint32_t>(payload.size()));
    wire.push_back(9);
    append_le32(wire, 1);

    size_t offset = 0;
    size_t length = std::min(payload.size(), kDefaultChunkSize);
    wire.insert(wire.end(), payload.begin(), payload.begin() + length);
    offset += length;
    while (offset < payload.size()) {
        wire.push_back(0xc3);
        length = std::min(payload.size() - offset, kDefaultChunkSize);
        wire.insert(wire.end(), payload.begin() + offset,
                    payload.begin() + offset + length);
        offset += length;
    }
    return wire;
}

bool complete_handshake(Rtmp& server, int client_fd) {
    std::vector<uint8_t> c0c1(1537, 0);
    c0c1[0] = 3;
    for (size_t index = 9; index < c0c1.size(); ++index) {
        c0c1[index] = static_cast<uint8_t>(index & 0xffU);
    }
    if (!send_all(client_fd, c0c1.data(), c0c1.size())) {
        return false;
    }
    auto result = server.process();
    if (result.status != Rtmp::StepStatus::NeedRead ||
        result.stage != Rtmp::ProcessStage::Handshake) {
        return false;
    }

    std::vector<uint8_t> response(3073);
    if (!receive_exact(client_fd, response.data(), response.size())) {
        return false;
    }
    std::vector<uint8_t> c2(1536, 0);
    std::copy(response.begin() + 1, response.begin() + 5, c2.begin());
    std::copy(response.begin() + 9, response.begin() + 1537, c2.begin() + 8);
    if (!send_all(client_fd, c2.data(), c2.size())) {
        return false;
    }
    result = server.process();
    return result.status == Rtmp::StepStatus::NeedRead &&
           result.stage == Rtmp::ProcessStage::ChunkRead;
}

}  // namespace

int main(int argc, char* argv[]) {
    size_t message_count = 5000;
    size_t payload_size = 1024;
    if (argc > 1) {
        message_count = static_cast<size_t>(std::strtoul(argv[1], nullptr, 10));
    }
    if (argc > 2) {
        payload_size = static_cast<size_t>(std::strtoul(argv[2], nullptr, 10));
    }
    if (message_count == 0 || payload_size == 0 || payload_size > 0x00ffffffU) {
        std::cerr << "message_count and payload_size must be valid nonzero values\n";
        return 2;
    }

    int sockets[2] = {-1, -1};
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0 ||
        !set_nonblocking(sockets[0]) || !set_nonblocking(sockets[1])) {
        std::cerr << "failed to create nonblocking socketpair\n";
        return 1;
    }

    int exit_code = 0;
    {
        Rtmp server(sockets[0], Rtmp::TriggerMode::ET);
        if (!complete_handshake(server, sockets[1])) {
            std::cerr << "failed to complete benchmark handshake\n";
            close(sockets[1]);
            return 1;
        }

        std::vector<uint8_t> payload(payload_size);
        for (size_t index = 0; index < payload.size(); ++index) {
            payload[index] = static_cast<uint8_t>((index * 17U) & 0xffU);
        }
        const std::vector<uint8_t> wire = encode_message(payload);

        const auto start = std::chrono::steady_clock::now();
        for (size_t index = 0; index < message_count; ++index) {
            if (!send_all(sockets[1], wire.data(), wire.size())) {
                std::cerr << "send failed at Message " << index << '\n';
                exit_code = 1;
                break;
            }
            const auto result = server.process();
            if (result.status != Rtmp::StepStatus::Progress ||
                result.stage != Rtmp::ProcessStage::MessageAssemble ||
                !result.message_ptr || result.message_ptr->payload.size() != payload_size) {
                std::cerr << "orchestration failed at Message " << index << '\n';
                exit_code = 1;
                break;
            }
        }
        const auto finish = std::chrono::steady_clock::now();

        if (exit_code == 0) {
            const double total_seconds =
                std::chrono::duration<double>(finish - start).count();
            const double average_microseconds =
                total_seconds * 1000000.0 / static_cast<double>(message_count);
            const double messages_per_second =
                static_cast<double>(message_count) / total_seconds;
            const double total_mib =
                static_cast<double>(message_count) * static_cast<double>(payload_size) /
                (1024.0 * 1024.0);

            std::cout << std::fixed << std::setprecision(2)
                      << "messages: " << message_count << '\n'
                      << "payload_bytes_per_message: " << payload_size << '\n'
                      << "total_payload_mib: " << total_mib << '\n'
                      << "total_seconds: " << total_seconds << '\n'
                      << "average_us_per_message: " << average_microseconds << '\n'
                      << "messages_per_second: " << messages_per_second << '\n'
                      << "payload_mib_per_second: " << total_mib / total_seconds << '\n';
        }
    }

    close(sockets[1]);
    return exit_code;
}
