#ifndef MESSAGE_ASSEMBLER
#define MESSAGE_ASSEMBLER

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>
#include <memory>


/* Buffer class for Message assembling */

/* Parameters data */
struct MessageMetadata {
    uint32_t timestamp = 0;
    uint32_t message_length = 0;
    uint8_t message_type_id = 0;
    uint32_t message_stream_id = 0;
};
struct MessageChunk {
    uint32_t csid = 0;
    bool message_start = false;

    MessageMetadata metadata;

    const uint8_t* payload = nullptr;
    size_t payload_length = 0;
};

class MessageBuffer {
public:
    /* buffer */
    MessageMetadata metadata;
    std::vector<uint8_t> buffer;
    
public:
    MessageBuffer(size_t message_length = 0);
};

/* Class for Message assembling */
struct RtmpMessage {
    MessageMetadata metadata;
    std::vector<uint8_t> payload;
};

class MessageAssembler {
public:
    /* result data */
    enum class AssembleStatus {
        ASSEMBLE_SUCCESS = 0,
        NEED_MORE_DATA,
        PROTOCOL_ERROR
    };
    using MessagePtr = std::shared_ptr<const RtmpMessage>;
    struct AssembleResult {
        MessageAssembler::AssembleStatus status = 
            AssembleStatus::NEED_MORE_DATA;
        
        MessagePtr message_ptr = nullptr;
    };
    enum class BufferAppendStatus {
        APPEND_SUCCESS = 0,
        APPEND_ERROR,
        MESSAGE_FINISHED,
        MESSAGE_ERROR
    };

    /* message buffers */
    std::unordered_map<uint32_t, MessageBuffer> buffers; //Map of message buffers, key is the CSID；

public:
    MessageAssembler();
    ~MessageAssembler() = default;

    /* Append to buffer */
    BufferAppendStatus Append(MessageChunk message_chunk);
    
    /* Assemble message */
    AssembleResult AssembleMessage(uint32_t csid);

private:
    /* Append to buffer */
    BufferAppendStatus AddFragment(MessageBuffer& message_buffer, const uint8_t* ptr, size_t length);
};

#endif