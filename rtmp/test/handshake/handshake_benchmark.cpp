#include "rtmp.h"

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

bool run_one_handshake(uint32_t sequence) {
    int sockets[2] = {-1, -1};
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0) {
        return false;
    }
    if (!set_nonblocking(sockets[0]) || !set_nonblocking(sockets[1])) {
        close(sockets[0]);
        close(sockets[1]);
        return false;
    }

    bool success = true;
    {
        Rtmp server(sockets[0], Rtmp::TriggerMode::ET);
        std::vector<uint8_t> c0c1(1537, 0);
        c0c1[0] = 3;
        c0c1[1] = static_cast<uint8_t>((sequence >> 24) & 0xffU);
        c0c1[2] = static_cast<uint8_t>((sequence >> 16) & 0xffU);
        c0c1[3] = static_cast<uint8_t>((sequence >> 8) & 0xffU);
        c0c1[4] = static_cast<uint8_t>(sequence & 0xffU);
        for (size_t index = 9; index < c0c1.size(); ++index) {
            c0c1[index] = static_cast<uint8_t>((index + sequence) & 0xffU);
        }

        success = send_all(sockets[1], c0c1.data(), c0c1.size());
        auto result = server.process();
        success = success && result.status == Rtmp::StepStatus::NeedRead &&
                  result.stage == Rtmp::ProcessStage::Handshake;

        std::vector<uint8_t> response(3073);
        success = success && receive_exact(sockets[1], response.data(), response.size());

        std::vector<uint8_t> c2(1536, 0);
        std::copy(response.begin() + 1, response.begin() + 5, c2.begin());
        std::copy(response.begin() + 9, response.begin() + 1537, c2.begin() + 8);
        success = success && send_all(sockets[1], c2.data(), c2.size());

        result = server.process();
        success = success && result.status == Rtmp::StepStatus::NeedRead &&
                  result.stage == Rtmp::ProcessStage::ChunkRead;
    }

    close(sockets[1]);
    return success;
}

}  // namespace

int main(int argc, char* argv[]) {
    size_t iterations = 1000;
    if (argc > 1) {
        const unsigned long parsed = std::strtoul(argv[1], nullptr, 10);
        if (parsed == 0) {
            std::cerr << "iterations must be greater than zero\n";
            return 2;
        }
        iterations = static_cast<size_t>(parsed);
    }

    const auto start = std::chrono::steady_clock::now();
    for (size_t index = 0; index < iterations; ++index) {
        if (!run_one_handshake(static_cast<uint32_t>(index))) {
            std::cerr << "handshake benchmark failed at iteration " << index << '\n';
            return 1;
        }
    }
    const auto finish = std::chrono::steady_clock::now();

    const double total_seconds =
        std::chrono::duration<double>(finish - start).count();
    const double average_microseconds =
        total_seconds * 1000000.0 / static_cast<double>(iterations);
    const double handshakes_per_second =
        static_cast<double>(iterations) / total_seconds;

    std::cout << std::fixed << std::setprecision(2)
              << "handshakes: " << iterations << '\n'
              << "total_seconds: " << total_seconds << '\n'
              << "average_us_per_handshake: " << average_microseconds << '\n'
              << "handshakes_per_second: " << handshakes_per_second << '\n';
    return 0;
}
