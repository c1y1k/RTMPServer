
#include "../../chunk_parser.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <exception>
#include <fcntl.h>
#include <functional>
#include <iostream>
#include <pthread.h>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

using Bytes = std::vector<uint8_t>;
using ParseStatus = ChunkParser::ParseStatus;

volatile std::sig_atomic_t signal_count = 0;
volatile std::sig_atomic_t hold_signal_handler = 0;

extern "C" void interrupt_read_handler(int) {
    ++signal_count;
    while (hold_signal_handler != 0) {
    }
}

class SignalHandlerGuard {
public:
    SignalHandlerGuard() {
        struct sigaction action {};
        action.sa_handler = interrupt_read_handler;
        ::sigemptyset(&action.sa_mask);
        action.sa_flags = 0;
        if (::sigaction(SIGUSR1, &action, &old_action_) == -1) {
            throw std::runtime_error("failed to install SIGUSR1 handler");
        }
    }

    ~SignalHandlerGuard() {
        hold_signal_handler = 0;
        ::sigaction(SIGUSR1, &old_action_, nullptr);
    }

    SignalHandlerGuard(const SignalHandlerGuard&) = delete;
    SignalHandlerGuard& operator=(const SignalHandlerGuard&) = delete;

private:
    struct sigaction old_action_ {};
};

class NonblockingSocketPair {
public:
    explicit NonblockingSocketPair(bool nonblocking = true) {
        int sockets[2] = {-1, -1};
        if (::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == -1) {
            throw std::runtime_error("socketpair failed");
        }
        client_fd_ = sockets[0];
        server_fd_ = sockets[1];

        try {
            if (nonblocking) {
                set_nonblocking(client_fd_);
                set_nonblocking(server_fd_);
            }
        } catch (...) {
            ::close(client_fd_);
            ::close(server_fd_);
            throw;
        }
    }

    ~NonblockingSocketPair() {
        if (client_fd_ != -1) {
            ::close(client_fd_);
        }
        if (server_fd_ != -1) {
            ::close(server_fd_);
        }
    }

    NonblockingSocketPair(const NonblockingSocketPair&) = delete;
    NonblockingSocketPair& operator=(const NonblockingSocketPair&) = delete;

    int client_fd() const { return client_fd_; }
    int server_fd() const { return server_fd_; }

    bool make_server_nonblocking() {
        const int flags = ::fcntl(server_fd_, F_GETFL, 0);
        return flags != -1 &&
               ::fcntl(server_fd_, F_SETFL, flags | O_NONBLOCK) != -1;
    }

    void close_client() {
        if (client_fd_ != -1) {
            ::close(client_fd_);
            client_fd_ = -1;
        }
    }

private:
    static void set_nonblocking(int fd) {
        const int flags = ::fcntl(fd, F_GETFL, 0);
        if (flags == -1 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1) {
            throw std::runtime_error("failed to set socket nonblocking");
        }
    }

    int client_fd_ = -1;
    int server_fd_ = -1;
};

void expect(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void write_all(int fd, const Bytes& data) {
    size_t written = 0;
    while (written < data.size()) {
        const ssize_t result = ::write(fd,
                                       data.data() + written,
                                       data.size() - written);
        if (result > 0) {
            written += static_cast<size_t>(result);
            continue;
        }
        if (result == -1 && errno == EINTR) {
            continue;
        }
        throw std::runtime_error("test socket write failed");
    }
}

Bytes receive_buffer_bytes(const ChunkParser& parser) {
    return Bytes(parser.receive_buffer.chunk_buffer.begin(),
                 parser.receive_buffer.chunk_buffer.begin() +
                     static_cast<std::ptrdiff_t>(parser.receive_buffer.length));
}

void append_u24_be(Bytes& out, uint32_t value) {
    out.push_back(static_cast<uint8_t>((value >> 16) & 0xFF));
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>(value & 0xFF));
}

void append_u32_be(Bytes& out, uint32_t value) {
    out.push_back(static_cast<uint8_t>((value >> 24) & 0xFF));
    out.push_back(static_cast<uint8_t>((value >> 16) & 0xFF));
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>(value & 0xFF));
}

void append_u32_le(Bytes& out, uint32_t value) {
    out.push_back(static_cast<uint8_t>(value & 0xFF));
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>((value >> 16) & 0xFF));
    out.push_back(static_cast<uint8_t>((value >> 24) & 0xFF));
}

uint8_t basic_header(uint8_t fmt, uint32_t csid) {
    expect(fmt <= 3, "fmt must be in [0, 3]");
    expect(csid >= 2 && csid <= 63,
           "test builder currently supports direct CSID values only");
    return static_cast<uint8_t>((fmt << 6) | csid);
}

void append_basic_header(Bytes& out, uint8_t fmt, uint32_t csid) {
    expect(fmt <= 3, "fmt must be in [0, 3]");
    expect(csid >= 2 && csid <= 65599,
           "CSID must be in the RTMP Basic Header range");

    if (csid <= 63) {
        out.push_back(static_cast<uint8_t>((fmt << 6) | csid));
        return;
    }

    const uint32_t encoded_csid = csid - 64;
    if (csid <= 319) {
        out.push_back(static_cast<uint8_t>(fmt << 6));
        out.push_back(static_cast<uint8_t>(encoded_csid));
        return;
    }

    out.push_back(static_cast<uint8_t>((fmt << 6) | 1));
    out.push_back(static_cast<uint8_t>(encoded_csid & 0xFF));
    out.push_back(static_cast<uint8_t>((encoded_csid >> 8) & 0xFF));
}

Bytes make_fmt0(uint32_t csid,
                uint32_t timestamp,
                uint32_t message_length,
                uint8_t message_type,
                uint32_t message_stream_id,
                const Bytes& payload,
                bool extended = false) {
    Bytes out;
    append_basic_header(out, 0, csid);
    append_u24_be(out, extended ? 0x00FFFFFFU : timestamp);
    append_u24_be(out, message_length);
    out.push_back(message_type);
    append_u32_le(out, message_stream_id);
    if (extended) {
        append_u32_be(out, timestamp);
    }
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

Bytes make_fmt1(uint32_t csid,
                uint32_t timestamp_delta,
                uint32_t message_length,
                uint8_t message_type,
                const Bytes& payload,
                bool extended = false) {
    Bytes out;
    append_basic_header(out, 1, csid);
    append_u24_be(out, extended ? 0x00FFFFFFU : timestamp_delta);
    append_u24_be(out, message_length);
    out.push_back(message_type);
    if (extended) {
        append_u32_be(out, timestamp_delta);
    }
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

Bytes make_fmt2(uint32_t csid,
                uint32_t timestamp_delta,
                const Bytes& payload,
                bool extended = false) {
    Bytes out;
    append_basic_header(out, 2, csid);
    append_u24_be(out, extended ? 0x00FFFFFFU : timestamp_delta);
    if (extended) {
        append_u32_be(out, timestamp_delta);
    }
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

Bytes make_fmt3(uint32_t csid,
                const Bytes& payload,
                bool extended = false,
                uint32_t extended_value = 0) {
    Bytes out;
    append_basic_header(out, 3, csid);
    if (extended) {
        append_u32_be(out, extended_value);
    }
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

Bytes join(const Bytes& first, const Bytes& second) {
    Bytes result = first;
    result.insert(result.end(), second.begin(), second.end());
    return result;
}

void feed(ChunkParser& parser, const Bytes& data) {
    const size_t write_pos = parser.receive_buffer.length;
    const size_t required = write_pos + data.size();
    if (parser.receive_buffer.chunk_buffer.size() < required) {
        parser.receive_buffer.chunk_buffer.resize(required);
    }
    std::copy(data.begin(), data.end(),
              parser.receive_buffer.chunk_buffer.begin() + write_pos);
    parser.receive_buffer.length = required;
}

Bytes payload_of(const ChunkParser& parser,
                 const ChunkParser::ParseResult& result) {
    expect(result.status == ParseStatus::CHUNK_READY,
           "payload requested for a non-ready result");
    expect(result.body_pos + result.body_length <=
               parser.receive_buffer.length,
           "payload range exceeds receive buffer");
    const auto begin = parser.receive_buffer.chunk_buffer.begin() +
                       static_cast<std::ptrdiff_t>(result.body_pos);
    return Bytes(begin, begin + static_cast<std::ptrdiff_t>(result.body_length));
}

void expect_status(const ChunkParser::ParseResult& result,
                   ParseStatus expected,
                   const std::string& context) {
    expect(result.status == expected, context + ": unexpected parse status");
}

void expect_message_info(const ChunkParser::ParseResult& result,
                         bool message_start,
                         uint32_t timestamp,
                         uint32_t message_length,
                         uint8_t message_type_id,
                         uint32_t message_stream_id,
                         const std::string& context) {
    expect(result.message_start == message_start,
           context + ": wrong message_start flag");
    expect(result.metadata.timestamp == timestamp,
           context + ": wrong metadata timestamp");
    expect(result.metadata.message_length == message_length,
           context + ": wrong metadata message length");
    expect(result.metadata.message_type_id == message_type_id,
           context + ": wrong metadata message type ID");
    expect(result.metadata.message_stream_id == message_stream_id,
           context + ": wrong metadata message stream ID");
}

void test_single_fmt0_chunk() {
    ChunkParser parser;
    const Bytes expected_payload{'a', 'b', 'c'};
    feed(parser, make_fmt0(3, 10, 3, 8, 1, expected_payload));

    const auto result = parser.ChunkParse();
    expect_status(result, ParseStatus::CHUNK_READY, "single fmt0 chunk");
    expect(result.csid == 3, "single fmt0: wrong CSID");
    expect(result.body_length == 3, "single fmt0: wrong body length");
    expect(payload_of(parser, result) == expected_payload,
           "single fmt0: wrong payload");
    expect_message_info(result, true, 10, 3, 8, 1, "single fmt0");

    const auto& header = parser.current_chunk_header.at(3);
    expect(header.timestamp == 10, "single fmt0: wrong timestamp");
    expect(header.message_length == 3, "single fmt0: wrong message length");
    expect(header.message_type_id == 8, "single fmt0: wrong message type");
    expect(header.message_stream_id == 1,
           "single fmt0: wrong message stream ID");
    expect(header.bytes_remaining == 0,
           "single fmt0: message should be complete");
}

void test_extended_csid_boundaries() {
    const std::vector<uint32_t> csids{64, 319, 320, 65599};

    for (const uint32_t test_csid : csids) {
        ChunkParser parser;
        const Bytes expected_payload{
            static_cast<uint8_t>(test_csid & 0xFF),
            static_cast<uint8_t>((test_csid >> 8) & 0xFF)};

        feed(parser, make_fmt0(test_csid,
                               77,
                               static_cast<uint32_t>(expected_payload.size()),
                               8,
                               5,
                               expected_payload));

        const auto result = parser.ChunkParse();
        const std::string context =
            "extended CSID " + std::to_string(test_csid);
        expect_status(result, ParseStatus::CHUNK_READY, context);
        expect(result.csid == test_csid, context + ": wrong returned CSID");
        expect(payload_of(parser, result) == expected_payload,
               context + ": wrong payload");
        expect_message_info(result, true, 77, 2, 8, 5, context);
        expect(parser.current_chunk_header.count(test_csid) == 1,
               context + ": no per-CSID header state");
        expect(parser.current_chunk_header.at(test_csid).csid == test_csid,
               context + ": wrong cached CSID");
        expect(parser.current_chunk_header.at(test_csid).bytes_remaining == 0,
               context + ": message should be complete");
    }
}

void test_message_stream_id_little_endian() {
    ChunkParser parser;
    const Bytes raw_chunk{
        0x03,                   // fmt 0, CSID 3
        0x00, 0x00, 0x01,       // timestamp
        0x00, 0x00, 0x01,       // message length
        0x08,                   // message type ID
        0x78, 0x56, 0x34, 0x12, // Message Stream ID, little-endian
        0xAB};                  // payload
    feed(parser, raw_chunk);

    const auto result = parser.ChunkParse();
    expect_status(result, ParseStatus::CHUNK_READY,
                  "Message Stream ID little-endian");
    expect(result.metadata.message_stream_id == 0x12345678U,
           "Message Stream ID was not decoded as little-endian");
    expect(payload_of(parser, result) == Bytes{0xAB},
           "Message Stream ID endian test payload is wrong");
}

void test_three_byte_csid_little_endian() {
    ChunkParser parser;
    constexpr uint32_t encoded_csid = 0x1234U;
    constexpr uint32_t expected_csid = encoded_csid + 64U;
    const Bytes raw_chunk{
        0x01,             // fmt 0, three-byte Basic Header marker
        0x34, 0x12,       // encoded CSID, low byte first
        0x00, 0x00, 0x02, // timestamp
        0x00, 0x00, 0x01, // message length
        0x09,             // message type ID
        0x01, 0x00, 0x00, 0x00,
        0xCD};
    feed(parser, raw_chunk);

    const auto result = parser.ChunkParse();
    expect_status(result, ParseStatus::CHUNK_READY,
                  "three-byte CSID little-endian");
    expect(result.csid == expected_csid,
           "three-byte CSID was not decoded low-byte-first");
    expect(parser.current_chunk_header.count(expected_csid) == 1,
           "three-byte CSID state was stored under the wrong key");
    expect(payload_of(parser, result) == Bytes{0xCD},
           "three-byte CSID endian test payload is wrong");
}

void test_one_chunk_fragmented_input() {
    ChunkParser parser;
    const Bytes full = make_fmt0(3, 20, 3, 8, 1, {'x', 'y', 'z'});

    feed(parser, Bytes(full.begin(), full.begin() + 1));
    expect_status(parser.ChunkParse(), ParseStatus::NEED_MORE_DATA,
                  "fragmented basic header only");

    feed(parser, Bytes(full.begin() + 1, full.begin() + 6));
    expect_status(parser.ChunkParse(), ParseStatus::NEED_MORE_DATA,
                  "fragmented message header");

    feed(parser, Bytes(full.begin() + 6, full.begin() + 12));
    expect_status(parser.ChunkParse(), ParseStatus::NEED_MORE_DATA,
                  "complete header without payload");

    feed(parser, Bytes(full.begin() + 12, full.end()));
    const auto result = parser.ChunkParse();
    expect_status(result, ParseStatus::CHUNK_READY,
                  "fragmented payload completion");
    expect(payload_of(parser, result) == Bytes({'x', 'y', 'z'}),
           "fragmented input: wrong payload");
    expect_message_info(result, true, 20, 3, 8, 1,
                        "fragmented input");
}

void test_multiple_chunks_in_one_input() {
    ChunkParser parser;
    const Bytes first = make_fmt0(3, 1, 2, 8, 1, {'a', 'b'});
    const Bytes second = make_fmt0(4, 2, 2, 9, 2, {'1', '2'});
    feed(parser, join(first, second));

    const auto first_result = parser.ChunkParse();
    expect_status(first_result, ParseStatus::CHUNK_READY,
                  "coalesced first chunk");
    expect(first_result.csid == 3, "coalesced first chunk: wrong CSID");
    expect(payload_of(parser, first_result) == Bytes({'a', 'b'}),
           "coalesced first chunk: wrong payload");
    expect_message_info(first_result, true, 1, 2, 8, 1,
                        "coalesced first chunk");

    const auto second_result = parser.ChunkParse();
    expect_status(second_result, ParseStatus::CHUNK_READY,
                  "coalesced second chunk");
    expect(second_result.csid == 4, "coalesced second chunk: wrong CSID");
    expect(payload_of(parser, second_result) == Bytes({'1', '2'}),
           "coalesced second chunk: wrong payload");
    expect_message_info(second_result, true, 2, 2, 9, 2,
                        "coalesced second chunk");
}

void test_message_across_chunks() {
    ChunkParser parser;
    expect(parser.SetChunkSize(3), "failed to set test chunk size");
    const Bytes first = make_fmt0(3, 5, 6, 8, 1, {'a', 'b', 'c'});
    const Bytes second = make_fmt3(3, {'d', 'e', 'f'});
    feed(parser, join(first, second));

    Bytes assembled;
    auto result = parser.ChunkParse();
    expect_status(result, ParseStatus::CHUNK_READY,
                  "multi-chunk message first chunk");
    const Bytes first_payload = payload_of(parser, result);
    assembled.insert(assembled.end(), first_payload.begin(), first_payload.end());
    expect_message_info(result, true, 5, 6, 8, 1,
                        "multi-chunk message first chunk");
    expect(parser.current_chunk_header.at(3).bytes_remaining == 3,
           "multi-chunk message: wrong remaining length after first chunk");

    result = parser.ChunkParse();
    expect_status(result, ParseStatus::CHUNK_READY,
                  "multi-chunk message continuation");
    const Bytes second_payload = payload_of(parser, result);
    assembled.insert(assembled.end(), second_payload.begin(), second_payload.end());
    expect_message_info(result, false, 5, 6, 8, 1,
                        "multi-chunk message continuation");

    expect(assembled == Bytes({'a', 'b', 'c', 'd', 'e', 'f'}),
           "multi-chunk message: reconstructed test payload is wrong");
    expect(parser.current_chunk_header.at(3).bytes_remaining == 0,
           "multi-chunk message should be complete");
}

void test_default_chunk_size_payload_boundary() {
    ChunkParser parser;
    const Bytes first_payload(128, static_cast<uint8_t>('a'));
    const Bytes second_payload{static_cast<uint8_t>('b')};
    Bytes input = make_fmt0(3, 10, 129, 8, 1, first_payload);
    input = join(input, make_fmt3(3, second_payload));
    feed(parser, input);

    auto result = parser.ChunkParse();
    expect_status(result, ParseStatus::CHUNK_READY,
                  "default chunk size first fragment");
    expect(result.body_length == 128,
           "default chunk size did not limit payload to 128 bytes");
    expect(payload_of(parser, result) == first_payload,
           "default chunk size first payload is wrong");
    expect_message_info(result, true, 10, 129, 8, 1,
                        "default chunk size first fragment");

    result = parser.ChunkParse();
    expect_status(result, ParseStatus::CHUNK_READY,
                  "default chunk size final fragment");
    expect(result.body_length == 1,
           "default chunk size final payload is not 1 byte");
    expect(payload_of(parser, result) == second_payload,
           "default chunk size final payload is wrong");
    expect_message_info(result, false, 10, 129, 8, 1,
                        "default chunk size final fragment");
    expect(parser.current_chunk_header.at(3).bytes_remaining == 0,
           "default chunk size message should be complete");
}

void test_updated_chunk_size_payload_boundary() {
    ChunkParser parser;
    expect(parser.SetChunkSize(256), "failed to set chunk size to 256");

    const Bytes first_payload(256, static_cast<uint8_t>('x'));
    const Bytes second_payload(44, static_cast<uint8_t>('y'));
    Bytes input = make_fmt0(3, 20, 300, 9, 2, first_payload);
    input = join(input, make_fmt3(3, second_payload));
    feed(parser, input);

    auto result = parser.ChunkParse();
    expect_status(result, ParseStatus::CHUNK_READY,
                  "updated chunk size first fragment");
    expect(result.body_length == 256,
           "updated chunk size did not limit payload to 256 bytes");
    expect(payload_of(parser, result) == first_payload,
           "updated chunk size first payload is wrong");
    expect_message_info(result, true, 20, 300, 9, 2,
                        "updated chunk size first fragment");

    result = parser.ChunkParse();
    expect_status(result, ParseStatus::CHUNK_READY,
                  "updated chunk size final fragment");
    expect(result.body_length == 44,
           "updated chunk size final payload is not 44 bytes");
    expect(payload_of(parser, result) == second_payload,
           "updated chunk size final payload is wrong");
    expect_message_info(result, false, 20, 300, 9, 2,
                        "updated chunk size final fragment");
    expect(parser.current_chunk_header.at(3).bytes_remaining == 0,
           "updated chunk size message should be complete");
}

void test_chunk_size_update_with_unfinished_csid() {
    ChunkParser parser;
    const Bytes csid3_first(128, static_cast<uint8_t>('a'));
    feed(parser, make_fmt0(3, 30, 400, 8, 1, csid3_first));

    auto result = parser.ChunkParse();
    expect_status(result, ParseStatus::CHUNK_READY,
                  "unfinished CSID before chunk size update");
    expect(result.body_length == 128,
           "unfinished CSID did not use the old chunk size");
    expect(parser.current_chunk_header.at(3).bytes_remaining == 272,
           "unfinished CSID has wrong remaining length before update");

    expect(parser.SetChunkSize(256),
           "failed to update chunk size while a CSID was unfinished");

    const Bytes csid4_first(256, static_cast<uint8_t>('x'));
    const Bytes csid4_last(44, static_cast<uint8_t>('y'));
    const Bytes csid3_second(256, static_cast<uint8_t>('b'));
    const Bytes csid3_last(16, static_cast<uint8_t>('c'));
    Bytes input = make_fmt0(4, 40, 300, 9, 2, csid4_first);
    input = join(input, make_fmt3(4, csid4_last));
    input = join(input, make_fmt3(3, csid3_second));
    input = join(input, make_fmt3(3, csid3_last));
    feed(parser, input);

    result = parser.ChunkParse();
    expect_status(result, ParseStatus::CHUNK_READY,
                  "new CSID after chunk size update");
    expect(result.csid == 4 && result.body_length == 256,
           "new CSID did not use the updated chunk size");
    expect(payload_of(parser, result) == csid4_first,
           "new CSID first payload is wrong after update");

    result = parser.ChunkParse();
    expect_status(result, ParseStatus::CHUNK_READY,
                  "new CSID final fragment after update");
    expect(result.csid == 4 && result.body_length == 44,
           "new CSID final payload has the wrong size");
    expect(payload_of(parser, result) == csid4_last,
           "new CSID final payload is wrong after update");

    result = parser.ChunkParse();
    expect_status(result, ParseStatus::CHUNK_READY,
                  "unfinished CSID continuation after update");
    expect(result.csid == 3 && result.body_length == 256,
           "unfinished CSID continuation did not use updated chunk size");
    expect(payload_of(parser, result) == csid3_second,
           "unfinished CSID continuation payload is wrong");

    result = parser.ChunkParse();
    expect_status(result, ParseStatus::CHUNK_READY,
                  "unfinished CSID final fragment after update");
    expect(result.csid == 3 && result.body_length == 16,
           "unfinished CSID final payload has the wrong size");
    expect(payload_of(parser, result) == csid3_last,
           "unfinished CSID final payload is wrong");
    expect(parser.current_chunk_header.at(3).bytes_remaining == 0 &&
               parser.current_chunk_header.at(4).bytes_remaining == 0,
           "messages should both be complete after chunk size update");
}

void test_fmt1_fmt2_fmt3_inheritance() {
    ChunkParser parser;
    feed(parser, make_fmt0(3, 10, 2, 8, 7, {'a', 'a'}));
    auto result = parser.ChunkParse();
    expect_status(result, ParseStatus::CHUNK_READY, "fmt0 inheritance base");
    expect_message_info(result, true, 10, 2, 8, 7,
                        "fmt0 inheritance base");

    feed(parser, make_fmt1(3, 5, 3, 9, {'b', 'b', 'b'}));
    result = parser.ChunkParse();
    expect_status(result, ParseStatus::CHUNK_READY, "fmt1 inherited stream ID");
    expect_message_info(result, true, 15, 3, 9, 7,
                        "fmt1 inherited stream ID");
    auto header = parser.current_chunk_header.at(3);
    expect(header.timestamp == 15, "fmt1: wrong inherited timestamp");
    expect(header.message_stream_id == 7, "fmt1: stream ID not inherited");
    expect(header.message_length == 3 && header.message_type_id == 9,
           "fmt1: new length or type not applied");

    feed(parser, make_fmt2(3, 7, {'c', 'c', 'c'}));
    result = parser.ChunkParse();
    expect_status(result, ParseStatus::CHUNK_READY,
                  "fmt2 inherited length and type");
    expect_message_info(result, true, 22, 3, 9, 7,
                        "fmt2 inherited length and type");
    header = parser.current_chunk_header.at(3);
    expect(header.timestamp == 22, "fmt2: wrong timestamp");
    expect(header.message_length == 3 && header.message_type_id == 9 &&
               header.message_stream_id == 7,
           "fmt2: historical fields not inherited");

    feed(parser, make_fmt3(3, {'d', 'd', 'd'}));
    result = parser.ChunkParse();
    expect_status(result, ParseStatus::CHUNK_READY,
                  "fmt3 new message inheritance");
    expect_message_info(result, true, 29, 3, 9, 7,
                        "fmt3 new message inheritance");
    header = parser.current_chunk_header.at(3);
    expect(header.timestamp == 29, "fmt3 new message: wrong timestamp");
    expect(header.message_length == 3 && header.message_type_id == 9 &&
               header.message_stream_id == 7,
           "fmt3 new message: historical fields not inherited");
}

void test_interleaved_csids() {
    ChunkParser parser;
    expect(parser.SetChunkSize(2), "failed to set interleaving chunk size");

    Bytes input = make_fmt0(3, 10, 4, 8, 1, {'a', 'b'});
    input = join(input, make_fmt0(4, 20, 4, 9, 2, {'1', '2'}));
    input = join(input, make_fmt3(3, {'c', 'd'}));
    input = join(input, make_fmt3(4, {'3', '4'}));
    feed(parser, input);

    const std::vector<uint32_t> expected_csids{3, 4, 3, 4};
    const std::vector<Bytes> expected_payloads{
        {'a', 'b'}, {'1', '2'}, {'c', 'd'}, {'3', '4'}};
    const std::vector<bool> expected_message_starts{true, true, false, false};
    const std::vector<uint32_t> expected_timestamps{10, 20, 10, 20};
    const std::vector<uint32_t> expected_lengths{4, 4, 4, 4};
    const std::vector<uint8_t> expected_types{8, 9, 8, 9};
    const std::vector<uint32_t> expected_stream_ids{1, 2, 1, 2};

    for (size_t i = 0; i < expected_csids.size(); ++i) {
        const auto result = parser.ChunkParse();
        expect_status(result, ParseStatus::CHUNK_READY,
                      "interleaved CSID chunk");
        expect(result.csid == expected_csids[i],
               "interleaved CSID: wrong output order");
        expect(payload_of(parser, result) == expected_payloads[i],
               "interleaved CSID: wrong payload");
        expect_message_info(result,
                            expected_message_starts[i],
                            expected_timestamps[i],
                            expected_lengths[i],
                            expected_types[i],
                            expected_stream_ids[i],
                            "interleaved CSID chunk");
    }

    const auto& header3 = parser.current_chunk_header.at(3);
    const auto& header4 = parser.current_chunk_header.at(4);
    expect(header3.timestamp == 10 && header3.message_type_id == 8 &&
               header3.message_stream_id == 1,
           "CSID 3 state was polluted");
    expect(header4.timestamp == 20 && header4.message_type_id == 9 &&
               header4.message_stream_id == 2,
           "CSID 4 state was polluted");
    expect(header3.bytes_remaining == 0 && header4.bytes_remaining == 0,
           "interleaved messages should both be complete");
}

void test_extended_timestamps() {
    {
        ChunkParser parser;
        expect(parser.SetChunkSize(2), "failed to set extended timestamp chunk size");
        constexpr uint32_t timestamp = 0x01000000U;
        Bytes input = make_fmt0(3, timestamp, 4, 8, 1, {'a', 'b'}, true);
        input = join(input, make_fmt3(3, {'c', 'd'}, true, timestamp));
        feed(parser, input);

        auto result = parser.ChunkParse();
        expect_status(result, ParseStatus::CHUNK_READY,
                      "extended absolute timestamp first chunk");
        expect_message_info(result, true, timestamp, 4, 8, 1,
                            "extended absolute timestamp first chunk");
        expect(parser.current_chunk_header.at(3).timestamp == timestamp,
               "extended absolute timestamp was not parsed");

        result = parser.ChunkParse();
        expect_status(result, ParseStatus::CHUNK_READY,
                      "fmt3 extended timestamp continuation");
        expect_message_info(result, false, timestamp, 4, 8, 1,
                            "fmt3 extended timestamp continuation");
        expect(parser.current_chunk_header.at(3).timestamp == timestamp,
               "fmt3 continuation changed the message timestamp");
    }

    {
        ChunkParser parser;
        feed(parser, make_fmt0(3, 100, 1, 8, 1, {'a'}));
        auto result = parser.ChunkParse();
        expect_status(result, ParseStatus::CHUNK_READY,
                      "extended delta base message");
        expect_message_info(result, true, 100, 1, 8, 1,
                            "extended delta base message");

        constexpr uint32_t delta = 0x01000000U;
        feed(parser, make_fmt1(3, delta, 1, 8, {'b'}, true));
        result = parser.ChunkParse();
        expect_status(result, ParseStatus::CHUNK_READY,
                      "extended delta fmt1 message");
        expect_message_info(result, true, 100U + delta, 1, 8, 1,
                            "extended delta fmt1 message");
        expect(parser.current_chunk_header.at(3).timestamp == 100U + delta,
               "extended timestamp delta was not applied");

        feed(parser, make_fmt3(3, {'c'}, true, delta));
        result = parser.ChunkParse();
        expect_status(result, ParseStatus::CHUNK_READY,
                      "extended delta fmt3 new message");
        expect_message_info(result, true, 100U + delta + delta, 1, 8, 1,
                            "extended delta fmt3 new message");
        expect(parser.current_chunk_header.at(3).timestamp == 100U + delta + delta,
               "fmt3 new message did not inherit extended delta");
    }
}

void test_fmt2_extended_timestamp_delta() {
    ChunkParser parser;
    feed(parser, make_fmt0(3, 100, 1, 8, 1, {'a'}));
    auto result = parser.ChunkParse();
    expect_status(result, ParseStatus::CHUNK_READY,
                  "fmt2 extended delta base message");
    expect_message_info(result, true, 100, 1, 8, 1,
                        "fmt2 extended delta base message");

    constexpr uint32_t delta = 0x01000000U;
    const Bytes fmt2 = make_fmt2(3, delta, {'b'}, true);

    feed(parser, Bytes(fmt2.begin(), fmt2.begin() + 4));
    expect_status(parser.ChunkParse(), ParseStatus::NEED_MORE_DATA,
                  "fmt2 extended delta without extended field");

    feed(parser, Bytes(fmt2.begin() + 4, fmt2.begin() + 6));
    expect_status(parser.ChunkParse(), ParseStatus::NEED_MORE_DATA,
                  "fmt2 truncated extended delta");

    feed(parser, Bytes(fmt2.begin() + 6, fmt2.end()));
    result = parser.ChunkParse();
    expect_status(result, ParseStatus::CHUNK_READY,
                  "fmt2 extended delta completion");
    expect(payload_of(parser, result) == Bytes({'b'}),
           "fmt2 extended delta: wrong payload");
    expect_message_info(result, true, 100U + delta, 1, 8, 1,
                        "fmt2 extended delta completion");

    const auto& fmt2_header = parser.current_chunk_header.at(3);
    expect(fmt2_header.timestamp_delta == delta,
           "fmt2 extended delta was not cached");
    expect(fmt2_header.timestamp == 100U + delta,
           "fmt2 extended delta produced the wrong absolute timestamp");

    feed(parser, make_fmt3(3, {'c'}, true, delta));
    result = parser.ChunkParse();
    expect_status(result, ParseStatus::CHUNK_READY,
                  "fmt3 inherited fmt2 extended delta");
    expect(payload_of(parser, result) == Bytes({'c'}),
           "fmt3 inherited fmt2 delta: wrong payload");
    expect_message_info(result, true, 100U + delta + delta, 1, 8, 1,
                        "fmt3 inherited fmt2 extended delta");
}

void test_zero_length_message() {
    ChunkParser parser;
    feed(parser, make_fmt0(3, 42, 0, 4, 9, {}));

    const auto result = parser.ChunkParse();
    expect_status(result, ParseStatus::CHUNK_READY, "zero-length message");
    expect(result.body_length == 0, "zero-length message returned payload");
    expect_message_info(result, true, 42, 0, 4, 9,
                        "zero-length message");
    expect(parser.current_chunk_header.at(3).bytes_remaining == 0,
           "zero-length message has remaining bytes");
}

void test_truncated_headers_and_payload() {
    {
        ChunkParser parser;
        feed(parser, {0x00});
        expect_status(parser.ChunkParse(), ParseStatus::NEED_MORE_DATA,
                      "truncated two-byte basic header");
    }
    {
        ChunkParser parser;
        feed(parser, {0x01, 0x00});
        expect_status(parser.ChunkParse(), ParseStatus::NEED_MORE_DATA,
                      "truncated three-byte basic header");
    }
    {
        ChunkParser parser;
        const Bytes full = make_fmt0(3, 1, 1, 8, 1, {'x'});
        feed(parser, Bytes(full.begin(), full.begin() + 8));
        expect_status(parser.ChunkParse(), ParseStatus::NEED_MORE_DATA,
                      "truncated fmt0 message header");
    }
    {
        ChunkParser parser;
        const Bytes full = make_fmt0(3, 0x01000000U, 1, 8, 1, {'x'}, true);
        feed(parser, Bytes(full.begin(), full.begin() + 14));
        expect_status(parser.ChunkParse(), ParseStatus::NEED_MORE_DATA,
                      "truncated extended timestamp");
    }
    {
        ChunkParser parser;
        const Bytes full = make_fmt0(3, 1, 4, 8, 1, {'a', 'b'});
        feed(parser, full);
        expect_status(parser.ChunkParse(), ParseStatus::NEED_MORE_DATA,
                      "truncated payload");
    }
}

void test_invalid_fields_and_lengths() {
    {
        ChunkParser parser;
        feed(parser, {basic_header(1, 3)});
        expect_status(parser.ChunkParse(), ParseStatus::PROTOCOL_ERROR,
                      "new CSID beginning with fmt1");
    }
    {
        ChunkParser parser;
        expect(parser.SetChunkSize(2), "failed to set invalid-sequence chunk size");
        feed(parser, make_fmt0(3, 1, 4, 8, 1, {'a', 'b'}));
        expect_status(parser.ChunkParse(), ParseStatus::CHUNK_READY,
                      "invalid sequence base chunk");
        feed(parser, make_fmt1(3, 1, 2, 8, {'x', 'y'}));
        expect_status(parser.ChunkParse(), ParseStatus::PROTOCOL_ERROR,
                      "non-fmt3 while message remains incomplete");
    }
    {
        ChunkParser parser;
        expect(!parser.SetChunkSize(0), "zero chunk size should be rejected");
        expect(!parser.SetChunkSize(0x80000000U),
               "chunk size with high bit set should be rejected");
        expect(!parser.SetChunkSize(
                   static_cast<uint32_t>(parser.receive_buffer.max_cap + 1)),
               "chunk size above local buffer limit should be rejected");
        expect(parser.SetChunkSize(4096), "valid chunk size should be accepted");
        expect(parser.in_chunk_size == 4096,
               "accepted chunk size was not stored");
    }
}

void test_read_lt_nonblocking() {
    NonblockingSocketPair sockets;
    ChunkParser parser;
    const Bytes expected{'l', 't', '-', 'd', 'a', 't', 'a'};
    write_all(sockets.client_fd(), expected);

    expect(parser.Read(sockets.server_fd(), ChunkParser::TriggerMode::LT) ==
               "success",
           "LT read did not report success");
    expect(parser.receive_buffer.length == expected.size(),
           "LT read stored the wrong byte count");
    expect(receive_buffer_bytes(parser) == expected,
           "LT read stored the wrong bytes");

    const size_t length_before_eagain = parser.receive_buffer.length;
    expect(parser.Read(sockets.server_fd(), ChunkParser::TriggerMode::LT) ==
               "success",
           "LT EAGAIN should be a normal result");
    expect(parser.receive_buffer.length == length_before_eagain,
           "LT EAGAIN changed the receive buffer");
}

void test_read_et_drains_until_eagain() {
    NonblockingSocketPair sockets;
    ChunkParser parser;
    const Bytes first{'e', 't', '-'};
    const Bytes second{'d', 'r', 'a', 'i', 'n'};
    write_all(sockets.client_fd(), first);
    write_all(sockets.client_fd(), second);

    expect(parser.Read(sockets.server_fd(), ChunkParser::TriggerMode::ET) ==
               "success",
           "ET read did not finish at EAGAIN");
    expect(parser.receive_buffer.length == first.size() + second.size(),
           "ET read did not drain all available bytes");
    expect(receive_buffer_bytes(parser) == join(first, second),
           "ET read changed byte order or content");

    const size_t length_before_eagain = parser.receive_buffer.length;
    expect(parser.Read(sockets.server_fd(), ChunkParser::TriggerMode::ET) ==
               "success",
           "ET EAGAIN should be a normal result");
    expect(parser.receive_buffer.length == length_before_eagain,
           "ET EAGAIN changed the receive buffer");
}

void test_read_peer_close() {
    {
        NonblockingSocketPair sockets;
        ChunkParser parser;
        sockets.close_client();
        expect(parser.Read(sockets.server_fd(), ChunkParser::TriggerMode::LT) ==
                   "close",
               "LT did not report peer close");
    }
    {
        NonblockingSocketPair sockets;
        ChunkParser parser;
        sockets.close_client();
        expect(parser.Read(sockets.server_fd(), ChunkParser::TriggerMode::ET) ==
                   "close",
               "ET did not report peer close");
    }
}

std::string trigger_mode_name(ChunkParser::TriggerMode mode) {
    return mode == ChunkParser::TriggerMode::LT ? "LT" : "ET";
}

void test_receive_buffer_compaction() {
    const std::vector<ChunkParser::TriggerMode> modes{
        ChunkParser::TriggerMode::LT,
        ChunkParser::TriggerMode::ET};

    for (const auto mode : modes) {
        NonblockingSocketPair sockets;
        ChunkParser parser;
        parser.receive_buffer = ReceiveBuffer(8);

        const Bytes initial{'x', 'x', 'x', 'x', 'o', 'l', 'd', '!'};
        std::copy(initial.begin(), initial.end(),
                  parser.receive_buffer.chunk_buffer.begin());
        parser.receive_buffer.length = initial.size();
        parser.receive_buffer.parse_pos = 4;

        const Bytes incoming{'n', 'e', 'w'};
        write_all(sockets.client_fd(), incoming);
        const std::string context = trigger_mode_name(mode) + " compaction";

        expect(parser.Read(sockets.server_fd(), mode) == "success",
               context + ": Read failed");
        expect(parser.receive_buffer.parse_pos == 0,
               context + ": parse_pos was not reset");
        expect(parser.receive_buffer.length == 7,
               context + ": wrong length after compaction and read");
        expect(parser.receive_buffer.capacity == 8,
               context + ": buffer expanded unnecessarily");
        expect(parser.receive_buffer.chunk_buffer.size() == 8,
               context + ": vector size changed unexpectedly");
        expect(receive_buffer_bytes(parser) ==
                   Bytes({'o', 'l', 'd', '!', 'n', 'e', 'w'}),
               context + ": unread or new data was corrupted");
    }
}

void test_receive_buffer_growth() {
    const std::vector<ChunkParser::TriggerMode> modes{
        ChunkParser::TriggerMode::LT,
        ChunkParser::TriggerMode::ET};

    for (const auto mode : modes) {
        {
            NonblockingSocketPair sockets;
            ChunkParser parser;
            parser.receive_buffer = ReceiveBuffer(4);
            const Bytes initial{'o', 'l', 'd', '!'};
            std::copy(initial.begin(), initial.end(),
                      parser.receive_buffer.chunk_buffer.begin());
            parser.receive_buffer.length = initial.size();
            parser.receive_buffer.parse_pos = 0;

            const Bytes incoming{'n', 'e'};
            write_all(sockets.client_fd(), incoming);
            const std::string context = trigger_mode_name(mode) + " growth";

            expect(parser.Read(sockets.server_fd(), mode) == "success",
                   context + ": Read failed");
            expect(parser.receive_buffer.capacity == 8,
                   context + ": capacity did not double");
            expect(parser.receive_buffer.chunk_buffer.size() == 8,
                   context + ": vector size and capacity field differ");
            expect(parser.receive_buffer.length == 6,
                   context + ": wrong length after growth");
            expect(receive_buffer_bytes(parser) ==
                       Bytes({'o', 'l', 'd', '!', 'n', 'e'}),
                   context + ": data was corrupted during growth");
        }

        {
            NonblockingSocketPair sockets;
            ChunkParser parser;
            parser.receive_buffer = ReceiveBuffer(4);
            parser.receive_buffer.max_cap = 6;
            const Bytes initial{'1', '2', '3', '4'};
            std::copy(initial.begin(), initial.end(),
                      parser.receive_buffer.chunk_buffer.begin());
            parser.receive_buffer.length = initial.size();

            write_all(sockets.client_fd(), {'5'});
            const std::string context =
                trigger_mode_name(mode) + " growth capped at max_cap";

            expect(parser.Read(sockets.server_fd(), mode) == "success",
                   context + ": Read failed");
            expect(parser.receive_buffer.capacity == 6,
                   context + ": capacity did not clamp to max_cap");
            expect(parser.receive_buffer.chunk_buffer.size() == 6,
                   context + ": vector size did not clamp to max_cap");
            expect(receive_buffer_bytes(parser) ==
                       Bytes({'1', '2', '3', '4', '5'}),
                   context + ": data was corrupted at capped growth");
        }
    }
}

void test_receive_buffer_full_limit() {
    const std::vector<ChunkParser::TriggerMode> modes{
        ChunkParser::TriggerMode::LT,
        ChunkParser::TriggerMode::ET};

    for (const auto mode : modes) {
        NonblockingSocketPair sockets;
        ChunkParser parser;
        parser.receive_buffer = ReceiveBuffer(4);
        parser.receive_buffer.max_cap = 4;
        const Bytes initial{'f', 'u', 'l', 'l'};
        std::copy(initial.begin(), initial.end(),
                  parser.receive_buffer.chunk_buffer.begin());
        parser.receive_buffer.length = initial.size();
        parser.receive_buffer.parse_pos = 0;
        write_all(sockets.client_fd(), {'x'});

        const std::string context = trigger_mode_name(mode) + " buffer_full";
        expect(parser.Read(sockets.server_fd(), mode) == "buffer_full",
               context + ": full buffer did not report buffer_full");
        expect(parser.receive_buffer.length == 4 &&
                   parser.receive_buffer.parse_pos == 0 &&
                   parser.receive_buffer.capacity == 4,
               context + ": full buffer state changed");
        expect(receive_buffer_bytes(parser) == initial,
               context + ": existing data was corrupted");

        uint8_t unread_byte = 0;
        const ssize_t read_result =
            ::read(sockets.server_fd(), &unread_byte, sizeof(unread_byte));
        expect(read_result == 1 && unread_byte == static_cast<uint8_t>('x'),
               context + ": Read consumed socket data after buffer_full");
    }
}

void run_eintr_retry_test(ChunkParser::TriggerMode mode) {
    SignalHandlerGuard signal_guard;
    NonblockingSocketPair sockets(false);
    ChunkParser parser;
    std::atomic<bool> reader_entered{false};
    std::string read_status;

    signal_count = 0;
    hold_signal_handler = 1;

    std::thread reader([&] {
        reader_entered.store(true, std::memory_order_release);
        read_status = parser.Read(sockets.server_fd(), mode);
    });

    while (!reader_entered.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    const int kill_result = ::pthread_kill(reader.native_handle(), SIGUSR1);
    bool handler_observed = false;
    for (int attempt = 0; attempt < 1000; ++attempt) {
        if (signal_count > 0) {
            handler_observed = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    bool nonblocking_switch_ok = true;
    if (mode == ChunkParser::TriggerMode::ET) {
        nonblocking_switch_ok = sockets.make_server_nonblocking();
    }

    const Bytes expected{'i', 'n', 't', 'r'};
    const ssize_t write_result =
        ::write(sockets.client_fd(), expected.data(), expected.size());
    hold_signal_handler = 0;
    reader.join();

    const std::string context = trigger_mode_name(mode) + " EINTR retry";
    expect(kill_result == 0, context + ": pthread_kill failed");
    expect(handler_observed, context + ": signal handler was not observed");
    expect(nonblocking_switch_ok,
           context + ": failed to switch ET socket to nonblocking");
    expect(write_result == static_cast<ssize_t>(expected.size()),
           context + ": failed to write wake-up data");
    expect(read_status == "success", context + ": Read did not recover");
    expect(receive_buffer_bytes(parser) == expected,
           context + ": retried read returned wrong data");
}

void test_read_lt_retries_eintr() {
    run_eintr_retry_test(ChunkParser::TriggerMode::LT);
}

void test_read_et_retries_eintr() {
    run_eintr_retry_test(ChunkParser::TriggerMode::ET);
}

}  // namespace

int main() {
    const std::vector<std::pair<std::string, std::function<void()>>> tests{
        {"single fmt0 chunk", test_single_fmt0_chunk},
        {"extended CSID boundaries", test_extended_csid_boundaries},
        {"Message Stream ID little-endian",
         test_message_stream_id_little_endian},
        {"three-byte CSID little-endian",
         test_three_byte_csid_little_endian},
        {"one chunk fragmented input", test_one_chunk_fragmented_input},
        {"multiple chunks in one input", test_multiple_chunks_in_one_input},
        {"message across chunks", test_message_across_chunks},
        {"default chunk size payload boundary",
         test_default_chunk_size_payload_boundary},
        {"updated chunk size payload boundary",
         test_updated_chunk_size_payload_boundary},
        {"chunk size update with unfinished CSID",
         test_chunk_size_update_with_unfinished_csid},
        {"fmt1-fmt3 inheritance", test_fmt1_fmt2_fmt3_inheritance},
        {"interleaved CSIDs", test_interleaved_csids},
        {"extended timestamps", test_extended_timestamps},
        {"fmt2 extended timestamp delta", test_fmt2_extended_timestamp_delta},
        {"zero-length message", test_zero_length_message},
        {"truncated headers and payload", test_truncated_headers_and_payload},
        {"invalid fields and lengths", test_invalid_fields_and_lengths},
        {"Read LT nonblocking", test_read_lt_nonblocking},
        {"Read ET drains until EAGAIN", test_read_et_drains_until_eagain},
        {"Read peer close", test_read_peer_close},
        {"ReceiveBuffer compaction", test_receive_buffer_compaction},
        {"ReceiveBuffer growth", test_receive_buffer_growth},
        {"ReceiveBuffer full limit", test_receive_buffer_full_limit},
        {"Read LT retries EINTR", test_read_lt_retries_eintr},
        {"Read ET retries EINTR", test_read_et_retries_eintr},
    };

    size_t passed = 0;
    for (const auto& test : tests) {
        try {
            test.second();
            ++passed;
            std::cout << "[PASS] " << test.first << '\n';
        } catch (const std::exception& error) {
            std::cerr << "[FAIL] " << test.first << ": " << error.what()
                      << '\n';
        }
    }

    std::cout << passed << '/' << tests.size() << " tests passed\n";
    return passed == tests.size() ? 0 : 1;
}
