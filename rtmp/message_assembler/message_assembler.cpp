#include "message_assembler.h"

/* Class MessageAssembler */

/* Add a Fragment */
MessageAssembler::BufferAppendStatus MessageAssembler::AddFragment(MessageBuffer& message_buffer, const uint8_t* ptr, size_t length) {
    size_t current_size = message_buffer.buffer.size();
    if(current_size + length > message_buffer.metadata.message_length) {
        return BufferAppendStatus::MESSAGE_ERROR;
    }//robustness
    
    /* Add to buffer */
    if(length > 0) {
        message_buffer.buffer.insert(message_buffer.buffer.end(), ptr, ptr + length);
    }

    /* Check current reception status */
    current_size = message_buffer.buffer.size();
    if(current_size == message_buffer.metadata.message_length) {
        return BufferAppendStatus::MESSAGE_FINISHED;
    } else {
        return BufferAppendStatus::APPEND_SUCCESS;
    }
}

/* Append to buffer */
MessageAssembler::BufferAppendStatus MessageAssembler::Append(MessageChunk message_chunk) {
    if(message_chunk.payload == nullptr && message_chunk.payload_length > 0) {
        return BufferAppendStatus::APPEND_ERROR;
    }//robustness
    
    if (message_chunk.metadata.message_length > 0 &&
      message_chunk.payload_length == 0) {
      return BufferAppendStatus::MESSAGE_ERROR;
    }//robustness

    if (message_chunk.metadata.message_length == 0 &&
      message_chunk.payload_length > 0) {
      return BufferAppendStatus::MESSAGE_ERROR;
    }//robustness
    
    /* new message */
    if(message_chunk.message_start) {
        //If it is the start of a new message, create a new buffer for this message
        auto [iterator, success] = buffers.try_emplace(message_chunk.csid, message_chunk.metadata.message_length);
        if(!success) {
            return BufferAppendStatus::MESSAGE_ERROR;
        }

        /* Add message header */
        iterator->second.metadata = message_chunk.metadata;
    }
    
    if(buffers.find(message_chunk.csid) == buffers.end()) {
        return BufferAppendStatus::APPEND_ERROR;
    }//robustness, because of the detection module above, there should be corresponding buffers in all circumstances.

    /* Append data to buffer */
    return AddFragment(buffers[message_chunk.csid], message_chunk.payload, message_chunk.payload_length);
}

MessageAssembler::AssembleResult MessageAssembler::AssembleMessage(uint32_t csid) {
    if(this->buffers.find(csid) == this->buffers.end()) {
        return {
            AssembleStatus::PROTOCOL_ERROR,
            nullptr
        };
    };//robustness
    
    if(this->buffers[csid].buffer.size() < this->buffers[csid].metadata.message_length) {
        return {
            AssembleStatus::NEED_MORE_DATA,
            nullptr
        };
    };//robustness

    if(this->buffers[csid].buffer.size() > this->buffers[csid].metadata.message_length) {
        return {
            AssembleStatus::PROTOCOL_ERROR,
            nullptr
        };
    };//robustness

    /* assmeble message as RtmpMessage */
    auto message = std::make_shared<RtmpMessage>();
    message->metadata = this->buffers[csid].metadata;
    message->payload = std::move(this->buffers[csid].buffer);

    /* Delete the old cache */
    this->buffers.erase(csid);

    return {
        AssembleStatus::ASSEMBLE_SUCCESS,
        std::move(message)
    };

}

/* Constructor function */
MessageAssembler::MessageAssembler() : buffers() {
}

/* Class MessageBuffer */
MessageBuffer::MessageBuffer(size_t message_length)
    : buffer(0) {
        buffer.reserve(message_length);
}