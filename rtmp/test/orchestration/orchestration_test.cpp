#include "rtmp.h"

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <fcntl.h>
#include <iostream>
#include <memory>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

constexpr size_t kDefaultChunkSize = 128;
constexpr int kIoTimeoutMs = 2000;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
    }
    return condition;
}

bool set_nonblocking(int fd) {
    const int flags = fcntl(fd, F_GETFL, 0);
    return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

bool wait_for(int fd, short events) {
    pollfd descriptor{fd, events, 0};
    for (;;) {
        const int result = poll(&descriptor, 1, kIoTimeoutMs);
        if (result > 0) {
            return (descriptor.revents & events) != 0;
        }
        if (result < 0 && errno == EINTR) {
            continue;
        }
        return false;
    }
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
        if (result < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) &&
            wait_for(fd, POLLOUT)) {
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
        if (result < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) &&
            wait_for(fd, POLLIN)) {
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

struct EncodedMessage {
    std::vector<std::vector<uint8_t>> chunks;

    std::vector<uint8_t> flatten() const {
        std::vector<uint8_t> output;
        for (const auto& chunk : chunks) {
            output.insert(output.end(), chunk.begin(), chunk.end());
        }
        return output;
    }
};

EncodedMessage encode_message(uint32_t csid,
                              uint32_t timestamp,
                              uint8_t message_type_id,
                              uint32_t message_stream_id,
                              const std::vector<uint8_t>& payload) {
    EncodedMessage encoded;
    std::vector<uint8_t> first;
    first.push_back(static_cast<uint8_t>(csid));
    append_be24(first, timestamp);
    append_be24(first, static_cast<uint32_t>(payload.size()));
    first.push_back(message_type_id);
    append_le32(first, message_stream_id);

    size_t offset = 0;
    const size_t first_length = std::min(payload.size(), kDefaultChunkSize);
    first.insert(first.end(), payload.begin(), payload.begin() + first_length);
    offset += first_length;
    encoded.chunks.push_back(std::move(first));

    while (offset < payload.size()) {
        std::vector<uint8_t> continuation;
        continuation.push_back(static_cast<uint8_t>(0xc0U | csid));
        const size_t length = std::min(payload.size() - offset, kDefaultChunkSize);
        continuation.insert(continuation.end(), payload.begin() + offset,
                            payload.begin() + offset + length);
        offset += length;
        encoded.chunks.push_back(std::move(continuation));
    }
    return encoded;
}

std::vector<uint8_t> make_payload(size_t length, uint8_t seed) {
    std::vector<uint8_t> payload(length);
    for (size_t index = 0; index < length; ++index) {
        payload[index] = static_cast<uint8_t>((index * 31U + seed) & 0xffU);
    }
    return payload;
}

class TestConnection {
public:
    explicit TestConnection(Rtmp::TriggerMode mode) {
        int sockets[2] = {-1, -1};
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0) {
            return;
        }
        if (!set_nonblocking(sockets[0]) || !set_nonblocking(sockets[1])) {
            close(sockets[0]);
            close(sockets[1]);
            return;
        }
        client_fd_ = sockets[1];
        server_ = std::make_unique<Rtmp>(sockets[0], mode);
    }

    ~TestConnection() {
        server_.reset();
        if (client_fd_ != -1) {
            close(client_fd_);
        }
    }

    bool valid() const {
        return server_ != nullptr && client_fd_ != -1;
    }

    bool send_bytes(const std::vector<uint8_t>& data) {
        return send_all(client_fd_, data.data(), data.size());
    }

    bool send_bytes(const uint8_t* data, size_t length) {
        return send_all(client_fd_, data, length);
    }

    Rtmp::ProcessResult process() {
        return server_->process();
    }

    bool complete_handshake(const std::vector<uint8_t>& bytes_after_c2 = {},
                            Rtmp::ProcessResult* final_result = nullptr) {
        std::vector<uint8_t> c0c1(1537, 0);
        c0c1[0] = 3;
        c0c1[1] = 0x10;
        c0c1[2] = 0x20;
        c0c1[3] = 0x30;
        c0c1[4] = 0x40;
        for (size_t index = 9; index < c0c1.size(); ++index) {
            c0c1[index] = static_cast<uint8_t>((index * 13U) & 0xffU);
        }
        if (!send_bytes(c0c1)) {
            return false;
        }

        auto result = process();
        if (result.status != Rtmp::StepStatus::NeedRead ||
            result.stage != Rtmp::ProcessStage::Handshake) {
            return false;
        }

        std::vector<uint8_t> response(3073);
        if (!receive_exact(client_fd_, response.data(), response.size())) {
            return false;
        }

        std::vector<uint8_t> c2_and_tail(1536 + bytes_after_c2.size(), 0);
        std::copy(response.begin() + 1, response.begin() + 5,
                  c2_and_tail.begin());
        std::copy(response.begin() + 9, response.begin() + 1537,
                  c2_and_tail.begin() + 8);
        std::copy(bytes_after_c2.begin(), bytes_after_c2.end(),
                  c2_and_tail.begin() + 1536);
        if (!send_bytes(c2_and_tail)) {
            return false;
        }

        result = process();
        if (final_result != nullptr) {
            *final_result = result;
        }
        if (bytes_after_c2.empty()) {
            return result.status == Rtmp::StepStatus::NeedRead &&
                   result.stage == Rtmp::ProcessStage::ChunkRead;
        }
        return true;
    }

private:
    int client_fd_ = -1;
    std::unique_ptr<Rtmp> server_;
};

bool expect_message(const Rtmp::ProcessResult& result,
                    uint32_t timestamp,
                    uint8_t message_type_id,
                    uint32_t message_stream_id,
                    const std::vector<uint8_t>& payload) {
    bool ok = true;
    ok &= expect(result.status == Rtmp::StepStatus::Progress,
                 "completed Message must return Progress");
    ok &= expect(result.stage == Rtmp::ProcessStage::MessageAssemble,
                 "completed Message stage must be MessageAssemble");
    ok &= expect(result.message_ptr != nullptr,
                 "completed Message must return message_ptr");
    if (!result.message_ptr) {
        return false;
    }
    ok &= expect(result.message_ptr->metadata.timestamp == timestamp,
                 "Message timestamp mapping");
    ok &= expect(result.message_ptr->metadata.message_length == payload.size(),
                 "Message length mapping");
    ok &= expect(result.message_ptr->metadata.message_type_id == message_type_id,
                 "Message type ID mapping");
    ok &= expect(result.message_ptr->metadata.message_stream_id == message_stream_id,
                 "Message stream ID mapping");
    ok &= expect(result.message_ptr->payload == payload,
                 "Message payload content");
    return ok;
}

bool test_single_chunk(Rtmp::TriggerMode mode) {
    TestConnection connection(mode);
    bool ok = expect(connection.valid(), "create single-chunk connection");
    ok &= expect(connection.complete_handshake(), "complete single-chunk handshake");

    const std::vector<uint8_t> payload = make_payload(64, 7);
    const std::vector<uint8_t> wire = encode_message(3, 1234, 20, 1, payload).flatten();
    ok &= expect(connection.send_bytes(wire), "send single-chunk Message");
    const auto result = connection.process();
    ok &= expect_message(result, 1234, 20, 1, payload);
    return ok;
}

bool test_multi_chunk(Rtmp::TriggerMode mode) {
    TestConnection connection(mode);
    bool ok = expect(connection.valid(), "create multi-chunk connection");
    ok &= expect(connection.complete_handshake(), "complete multi-chunk handshake");

    const std::vector<uint8_t> payload = make_payload(400, 11);
    const EncodedMessage encoded = encode_message(5, 4321, 9, 7, payload);
    ok &= expect(encoded.chunks.size() == 4, "400-byte Message must use four chunks");
    const std::vector<uint8_t> wire = encoded.flatten();
    ok &= expect(connection.send_bytes(wire), "send multi-chunk Message");
    const auto result = connection.process();
    ok &= expect_message(result, 4321, 9, 7, payload);
    return ok;
}

bool test_tcp_fragmentation() {
    TestConnection connection(Rtmp::TriggerMode::ET);
    bool ok = expect(connection.valid(), "create TCP-fragmentation connection");
    ok &= expect(connection.complete_handshake(), "complete fragmentation handshake");

    const std::vector<uint8_t> payload = make_payload(80, 19);
    const std::vector<uint8_t> wire = encode_message(6, 777, 18, 3, payload).flatten();

    ok &= expect(connection.send_bytes(wire.data(), 1), "send Basic Header fragment");
    auto result = connection.process();
    ok &= expect(result.status == Rtmp::StepStatus::NeedRead &&
                     result.stage == Rtmp::ProcessStage::ChunkRead,
                 "Basic Header fragment must need more bytes");

    ok &= expect(connection.send_bytes(wire.data() + 1, 6),
                 "send partial Message Header");
    result = connection.process();
    ok &= expect(result.status == Rtmp::StepStatus::NeedRead &&
                     result.stage == Rtmp::ProcessStage::ChunkRead,
                 "partial Message Header must need more bytes");

    ok &= expect(connection.send_bytes(wire.data() + 7, 20),
                 "send remaining Header and partial Payload");
    result = connection.process();
    ok &= expect(result.status == Rtmp::StepStatus::NeedRead &&
                     result.stage == Rtmp::ProcessStage::ChunkRead,
                 "partial Payload must need more bytes");

    ok &= expect(connection.send_bytes(wire.data() + 27, wire.size() - 27),
                 "send remaining Payload");
    result = connection.process();
    ok &= expect_message(result, 777, 18, 3, payload);
    return ok;
}

bool test_interleaved_csids() {
    TestConnection connection(Rtmp::TriggerMode::ET);
    bool ok = expect(connection.valid(), "create interleaved-CSID connection");
    ok &= expect(connection.complete_handshake(), "complete interleaved-CSID handshake");

    const std::vector<uint8_t> payload_a = make_payload(200, 23);
    const std::vector<uint8_t> payload_b = make_payload(150, 29);
    const EncodedMessage message_a = encode_message(3, 100, 8, 1, payload_a);
    const EncodedMessage message_b = encode_message(4, 200, 9, 2, payload_b);

    std::vector<uint8_t> wire;
    wire.insert(wire.end(), message_a.chunks[0].begin(), message_a.chunks[0].end());
    wire.insert(wire.end(), message_b.chunks[0].begin(), message_b.chunks[0].end());
    wire.insert(wire.end(), message_a.chunks[1].begin(), message_a.chunks[1].end());
    wire.insert(wire.end(), message_b.chunks[1].begin(), message_b.chunks[1].end());

    ok &= expect(connection.send_bytes(wire), "send interleaved CSID chunks");
    auto result = connection.process();
    ok &= expect_message(result, 100, 8, 1, payload_a);
    result = connection.process();
    ok &= expect_message(result, 200, 9, 2, payload_b);
    return ok;
}

bool test_cached_sequential_messages() {
    TestConnection connection(Rtmp::TriggerMode::ET);
    bool ok = expect(connection.valid(), "create sequential-Message connection");
    ok &= expect(connection.complete_handshake(), "complete sequential-Message handshake");

    const std::vector<uint8_t> payload_a = make_payload(32, 31);
    const std::vector<uint8_t> payload_b = make_payload(48, 37);
    std::vector<uint8_t> wire = encode_message(3, 10, 20, 1, payload_a).flatten();
    const std::vector<uint8_t> second = encode_message(3, 20, 20, 1, payload_b).flatten();
    wire.insert(wire.end(), second.begin(), second.end());

    ok &= expect(connection.send_bytes(wire), "send two cached Messages");
    auto result = connection.process();
    ok &= expect_message(result, 10, 20, 1, payload_a);
    result = connection.process();
    ok &= expect_message(result, 20, 20, 1, payload_b);
    return ok;
}

bool test_c2_and_first_chunk_coalesced() {
    TestConnection connection(Rtmp::TriggerMode::ET);
    bool ok = expect(connection.valid(), "create C2-coalescing connection");

    const std::vector<uint8_t> payload = make_payload(96, 41);
    const std::vector<uint8_t> wire = encode_message(7, 555, 18, 9, payload).flatten();
    Rtmp::ProcessResult result{Rtmp::StepStatus::InternalError,
                               Rtmp::ProcessStage::Handshake};
    ok &= expect(connection.complete_handshake(wire, &result),
                 "send C2 and first Chunk together");
    ok &= expect_message(result, 555, 18, 9, payload);
    return ok;
}

bool test_invalid_first_fmt3() {
    TestConnection connection(Rtmp::TriggerMode::LT);
    bool ok = expect(connection.valid(), "create invalid-fmt3 connection");
    ok &= expect(connection.complete_handshake(), "complete invalid-fmt3 handshake");

    const std::vector<uint8_t> invalid_chunk{0xc3};
    ok &= expect(connection.send_bytes(invalid_chunk), "send invalid first fmt 3 chunk");
    const auto result = connection.process();
    ok &= expect(result.status == Rtmp::StepStatus::ProtocolError &&
                     result.stage == Rtmp::ProcessStage::ChunkParse,
                 "invalid first fmt 3 must map to ChunkParse protocol error");
    return ok;
}

}  // namespace

int main() {
    bool ok = true;
    ok &= test_single_chunk(Rtmp::TriggerMode::LT);
    ok &= test_single_chunk(Rtmp::TriggerMode::ET);
    ok &= test_multi_chunk(Rtmp::TriggerMode::LT);
    ok &= test_multi_chunk(Rtmp::TriggerMode::ET);
    ok &= test_tcp_fragmentation();
    ok &= test_interleaved_csids();
    ok &= test_cached_sequential_messages();
    ok &= test_c2_and_first_chunk_coalesced();
    ok &= test_invalid_first_fmt3();

    if (!ok) {
        return 1;
    }
    std::cout << "9/9 orchestration tests passed\n";
    return 0;
}
