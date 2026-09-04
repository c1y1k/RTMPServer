#ifndef chunk_parser_h
#define chunk_parser_h

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>
#include <unordered_map>

class ChunkHeader {
public:
    ChunkHeader();

    /*basic chunk header fields*/
    uint8_t fmt;
    uint32_t csid;
    /*message fields*/
    uint32_t timestamp;
    uint32_t timestamp_delta;
    uint32_t message_length;
    uint8_t message_type_id;
    uint32_t message_stream_id;
    /*additional fields*/
    enum ExtendedTimestampFlag {
        NO_EXTENDED_TIMESTAMP,
        HAS_EXTENDED_ABSOLUTE_TIMESTAMP,
        HAS_EXTENDED_DELTA_TIMESTAMP
    } extended_timestamp_flag;
    size_t bytes_remaining;   //Remaining bytes of the message
};


/* ReceiveBuffer, used for chunk parsing */
class ReceiveBuffer {
public:
    std::vector<uint8_t> chunk_buffer;
    size_t length;
    size_t parse_pos;
    size_t capacity, max_cap;//Buffer capacity and maximum capacity, Bytes

public:
    ReceiveBuffer(uint32_t initial_capacity = 4 * 1024);
    ~ReceiveBuffer() = default;

    void append(const char* data, size_t length);
};

/* Results Data */
struct MessageMetadata {
    uint32_t timestamp = 0;
    uint32_t message_length = 0;
    uint8_t message_type_id = 0;
    uint32_t message_stream_id = 0;
};

class ChunkParser {
public:
    /* Parameters data */
    enum class TriggerMode {
        LT,
        ET
    };

    /* Results Data */
    enum class ParseStatus {
        NEED_MORE_DATA,
        CHUNK_READY,
        PROTOCOL_ERROR,
        PARSER_ERROR
    };

    struct ParseResult {
        ParseStatus status;
        size_t body_pos = 0;
        size_t body_length = 0;
        uint32_t csid = 0;

        bool message_start = false;
        MessageMetadata metadata{};
    };

    /* Global chunk buffer */
    ReceiveBuffer receive_buffer;
    uint32_t in_chunk_size;
    std::unordered_map<uint32_t, ChunkHeader> current_chunk_header;

    /* Single chunk parsing buffer */
    uint32_t phase;  //The current parsing stage, 0 basic header, 1 message header, 2 extended timestamp, 3 chunk data
    uint32_t csid;   //The current chunk stream ID
    bool is_first_chunk;
    size_t bytes_remaining;  //The remaining bytes of the current chunk, min(message length - bytes already received, chunk size)

private:


public:
    ChunkParser();
    ~ChunkParser() = default;
    ParseResult ChunkParse();
    std::string Read(int socket_fd, TriggerMode trigger_mode);
    bool SetChunkSize(uint32_t new_chunk_size);
private:
};

#endif
