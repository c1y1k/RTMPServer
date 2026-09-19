#include "../../message_assembler.h"

#include <cstdint>
#include <exception>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using Bytes = std::vector<uint8_t>;
using AppendStatus = MessageAssembler::BufferAppendStatus;
using AssembleStatus = MessageAssembler::AssembleStatus;

void expect(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

MessageMetadata make_metadata(uint32_t length,
                              uint32_t timestamp = 100,
                              uint8_t type_id = 9,
                              uint32_t stream_id = 1) {
    return {timestamp, length, type_id, stream_id};
}

MessageChunk make_chunk(uint32_t csid,
                        bool message_start,
                        const MessageMetadata& metadata,
                        const Bytes& payload) {
    MessageChunk chunk;
    chunk.csid = csid;
    chunk.message_start = message_start;
    chunk.metadata = metadata;
    chunk.payload = payload.empty() ? nullptr : payload.data();
    chunk.payload_length = payload.size();
    return chunk;
}

void expect_metadata(const MessageMetadata& actual,
                     const MessageMetadata& expected,
                     const std::string& context) {
    expect(actual.timestamp == expected.timestamp,
           context + ": wrong timestamp");
    expect(actual.message_length == expected.message_length,
           context + ": wrong message length");
    expect(actual.message_type_id == expected.message_type_id,
           context + ": wrong message type ID");
    expect(actual.message_stream_id == expected.message_stream_id,
           context + ": wrong message stream ID");
}

void test_single_chunk_message() {
    MessageAssembler assembler;
    const MessageMetadata metadata = make_metadata(3, 123, 8, 7);
    const Bytes payload{'a', 'b', 'c'};

    expect(assembler.Append(make_chunk(3, true, metadata, payload)) ==
               AppendStatus::MESSAGE_FINISHED,
           "single chunk did not finish");

    const auto result = assembler.AssembleMessage(3);
    expect(result.status == AssembleStatus::ASSEMBLE_SUCCESS,
           "single chunk assembly failed");
    expect(result.message_ptr != nullptr,
           "single chunk returned a null message");
    expect_metadata(result.message_ptr->metadata, metadata, "single chunk");
    expect(result.message_ptr->payload == payload,
           "single chunk payload mismatch");
}

void test_multi_chunk_and_need_more_data() {
    MessageAssembler assembler;
    const MessageMetadata metadata = make_metadata(5);
    const Bytes first{'a', 'b', 'c'};
    const Bytes second{'d', 'e'};

    expect(assembler.Append(make_chunk(4, true, metadata, first)) ==
               AppendStatus::APPEND_SUCCESS,
           "first fragment returned the wrong status");

    auto result = assembler.AssembleMessage(4);
    expect(result.status == AssembleStatus::NEED_MORE_DATA,
           "incomplete message did not request more data");
    expect(result.message_ptr == nullptr,
           "incomplete message unexpectedly returned a message");

    expect(assembler.Append(make_chunk(4, false, metadata, second)) ==
               AppendStatus::MESSAGE_FINISHED,
           "final fragment did not finish the message");

    result = assembler.AssembleMessage(4);
    expect(result.status == AssembleStatus::ASSEMBLE_SUCCESS,
           "multi-chunk assembly failed");
    expect(result.message_ptr->payload == Bytes({'a', 'b', 'c', 'd', 'e'}),
           "multi-chunk payload mismatch");
}

void test_interleaved_csids() {
    MessageAssembler assembler;
    const MessageMetadata metadata3 = make_metadata(4, 10, 8, 1);
    const MessageMetadata metadata5 = make_metadata(3, 20, 9, 2);

    expect(assembler.Append(make_chunk(3, true, metadata3, Bytes{'a', 'b'})) ==
               AppendStatus::APPEND_SUCCESS,
           "CSID 3 first fragment failed");
    expect(assembler.Append(make_chunk(5, true, metadata5, Bytes{'x'})) ==
               AppendStatus::APPEND_SUCCESS,
           "CSID 5 first fragment failed");
    expect(assembler.Append(make_chunk(3, false, metadata3, Bytes{'c', 'd'})) ==
               AppendStatus::MESSAGE_FINISHED,
           "CSID 3 final fragment failed");
    expect(assembler.Append(make_chunk(5, false, metadata5, Bytes{'y', 'z'})) ==
               AppendStatus::MESSAGE_FINISHED,
           "CSID 5 final fragment failed");

    const auto result5 = assembler.AssembleMessage(5);
    const auto result3 = assembler.AssembleMessage(3);
    expect(result3.message_ptr->payload == Bytes({'a', 'b', 'c', 'd'}),
           "CSID 3 payload was polluted");
    expect(result5.message_ptr->payload == Bytes({'x', 'y', 'z'}),
           "CSID 5 payload was polluted");
    expect_metadata(result3.message_ptr->metadata, metadata3, "CSID 3");
    expect_metadata(result5.message_ptr->metadata, metadata5, "CSID 5");
}

void test_same_csid_reuse_after_assembly() {
    MessageAssembler assembler;
    const MessageMetadata first_metadata = make_metadata(1, 1);
    const MessageMetadata second_metadata = make_metadata(2, 2);

    expect(assembler.Append(make_chunk(6, true, first_metadata, Bytes{'a'})) ==
               AppendStatus::MESSAGE_FINISHED,
           "first message did not finish");
    expect(assembler.AssembleMessage(6).status ==
               AssembleStatus::ASSEMBLE_SUCCESS,
           "first message assembly failed");
    expect(assembler.buffers.find(6) == assembler.buffers.end(),
           "completed CSID state was not erased");

    expect(assembler.Append(
               make_chunk(6, true, second_metadata, Bytes{'b', 'c'})) ==
               AppendStatus::MESSAGE_FINISHED,
           "reused CSID could not start a new message");
    const auto result = assembler.AssembleMessage(6);
    expect(result.message_ptr->payload == Bytes({'b', 'c'}),
           "reused CSID returned the wrong payload");
    expect_metadata(result.message_ptr->metadata,
                    second_metadata,
                    "reused CSID");
}

void test_zero_length_message() {
    MessageAssembler assembler;
    const MessageMetadata metadata = make_metadata(0);
    MessageChunk chunk;
    chunk.csid = 7;
    chunk.message_start = true;
    chunk.metadata = metadata;

    expect(assembler.Append(chunk) == AppendStatus::MESSAGE_FINISHED,
           "zero-length message did not finish");
    const auto result = assembler.AssembleMessage(7);
    expect(result.status == AssembleStatus::ASSEMBLE_SUCCESS,
           "zero-length message assembly failed");
    expect(result.message_ptr != nullptr && result.message_ptr->payload.empty(),
           "zero-length message returned a non-empty payload");
}

void test_invalid_pointer_and_length_combinations() {
    MessageAssembler assembler;

    MessageChunk null_payload;
    null_payload.csid = 3;
    null_payload.message_start = true;
    null_payload.metadata = make_metadata(1);
    null_payload.payload = nullptr;
    null_payload.payload_length = 1;
    expect(assembler.Append(null_payload) == AppendStatus::APPEND_ERROR,
           "null non-empty payload was accepted");

    MessageChunk empty_fragment;
    empty_fragment.csid = 4;
    empty_fragment.message_start = true;
    empty_fragment.metadata = make_metadata(1);
    expect(assembler.Append(empty_fragment) == AppendStatus::MESSAGE_ERROR,
           "empty fragment for a non-empty message was accepted");

    const Bytes one_byte{'x'};
    MessageChunk data_for_empty =
        make_chunk(5, true, make_metadata(0), one_byte);
    expect(assembler.Append(data_for_empty) == AppendStatus::MESSAGE_ERROR,
           "payload for a zero-length message was accepted");
}

void test_continuation_without_start() {
    MessageAssembler assembler;
    expect(assembler.Append(
               make_chunk(9, false, make_metadata(1), Bytes{'a'})) ==
               AppendStatus::APPEND_ERROR,
           "continuation without a message start was accepted");
}

void test_duplicate_message_start() {
    MessageAssembler assembler;
    const MessageMetadata metadata = make_metadata(3);

    expect(assembler.Append(make_chunk(10, true, metadata, Bytes{'a'})) ==
               AppendStatus::APPEND_SUCCESS,
           "initial message start failed");
    expect(assembler.Append(make_chunk(10, true, metadata, Bytes{'x'})) ==
               AppendStatus::MESSAGE_ERROR,
           "duplicate message start was accepted");
    expect(assembler.Append(make_chunk(10, false, metadata, Bytes{'b', 'c'})) ==
               AppendStatus::MESSAGE_FINISHED,
           "duplicate start polluted the original message");
    expect(assembler.AssembleMessage(10).message_ptr->payload ==
               Bytes({'a', 'b', 'c'}),
           "duplicate start changed the original payload");
}

void test_oversized_fragment_does_not_pollute_buffer() {
    MessageAssembler assembler;
    const MessageMetadata metadata = make_metadata(5);

    expect(assembler.Append(make_chunk(11, true, metadata, Bytes{'a', 'b', 'c'})) ==
               AppendStatus::APPEND_SUCCESS,
           "valid first fragment failed");
    expect(assembler.Append(make_chunk(11, false, metadata, Bytes{'d', 'e', 'f'})) ==
               AppendStatus::MESSAGE_ERROR,
           "oversized fragment was accepted");
    expect(assembler.Append(make_chunk(11, false, metadata, Bytes{'d', 'e'})) ==
               AppendStatus::MESSAGE_FINISHED,
           "buffer was polluted by the rejected fragment");
    expect(assembler.AssembleMessage(11).message_ptr->payload ==
               Bytes({'a', 'b', 'c', 'd', 'e'}),
           "rejected fragment changed the assembled payload");
}

void test_assemble_missing_csid() {
    MessageAssembler assembler;
    const size_t original_size = assembler.buffers.size();
    const auto result = assembler.AssembleMessage(123);

    expect(result.status == AssembleStatus::PROTOCOL_ERROR,
           "missing CSID returned the wrong status");
    expect(result.message_ptr == nullptr,
           "missing CSID returned a message");
    expect(assembler.buffers.size() == original_size,
           "missing CSID lookup created buffer state");
}

void test_append_copies_source_payload() {
    MessageAssembler assembler;
    const MessageMetadata metadata = make_metadata(4);
    Bytes source{'a', 'b'};

    expect(assembler.Append(make_chunk(12, true, metadata, source)) ==
               AppendStatus::APPEND_SUCCESS,
           "source-copy first fragment failed");
    source[0] = 'x';
    source[1] = 'y';

    expect(assembler.Append(make_chunk(12, false, metadata, Bytes{'c', 'd'})) ==
               AppendStatus::MESSAGE_FINISHED,
           "source-copy final fragment failed");
    expect(assembler.AssembleMessage(12).message_ptr->payload ==
               Bytes({'a', 'b', 'c', 'd'}),
           "assembler retained or reread the source pointer");
}

}  // namespace

int main() {
    const std::vector<std::pair<std::string, std::function<void()>>> tests{
        {"single chunk message", test_single_chunk_message},
        {"multi-chunk and need more data", test_multi_chunk_and_need_more_data},
        {"interleaved CSIDs", test_interleaved_csids},
        {"same CSID reuse after assembly", test_same_csid_reuse_after_assembly},
        {"zero-length message", test_zero_length_message},
        {"invalid pointer and length combinations",
         test_invalid_pointer_and_length_combinations},
        {"continuation without start", test_continuation_without_start},
        {"duplicate message start", test_duplicate_message_start},
        {"oversized fragment preserves buffer",
         test_oversized_fragment_does_not_pollute_buffer},
        {"assemble missing CSID", test_assemble_missing_csid},
        {"Append copies source payload", test_append_copies_source_payload},
    };

    size_t passed = 0;
    for (const auto& [name, test] : tests) {
        try {
            test();
            ++passed;
            std::cout << "[PASS] " << name << '\n';
        } catch (const std::exception& error) {
            std::cerr << "[FAIL] " << name << ": " << error.what() << '\n';
        }
    }

    std::cout << passed << '/' << tests.size() << " tests passed\n";
    return passed == tests.size() ? 0 : 1;
}
