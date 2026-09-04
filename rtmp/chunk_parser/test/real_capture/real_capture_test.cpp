#include "../../chunk_parser.h"

#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

using Bytes = std::vector<uint8_t>;

struct ExpectedHeader {
    uint32_t frame = 0;
    int64_t fmt = -1;
    int64_t csid = -1;
    int64_t timestamp = -1;
    int64_t timestamp_delta = -1;
    int64_t message_length = -1;
    int64_t type_id = -1;
    int64_t stream_id = -1;
};

struct MessageBody {
    MessageMetadata metadata{};
    Bytes bytes;
    size_t message_number = 0;
};

struct FrameSpan {
    size_t start = 0;
    size_t end = 0;
    uint32_t frame = 0;
};

struct ActualHeader {
    size_t chunk_number = 0;
    uint32_t fmt = 0;
    uint32_t csid = 0;
    uint32_t timestamp = 0;
    uint32_t timestamp_delta = 0;
    uint32_t message_length = 0;
    uint32_t type_id = 0;
    uint32_t stream_id = 0;
    bool matched = false;
};

struct ChunkRecord {
    size_t chunk_number = 0;
    size_t message_number = 0;
    size_t start = 0;
    size_t end = 0;
    size_t header_length = 0;
    size_t payload_length = 0;
    uint32_t fmt = 0;
    uint32_t csid = 0;
    bool message_start = false;
    std::vector<uint32_t> frames;
};

int64_t parse_optional(const std::string& value) {
    return value == "-" ? -1 : std::stoll(value, nullptr, 0);
}

std::vector<ExpectedHeader> read_reference(const std::string& path) {
    std::ifstream input(path);
    if (!input) {
        throw std::runtime_error("cannot open Wireshark reference: " + path);
    }

    std::string line;
    std::getline(input, line);
    std::vector<ExpectedHeader> headers;
    while (std::getline(input, line)) {
        if (line.empty()) {
            continue;
        }
        std::istringstream row(line);
        std::vector<std::string> fields;
        std::string field;
        while (std::getline(row, field, '\t')) {
            fields.push_back(field);
        }
        if (fields.size() != 8) {
            throw std::runtime_error("invalid Wireshark reference row: " + line);
        }
        headers.push_back(ExpectedHeader{
            static_cast<uint32_t>(std::stoul(fields[0])),
            parse_optional(fields[1]),
            parse_optional(fields[2]),
            parse_optional(fields[3]),
            parse_optional(fields[4]),
            parse_optional(fields[5]),
            parse_optional(fields[6]),
            parse_optional(fields[7]),
        });
    }
    return headers;
}

Bytes read_stream(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("cannot open RTMP stream: " + path);
    }
    return Bytes(std::istreambuf_iterator<char>(input),
                 std::istreambuf_iterator<char>());
}

std::vector<FrameSpan> read_frame_spans(const std::string& path) {
    std::ifstream input(path);
    if (!input) {
        throw std::runtime_error("cannot open TCP frame map: " + path);
    }
    std::string line;
    std::getline(input, line);
    std::vector<FrameSpan> spans;
    while (std::getline(input, line)) {
        if (line.empty()) {
            continue;
        }
        std::istringstream row(line);
        FrameSpan span;
        if (!(row >> span.start >> span.end >> span.frame)) {
            throw std::runtime_error("invalid TCP frame map row: " + line);
        }
        spans.push_back(span);
    }
    return spans;
}

std::vector<uint32_t> frames_for_range(const std::vector<FrameSpan>& spans,
                                       size_t start, size_t end) {
    std::vector<uint32_t> frames;
    for (const auto& span : spans) {
        if (span.end <= start || span.start >= end) {
            continue;
        }
        if (frames.empty() || frames.back() != span.frame) {
            frames.push_back(span.frame);
        }
    }
    return frames;
}

std::string join_frames(const std::vector<uint32_t>& frames) {
    std::ostringstream output;
    for (size_t index = 0; index < frames.size(); ++index) {
        if (index != 0) {
            output << ',';
        }
        output << frames[index];
    }
    return output.str();
}

std::string expected_value(int64_t value) {
    return value < 0 ? "-" : std::to_string(value);
}

std::string compared_value(int64_t expected, uint64_t actual) {
    return expected_value(expected) + " / " + std::to_string(actual);
}

void write_report(const std::string& path,
                  const std::vector<ExpectedHeader>& expected,
                  const std::vector<ActualHeader>& actual,
                  const std::vector<ChunkRecord>& chunks,
                  size_t stream_size) {
    std::ofstream output(path);
    if (!output) {
        throw std::runtime_error("cannot write comparison report: " + path);
    }

    output << "# RTMP真实抓包对比报告\n\n"
           << "- 方向：`172.30.80.1:51946 -> 172.30.95.121:1935`\n"
           << "- 剥离握手后的RTMP字节数：" << stream_size << "\n"
           << "- Wireshark可对比Chunk Header：" << expected.size() << "条\n"
           << "- ChunkParser物理Chunk：" << chunks.size() << "个\n"
           << "- 表格中的`Wireshark / ChunkParser`用于直接比较双方字段；"
              "`-`表示Wireshark因Header继承未重复显示该字段。\n\n"
           << "## 23个Chunk Header逐字段对比\n\n"
           << "这里只列出Wireshark实际导出独立RTMP Header字段的Chunk。"
              "字段格式为`Wireshark / ChunkParser`。\n\n"
           << "| Chunk | Frame | fmt | CSID | timestamp | delta | length | type | stream | 结果 |\n"
           << "|---:|---:|---|---|---|---|---|---|---|---|\n";

    for (size_t index = 0; index < expected.size(); ++index) {
        const auto& reference = expected[index];
        const auto& parsed = actual[index];
        output << "| " << parsed.chunk_number << " | " << reference.frame << " | "
               << compared_value(reference.fmt, parsed.fmt) << " | "
               << compared_value(reference.csid, parsed.csid) << " | "
               << compared_value(reference.timestamp, parsed.timestamp) << " | "
               << compared_value(reference.timestamp_delta,
                                 parsed.timestamp_delta) << " | "
               << compared_value(reference.message_length,
                                 parsed.message_length) << " | "
               << compared_value(reference.type_id, parsed.type_id) << " | "
               << compared_value(reference.stream_id, parsed.stream_id) << " | "
               << (parsed.matched ? "MATCH" : "DIFF") << " |\n";
    }

    output << "\n## Chunk与Message、TCP Frame对应关系\n\n"
           << "TCP Frame与RTMP Chunk并非一一对应。字节范围为剥离握手后RTMP流中的"
              "左闭右开区间。\n\n"
           << "| Chunk | Message | RTMP字节范围 | fmt | CSID | Header | Payload | TCP Frame | 类型 |\n"
           << "|---:|---:|---:|---:|---:|---:|---:|---|---|\n";
    for (const auto& chunk : chunks) {
        output << "| " << chunk.chunk_number << " | "
               << chunk.message_number << " | [" << chunk.start << ", "
               << chunk.end << ") | " << chunk.fmt << " | " << chunk.csid
               << " | " << chunk.header_length << " | "
               << chunk.payload_length << " | " << join_frames(chunk.frames)
               << " | " << (chunk.message_start ? "首Chunk" : "延续Chunk");
        if (chunk.frames.size() > 1) {
            output << "，跨Frame";
        }
        output << " |\n";
    }
}

void feed(ChunkParser& parser, const Bytes& data) {
    parser.receive_buffer.chunk_buffer = data;
    parser.receive_buffer.length = data.size();
    parser.receive_buffer.parse_pos = 0;
    parser.receive_buffer.capacity = data.size();
}

bool compare_field(const char* name, int64_t expected, uint64_t actual,
                   std::vector<std::string>& differences) {
    if (expected < 0 || static_cast<uint64_t>(expected) == actual) {
        return true;
    }
    std::ostringstream message;
    message << name << ": Wireshark=" << expected
            << ", ChunkParser=" << actual;
    differences.push_back(message.str());
    return false;
}

uint32_t read_u32_be(const Bytes& bytes) {
    return (static_cast<uint32_t>(bytes[0]) << 24) |
           (static_cast<uint32_t>(bytes[1]) << 16) |
           (static_cast<uint32_t>(bytes[2]) << 8) |
           static_cast<uint32_t>(bytes[3]);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 5) {
        std::cerr << "usage: real_capture_test RTMP_STREAM WIRESHARK_REFERENCE "
                     "FRAME_MAP REPORT\n";
        return 2;
    }

    try {
        const Bytes stream = read_stream(argv[1]);
        const auto expected = read_reference(argv[2]);
        const auto frame_spans = read_frame_spans(argv[3]);
        ChunkParser parser;
        feed(parser, stream);

        std::unordered_map<uint32_t, MessageBody> messages;
        size_t compared = 0;
        size_t chunks = 0;
        bool all_equal = true;
        std::vector<ActualHeader> actual_headers;
        std::vector<ChunkRecord> chunk_records;

        std::cout << "frame  fmt  csid  timestamp  delta  length  type  stream  result\n";
        while (compared < expected.size()) {
            const size_t chunk_start = parser.receive_buffer.parse_pos;
            const auto result = parser.ChunkParse();
            if (result.status != ChunkParser::ParseStatus::CHUNK_READY) {
                std::cerr << "parser stopped after " << chunks
                          << " chunks at byte offset "
                          << parser.receive_buffer.parse_pos << " with status "
                          << static_cast<int>(result.status) << '\n';
                return 1;
            }
            ++chunks;

            auto& body = messages[result.csid];
            if (result.message_start) {
                body.metadata = result.metadata;
                body.bytes.clear();
                body.message_number = compared + 1;

                const auto& reference = expected[compared];
                const auto& header = parser.current_chunk_header.at(result.csid);
                std::vector<std::string> differences;
                compare_field("fmt", reference.fmt, header.fmt, differences);
                compare_field("csid", reference.csid, result.csid, differences);
                compare_field("timestamp", reference.timestamp,
                              result.metadata.timestamp, differences);
                compare_field("timestamp_delta", reference.timestamp_delta,
                              header.timestamp_delta, differences);
                compare_field("message_length", reference.message_length,
                              result.metadata.message_length, differences);
                compare_field("type_id", reference.type_id,
                              result.metadata.message_type_id, differences);
                compare_field("stream_id", reference.stream_id,
                              result.metadata.message_stream_id, differences);

                std::cout << std::setw(5) << reference.frame << "  "
                          << std::setw(3) << static_cast<unsigned>(header.fmt) << "  "
                          << std::setw(4) << result.csid << "  "
                          << std::setw(9) << result.metadata.timestamp << "  "
                          << std::setw(5) << header.timestamp_delta << "  "
                          << std::setw(6) << result.metadata.message_length << "  "
                          << std::setw(4)
                          << static_cast<unsigned>(result.metadata.message_type_id)
                          << "  " << std::setw(6)
                          << result.metadata.message_stream_id << "  ";
                if (differences.empty()) {
                    std::cout << "MATCH\n";
                } else {
                    all_equal = false;
                    std::cout << "DIFF\n";
                    for (const auto& difference : differences) {
                        std::cout << "       - " << difference << '\n';
                    }
                }
                actual_headers.push_back(ActualHeader{
                    chunks,
                    header.fmt,
                    result.csid,
                    result.metadata.timestamp,
                    header.timestamp_delta,
                    result.metadata.message_length,
                    result.metadata.message_type_id,
                    result.metadata.message_stream_id,
                    differences.empty(),
                });
                ++compared;
            }

            const size_t body_end = result.body_pos + result.body_length;
            if (body_end > parser.receive_buffer.length) {
                throw std::runtime_error("ChunkParser returned an invalid payload range");
            }
            const auto& parsed_header =
                parser.current_chunk_header.at(result.csid);
            chunk_records.push_back(ChunkRecord{
                chunks,
                body.message_number,
                chunk_start,
                body_end,
                result.body_pos - chunk_start,
                result.body_length,
                parsed_header.fmt,
                result.csid,
                result.message_start,
                frames_for_range(frame_spans, chunk_start, body_end),
            });
            if (chunk_records.back().frames.empty()) {
                throw std::runtime_error("physical Chunk has no source TCP Frame");
            }
            body.bytes.insert(
                body.bytes.end(),
                parser.receive_buffer.chunk_buffer.begin() +
                    static_cast<std::ptrdiff_t>(result.body_pos),
                parser.receive_buffer.chunk_buffer.begin() +
                    static_cast<std::ptrdiff_t>(body_end));

            if (body.bytes.size() == body.metadata.message_length &&
                body.metadata.message_type_id == 1) {
                if (body.bytes.size() != 4 ||
                    !parser.SetChunkSize(read_u32_be(body.bytes))) {
                    throw std::runtime_error("invalid Set Chunk Size message in capture");
                }
            }
        }

        std::cout << "compared " << compared << '/' << expected.size()
                  << " Wireshark Chunk Headers across " << chunks
                  << " physical chunks\n";
        write_report(argv[4], expected, actual_headers, chunk_records,
                     stream.size());
        std::cout << "wrote comparison report to " << argv[4] << '\n';
        return all_equal ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << "real capture test failed: " << error.what() << '\n';
        return 1;
    }
}
