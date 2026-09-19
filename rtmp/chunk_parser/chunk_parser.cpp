#include "chunk_parser.h"

#include <algorithm>
#include <cerrno>
#include <unistd.h>

/* Class ChunkParser */

ChunkParser::ChunkParser()
    : receive_buffer(4 * 1024),
    in_chunk_size(128),
    current_chunk_header(),
    phase(0),
    csid(0),
    is_first_chunk(false),
    bytes_remaining(0){
}

/* Read the byte stream from the TCP buffer and store it in the RTMP connection buffer. */
std::string ChunkParser::Read(int socket_fd, TriggerMode trigger_mode) {

    ssize_t bytes_read = 0;
    if(trigger_mode == TriggerMode::ET){
        // Handle ET trigger model
        while(1){
            /* Detect chunk cache */
            if (receive_buffer.length == receive_buffer.capacity) {
                for(size_t i = receive_buffer.parse_pos; i < receive_buffer.length; i++) {
                    receive_buffer.chunk_buffer[i-receive_buffer.parse_pos] = receive_buffer.chunk_buffer[i];
                }
                receive_buffer.length -= receive_buffer.parse_pos;
                receive_buffer.parse_pos = 0;
            }

            /* Adjust chunk cache */
            if (receive_buffer.length == receive_buffer.capacity) {
                if(receive_buffer.capacity >= receive_buffer.max_cap){
                    return "buffer_full";
                }
                receive_buffer.capacity *= 2;
                if (receive_buffer.capacity > receive_buffer.max_cap) {
                    receive_buffer.capacity = receive_buffer.max_cap;
                }
                receive_buffer.chunk_buffer.resize(receive_buffer.capacity);
            }

            /* read */
            bytes_read = read(socket_fd, receive_buffer.chunk_buffer.data() + receive_buffer.length, receive_buffer.capacity - receive_buffer.length);
            if(bytes_read == 0) {//socket closed
                return "close";
            }else if(bytes_read == -1) {
                if(errno == EAGAIN || errno == EWOULDBLOCK){
                    return "success";
                } else if(errno == EINTR) {
                    continue;
                } else {
                    return "error";
                }
            }
            receive_buffer.length += bytes_read;
        }
    }else if(trigger_mode == TriggerMode::LT) {
        /* Detect chunk cache */
        if (receive_buffer.length == receive_buffer.capacity) {
            for(size_t i = receive_buffer.parse_pos; i < receive_buffer.length; i++) {
                receive_buffer.chunk_buffer[i-receive_buffer.parse_pos] = receive_buffer.chunk_buffer[i];
            }
            receive_buffer.length -= receive_buffer.parse_pos;
            receive_buffer.parse_pos = 0;
        }

        /* Adjust chunk cache */
        if (receive_buffer.length == receive_buffer.capacity) {
            if(receive_buffer.capacity >= receive_buffer.max_cap){
                return "buffer_full";
            }
            receive_buffer.capacity *= 2;
            if (receive_buffer.capacity > receive_buffer.max_cap) {
                receive_buffer.capacity = receive_buffer.max_cap;
            }
            receive_buffer.chunk_buffer.resize(receive_buffer.capacity);
        }


        // read
        do {
            bytes_read = read(socket_fd, receive_buffer.chunk_buffer.data() + receive_buffer.length, receive_buffer.capacity - receive_buffer.length);
        }while(bytes_read == -1 && errno == EINTR);

        if(bytes_read == 0) {
            return "close";
        }else if(bytes_read == -1) {
            if(errno == EAGAIN || errno == EWOULDBLOCK) {
                return "success";
            }
            return "error";
        }else{
            receive_buffer.length += bytes_read;
            return "success";
        }
    }else{
        return "error";
    }
}

/* Parse the chunk data in the RTMP connection buffer and return the parsing status and chunk body */
ChunkParser::ParseResult ChunkParser::ChunkParse() {

    /* Parse */
    if(phase == 0) {//Parse basic header
        if(receive_buffer.length - receive_buffer.parse_pos < 1) {
            return {ParseStatus::NEED_MORE_DATA};
        }//robustness
        if(this->bytes_remaining != 0) {
            return {ParseStatus::PROTOCOL_ERROR};
        }//robustness

        this->is_first_chunk = false;

        /* Analyzing CSID */
        uint8_t flag = 0;
        this->csid = static_cast<uint32_t>(receive_buffer.chunk_buffer[receive_buffer.parse_pos] & 0x3F);
        if(this->csid == 0) {
            if(receive_buffer.length - receive_buffer.parse_pos < 2) {
                return {ParseStatus::NEED_MORE_DATA};
            }
            this->csid = static_cast<uint32_t>(receive_buffer.chunk_buffer[receive_buffer.parse_pos + 1]) + 64;
            flag = 1;
        } else if(this->csid == 1) {
            if(receive_buffer.length - receive_buffer.parse_pos < 3) {
                return {ParseStatus::NEED_MORE_DATA};
            }
            this->csid = (static_cast<uint32_t>(receive_buffer.chunk_buffer[receive_buffer.parse_pos + 1]) + 64) + static_cast<uint32_t>(receive_buffer.chunk_buffer[receive_buffer.parse_pos + 2] << 8);
            flag = 2;
        }

        if(current_chunk_header.find(this->csid) == current_chunk_header.end()) {
            if(((receive_buffer.chunk_buffer[receive_buffer.parse_pos] >> 6) & 0x03) != 0) {
                return {ParseStatus::PROTOCOL_ERROR};
            }
            current_chunk_header[this->csid] = ChunkHeader();
        }// Check if there is a preceding header

        /* cache */
        current_chunk_header[csid].fmt = (receive_buffer.chunk_buffer[receive_buffer.parse_pos] >> 6) & 0x03;
        if(current_chunk_header[csid].fmt != 3 && current_chunk_header[csid].bytes_remaining != 0){
            return {ParseStatus::PROTOCOL_ERROR};
        }//robustness
        current_chunk_header[csid].csid = this->csid;


        receive_buffer.parse_pos += static_cast<size_t>(flag) + 1;

        /* next phase */
        this->phase = 1;
    }
    if(phase == 1) {//Parse message header
        uint8_t fmt = current_chunk_header[csid].fmt;

        //Parse message header based on fmt
        if(fmt == 0) {
            if(receive_buffer.length - receive_buffer.parse_pos < 11) {
                return {ParseStatus::NEED_MORE_DATA};
            }//robustness

            /* Parse the message header and cache it */
            /* timestamp and timestamp_delta */
            current_chunk_header[csid].timestamp = (static_cast<uint32_t>(receive_buffer.chunk_buffer[receive_buffer.parse_pos]) << 16) | (static_cast<uint32_t>(receive_buffer.chunk_buffer[receive_buffer.parse_pos + 1]) << 8) | static_cast<uint32_t>(receive_buffer.chunk_buffer[receive_buffer.parse_pos + 2]);
            current_chunk_header[csid].timestamp_delta = current_chunk_header[csid].timestamp;
            if(current_chunk_header[csid].timestamp == 0x00FFFFFF) {
                current_chunk_header[csid].extended_timestamp_flag = ChunkHeader::HAS_EXTENDED_ABSOLUTE_TIMESTAMP;
            } else {
                current_chunk_header[csid].extended_timestamp_flag = ChunkHeader::NO_EXTENDED_TIMESTAMP;
            }

            /* message_length, message_type_id, message_stream_id and message bytes_remaining */
            current_chunk_header[csid].message_length = (static_cast<uint32_t>(receive_buffer.chunk_buffer[receive_buffer.parse_pos + 3]) << 16) | (static_cast<uint32_t>(receive_buffer.chunk_buffer[receive_buffer.parse_pos + 4]) << 8) | static_cast<uint32_t>(receive_buffer.chunk_buffer[receive_buffer.parse_pos + 5]);
            if(current_chunk_header[csid].bytes_remaining == 0) {
                this->is_first_chunk = true;
                current_chunk_header[csid].bytes_remaining = current_chunk_header[csid].message_length;
            }//new message, reset message bytes_remaining
            current_chunk_header[csid].message_type_id = receive_buffer.chunk_buffer[receive_buffer.parse_pos + 6];
            current_chunk_header[csid].message_stream_id = (static_cast<uint32_t>(receive_buffer.chunk_buffer[receive_buffer.parse_pos + 7]) | (static_cast<uint32_t>(receive_buffer.chunk_buffer[receive_buffer.parse_pos + 8]) << 8) | (static_cast<uint32_t>(receive_buffer.chunk_buffer[receive_buffer.parse_pos + 9]) << 16) | (static_cast<uint32_t>(receive_buffer.chunk_buffer[receive_buffer.parse_pos + 10]) << 24));

            receive_buffer.parse_pos += 11;
        } else if(fmt == 1) {
            if(receive_buffer.length - receive_buffer.parse_pos < 7) {
                return {ParseStatus::NEED_MORE_DATA};
            }//robustness

            /* Parse the message header and cache it */
            /* timestamp and timestamp_delta */
            current_chunk_header[csid].timestamp_delta = (static_cast<uint32_t>(receive_buffer.chunk_buffer[receive_buffer.parse_pos]) << 16) | (static_cast<uint32_t>(receive_buffer.chunk_buffer[receive_buffer.parse_pos + 1]) << 8) | static_cast<uint32_t>(receive_buffer.chunk_buffer[receive_buffer.parse_pos + 2]);
            if(current_chunk_header[csid].timestamp_delta != 0x00FFFFFF) {
                current_chunk_header[csid].timestamp = current_chunk_header[csid].timestamp + current_chunk_header[csid].timestamp_delta;
                current_chunk_header[csid].extended_timestamp_flag = ChunkHeader::NO_EXTENDED_TIMESTAMP;
            }else if(current_chunk_header[csid].timestamp_delta == 0x00FFFFFF) {
                current_chunk_header[csid].extended_timestamp_flag = ChunkHeader::HAS_EXTENDED_DELTA_TIMESTAMP;
            }

            /* message_length, message_type_id, and message bytes_remaining */
            current_chunk_header[csid].message_length = (static_cast<uint32_t>(receive_buffer.chunk_buffer[receive_buffer.parse_pos + 3]) << 16) | (static_cast<uint32_t>(receive_buffer.chunk_buffer[receive_buffer.parse_pos + 4]) << 8) |   static_cast<uint32_t>(receive_buffer.chunk_buffer[receive_buffer.parse_pos + 5]);
            if(current_chunk_header[csid].bytes_remaining == 0) {
                this->is_first_chunk = true;
                current_chunk_header[csid].bytes_remaining = current_chunk_header[csid].message_length;
            }
            current_chunk_header[csid].message_type_id = receive_buffer.chunk_buffer[receive_buffer.parse_pos + 6];

            receive_buffer.parse_pos += 7;
        } else if(fmt == 2) {
            if(receive_buffer.length - receive_buffer.parse_pos < 3) {
                return {ParseStatus::NEED_MORE_DATA};
            }//robustness

            /* Parse the message header and cache it */
            /* timestamp and timestamp_delta */
            current_chunk_header[csid].timestamp_delta = (static_cast<uint32_t>(receive_buffer.chunk_buffer[receive_buffer.parse_pos]) << 16) | (static_cast<uint32_t>(receive_buffer.chunk_buffer[receive_buffer.parse_pos + 1]) << 8) | static_cast<uint32_t>(receive_buffer.chunk_buffer[receive_buffer.parse_pos + 2]);
            if(current_chunk_header[csid].timestamp_delta != 0x00FFFFFF) {
                current_chunk_header[csid].timestamp = current_chunk_header[csid].timestamp + current_chunk_header[csid].timestamp_delta;
                current_chunk_header[csid].extended_timestamp_flag = ChunkHeader::NO_EXTENDED_TIMESTAMP;
            }else if(current_chunk_header[csid].timestamp_delta == 0x00FFFFFF) {
                current_chunk_header[csid].extended_timestamp_flag = ChunkHeader::HAS_EXTENDED_DELTA_TIMESTAMP;
            }

            /* message bytes_remaining */
            if(current_chunk_header[csid].bytes_remaining == 0) {
                this->is_first_chunk = true;
                current_chunk_header[csid].bytes_remaining = current_chunk_header[csid].message_length;
            }

            receive_buffer.parse_pos += 3;
        } else if(fmt == 3) {
            /* No message header, use previous chunk header */
            if(current_chunk_header[csid].bytes_remaining == 0){//fmt==3 messaage first chunk
                /* Parse the message header and cache it */
                /* timestamp and timestamp_delta */
                current_chunk_header[csid].timestamp = current_chunk_header[csid].timestamp + current_chunk_header[csid].timestamp_delta;

                /* message bytes_remaining */
                if(current_chunk_header[csid].bytes_remaining == 0) {
                    this->is_first_chunk = true;
                    current_chunk_header[csid].bytes_remaining = current_chunk_header[csid].message_length;
                }else{
                    return {ParseStatus::PROTOCOL_ERROR};
                }
            }else{//Subsequent chunk
            }
        }

        if(current_chunk_header[csid].extended_timestamp_flag != ChunkHeader::NO_EXTENDED_TIMESTAMP) {
            this->phase = 2;
        }else {
            this->phase = 3;
        }
    }
    if(phase == 2) {
            if(receive_buffer.length - receive_buffer.parse_pos < 4) {
                return {ParseStatus::NEED_MORE_DATA};
            }//robustness

        //Parse extended timestamp
        if(current_chunk_header[csid].extended_timestamp_flag == ChunkHeader::HAS_EXTENDED_ABSOLUTE_TIMESTAMP && current_chunk_header[csid].fmt != 3) {
            current_chunk_header[csid].timestamp = (static_cast<uint32_t>(receive_buffer.chunk_buffer[receive_buffer.parse_pos]) << 24) | (static_cast<uint32_t>(receive_buffer.chunk_buffer[receive_buffer.parse_pos + 1]) << 16) | (static_cast<uint32_t>(receive_buffer.chunk_buffer[receive_buffer.parse_pos + 2]) << 8) | static_cast<uint32_t>(receive_buffer.chunk_buffer[receive_buffer.parse_pos + 3]);
            current_chunk_header[csid].timestamp_delta = current_chunk_header[csid].timestamp;
        }else if(current_chunk_header[csid].extended_timestamp_flag == ChunkHeader::HAS_EXTENDED_DELTA_TIMESTAMP && current_chunk_header[csid].fmt != 3) {
            current_chunk_header[csid].timestamp_delta = (static_cast<uint32_t>(receive_buffer.chunk_buffer[receive_buffer.parse_pos]) << 24) | (static_cast<uint32_t>(receive_buffer.chunk_buffer[receive_buffer.parse_pos + 1]) << 16) | (static_cast<uint32_t>(receive_buffer.chunk_buffer[receive_buffer.parse_pos + 2]) << 8) | static_cast<uint32_t>(receive_buffer.chunk_buffer[receive_buffer.parse_pos + 3]);
            current_chunk_header[csid].timestamp = current_chunk_header[csid].timestamp + current_chunk_header[csid].timestamp_delta;
        }
        receive_buffer.parse_pos += 4;

        this->phase = 3;
    }
    if(phase == 3) {
        //Parse chunk data
        this->bytes_remaining = std::min(
            current_chunk_header[this->csid].bytes_remaining,
            static_cast<size_t>(in_chunk_size));

        if(receive_buffer.length - receive_buffer.parse_pos < this->bytes_remaining) {
            return {ParseStatus::NEED_MORE_DATA};
        }

        //Process chunk data here, transfer the message information in the chunk to the message assembler based on the csid
        size_t body_pos = receive_buffer.parse_pos; //Return the position to the chunk data
        size_t body_length = this->bytes_remaining;

        current_chunk_header[this->csid].bytes_remaining -= this->bytes_remaining;
        receive_buffer.parse_pos += this->bytes_remaining;
        this->bytes_remaining = 0;
        phase = 0;

        return {
            ParseStatus::CHUNK_READY,
            body_pos,
            body_length,
            this->csid,
            this->is_first_chunk,
            {
                current_chunk_header[this->csid].timestamp,
                current_chunk_header[this->csid].message_length,
                current_chunk_header[this->csid].message_type_id,
                current_chunk_header[this->csid].message_stream_id,
            }
        };
    }
    return {ParseStatus::PARSER_ERROR};
}

/* Get the chunk payload pointer */
const uint8_t* ChunkParser::GetPayloadPtr(size_t offset, size_t length) const {
    if(offset > receive_buffer.length) {
        return nullptr;
    }//robustness

    if(length > receive_buffer.length - offset) {
        return nullptr;
    }//robustness

    if(length == 0) {
        return nullptr;
    }//robustness

    return receive_buffer.chunk_buffer.data() + offset;
}

/* RTMP connection chunk size setting for receiving data */
bool ChunkParser::SetChunkSize(uint32_t new_chunk_size){
    if(new_chunk_size == 0 || (new_chunk_size & 0x80000000U) != 0) {
        return false;
    }//robustness

    /* Chunk size limit */
    if(new_chunk_size > receive_buffer.max_cap) {
        return false;
    }

    in_chunk_size = new_chunk_size;
    return true;
}

/*Class ReceiveChunk*/
ReceiveBuffer::ReceiveBuffer(uint32_t initial_capacity)
    : chunk_buffer(initial_capacity),
    length(0),
    parse_pos(0),
    capacity(initial_capacity),
    max_cap(1024 * 1024 * 10) {
}

/*Class ChunkHeader*/
ChunkHeader::ChunkHeader()
    : fmt(0),
    csid(0),
    timestamp(0),
    timestamp_delta(0),
    message_length(0),
    message_type_id(0),
    message_stream_id(0),
    extended_timestamp_flag(NO_EXTENDED_TIMESTAMP),
    bytes_remaining(0) {
}
