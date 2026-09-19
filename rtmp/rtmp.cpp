#include "rtmp.h"

#include <vector>
#include <chrono>
#include <cstdint>
#include <cerrno>
#include <cstddef>
#include <sys/random.h>
#include <unistd.h>
#include <string>
#include <cstring>
#include <sys/socket.h>

void write_be32(uint8_t* destination, uint32_t value);
bool fill_random(uint8_t* buffer, size_t length);

/*Class Rtmp*/

/* Constructor */
Rtmp::Rtmp(int socket_fd, TriggerMode trigger_mode) : m_state(Rtmp_state::RTMP_HANDSHAKE_WAIT_C0C1),
    m_trigger_mode(trigger_mode),
    m_socket(socket_fd),
    m_stream_id(0),
    m_rtmp_info() {
}

/* Destructor function */
Rtmp::~Rtmp() {
    // Destructor implementation
    close_rtmp();
}

/* The overall orchestration function */
Rtmp::ProcessResult Rtmp::process() {
      for (;;) {
          // 1. 握手
          if (m_state == Rtmp_state::RTMP_HANDSHAKE_WAIT_C0C1 ||
              m_state == Rtmp_state::RTMP_HANDSHAKE_SEND ||
              m_state == Rtmp_state::RTMP_HANDSHAKE_WAIT_C2) {

              StepStatus step;

              if (m_state == Rtmp_state::RTMP_HANDSHAKE_WAIT_C0C1) {
                  step = handshake1();
              } else if (m_state == Rtmp_state::RTMP_HANDSHAKE_SEND) {
                  step = send_handshake_response();
              } else {
                  step = handshake2();
              }

              if (step == StepStatus::Progress) {
                  continue;
              }

              return {step, ProcessStage::Handshake};
          }

          // 2. 先解析 ChunkParser 中已有的数据
          auto parsed = m_chunk_parser.ChunkParse();

          if (parsed.status == ChunkParser::ParseStatus::CHUNK_READY) {
              const uint8_t* payload = m_chunk_parser.GetPayloadPtr(
                  parsed.body_pos, parsed.body_length
              );

              if (parsed.body_length > 0 && payload == nullptr) {
                  return {StepStatus::InternalError,
                          ProcessStage::ChunkParse};
              }

              MessageChunk fragment;
              fragment.csid = parsed.csid;
              fragment.message_start = parsed.message_start;
              fragment.metadata = {
                  parsed.metadata.timestamp,
                  parsed.metadata.message_length,
                  parsed.metadata.message_type_id,
                  parsed.metadata.message_stream_id
              };
              fragment.payload = payload;
              fragment.payload_length = parsed.body_length;

              // Append 会复制 Payload；必须在下一次 Read 前调用
              auto appended = m_message_assembler.Append(fragment);

              if (appended ==
                  MessageAssembler::BufferAppendStatus::APPEND_ERROR) {
                  return {StepStatus::InternalError,
                          ProcessStage::MessageAssemble};
              }

              if (appended ==
                  MessageAssembler::BufferAppendStatus::MESSAGE_ERROR) {
                  return {StepStatus::ProtocolError,
                          ProcessStage::MessageAssemble};
              }

              if (appended ==
                  MessageAssembler::BufferAppendStatus::MESSAGE_FINISHED) {
                  auto assembled =
                      m_message_assembler.AssembleMessage(parsed.csid);

                  if (assembled.status ==
                      MessageAssembler::AssembleStatus::PROTOCOL_ERROR) {
                      return {StepStatus::ProtocolError,
                              ProcessStage::MessageAssemble};
                  }

                  if (assembled.status !=
                          MessageAssembler::AssembleStatus::ASSEMBLE_SUCCESS ||
                      !assembled.message_ptr) {
                      return {StepStatus::InternalError,
                              ProcessStage::MessageAssemble};
                  }

                  return {StepStatus::Progress,
                    ProcessStage::MessageAssemble,
                    0,
                    assembled.message_ptr};
              }

              // 当前 Chunk 已消费，尝试解析缓存中的下一个
              continue;
          }

          if (parsed.status == ChunkParser::ParseStatus::PROTOCOL_ERROR) {
              return {StepStatus::ProtocolError,
                      ProcessStage::ChunkParse};
          }

          if (parsed.status == ChunkParser::ParseStatus::PARSER_ERROR) {
              return {StepStatus::InternalError,
                      ProcessStage::ChunkParse};
          }

          // 3. NEED_MORE_DATA：现有缓存不足，才从 socket 读取
          auto& buffer = m_chunk_parser.receive_buffer;
          const size_t before = buffer.length - buffer.parse_pos;

          const std::string read_status =
              m_chunk_parser.Read(m_socket, m_trigger_mode);
          const int read_errno = errno;

          const size_t after = buffer.length - buffer.parse_pos;

          if (read_status == "error") {
              return {StepStatus::SystemError,
                      ProcessStage::ChunkRead, read_errno};
          }

          // Read 可能先读到数据，然后检测到对端关闭或缓冲区满；
          // 这种情况先处理已经进入缓存的数据。
          if (after > before) {
              continue;
          }

          if (read_status == "close") {
              return {StepStatus::PeerClosed,
                      ProcessStage::ChunkRead};
          }

          if (read_status == "buffer_full") {
              return {StepStatus::ResourceLimit,
                      ProcessStage::ChunkRead};
          }

          if (read_status == "success") {
              return {StepStatus::NeedRead,
                      ProcessStage::ChunkRead};
          }

          return {StepStatus::InternalError,
                  ProcessStage::ChunkRead};
      }
  }

/* Handshake business function 1, wait for C0 and C1, Send S0 and S1 and S2 */
Rtmp::StepStatus Rtmp::handshake1() {
    /* Read from the tcp buffer into the user buffer */
    ssize_t read_result = 0;

    if(m_rtmp_info.receive_bytes < 1537) {
        if(m_trigger_mode == TriggerMode::LT) {
             
            do{
                read_result = read(m_socket, m_rtmp_info.receive_buffer.data() + m_rtmp_info.receive_bytes, 1537 - m_rtmp_info.receive_bytes);
            }while(read_result == -1 && errno == EINTR);
            
            if(read_result == 0) {
                return StepStatus::Error; // Connection closed
            } else if (read_result < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    return StepStatus::NeedRead; // Need to read more data
                } else {
                    return StepStatus::Error; // Read error
                }
            }else{
                m_rtmp_info.receive_bytes += static_cast<size_t>(read_result);
            }

        } else {
            
            while (m_rtmp_info.receive_bytes < 1537) {
                
                do{
                    read_result = read(m_socket, m_rtmp_info.receive_buffer.data() + m_rtmp_info.receive_bytes, 1537 - m_rtmp_info.receive_bytes);
                }while(read_result == -1 && errno == EINTR);
                
                if(read_result == 0) {
                    return StepStatus::Error; // Connection closed
                }else if(read_result == -1) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) {
                        return StepStatus::NeedRead;
                    }else{
                        return StepStatus::Error; // Read error
                    }
                }else{
                    m_rtmp_info.receive_bytes += static_cast<size_t>(read_result);
                }
        
            }
        
        }
    }
    
    /* Analyse and Cache C0C1 */
    if(m_rtmp_info.receive_bytes == 1537){//Length check
        /* C0 */
        m_rtmp_info.version = static_cast<uint8_t>(m_rtmp_info.receive_buffer[0]);
        if(m_rtmp_info.version != 3){
            return StepStatus::Error;
        }//Version verification

        /* C1 */
        std::memcpy(m_rtmp_info.c1_time.data(), m_rtmp_info.receive_buffer.data() + 1, 4);
        uint32_t c1_receive_time = static_cast<uint32_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()
            ).count()
        );
        write_be32(m_rtmp_info.c1_receive_time.data(), c1_receive_time);

        uint32_t c1_Zero = 0;
        std::memcpy(&c1_Zero, m_rtmp_info.receive_buffer.data() + 5, 4);
        if(c1_Zero != 0){
            return StepStatus::Error;
        }//robustness

        std::memcpy(m_rtmp_info.c1_random.data(), m_rtmp_info.receive_buffer.data() + 9, 1528);

        /* Transition to next state after receiving C0 and C1 */
        m_state = Rtmp_state::RTMP_HANDSHAKE_SEND;

        return StepStatus::Progress;
    }else{
        return StepStatus::NeedRead;
    }
}

/* This could involve sending S0, S1, and S2 */
Rtmp::StepStatus Rtmp::send_handshake_response() {
    /* Returns S0 + S1 + S2 */

    /* S0 */
    if(m_rtmp_info.processed_bytes < 1 ) {
        
        size_t bytes_sent = 0;
        ssize_t send_result = send_buffer(m_socket, &m_rtmp_info.s0_version, 1, bytes_sent);
        
        m_rtmp_info.processed_bytes += bytes_sent;

        if (send_result == -EAGAIN) {
        return StepStatus::NeedWrite;
        }

        if(send_result < 0) {
            return StepStatus::Error;
        }

    }

    /*S1*/
    if(m_rtmp_info.processed_bytes < 1 + 4) {
        
        if(m_rtmp_info.processed_bytes == 1) {
            uint32_t s1_time = static_cast<uint32_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()
                ).count()
            );

            write_be32(m_rtmp_info.s1_time.data(), s1_time);
        }

        size_t bytes_sent = 0;
        ssize_t send_result = send_buffer(m_socket, m_rtmp_info.s1_time.data() + (m_rtmp_info.processed_bytes - 1), 1 + 4 - m_rtmp_info.processed_bytes, bytes_sent);
        
        m_rtmp_info.processed_bytes += bytes_sent;

        if (send_result == -EAGAIN) {
        return StepStatus::NeedWrite;
        }

        if(send_result < 0) {
            return StepStatus::Error;
        }

    }

    if(m_rtmp_info.processed_bytes < 1 + 4 + 4) {

        std::vector<uint8_t> s1_zero(4, 0);

        size_t bytes_sent = 0;
        ssize_t send_result = send_buffer(m_socket, s1_zero.data() + (m_rtmp_info.processed_bytes - 1 - 4), 1 + 4 + 4 - m_rtmp_info.processed_bytes, bytes_sent);
        
        m_rtmp_info.processed_bytes += bytes_sent;

        if (send_result == -EAGAIN) {
        return StepStatus::NeedWrite;
        }

        if(send_result < 0) {
            return StepStatus::Error;
        }

    }

    if(m_rtmp_info.processed_bytes < 1 + 4 + 4 + 1528) {

        if(m_rtmp_info.processed_bytes == 1 + 4 + 4) {
            bool rand_flag = fill_random(m_rtmp_info.s1_random.data(), m_rtmp_info.s1_random.size());
            if(!rand_flag){
                return StepStatus::Error;
            }
        }

        size_t bytes_sent = 0;
        ssize_t send_result = send_buffer(m_socket, m_rtmp_info.s1_random.data() + (m_rtmp_info.processed_bytes - 1 - 4 - 4), 1 + 4 + 4 + 1528 - m_rtmp_info.processed_bytes, bytes_sent);
        
        m_rtmp_info.processed_bytes += bytes_sent;

        if (send_result == -EAGAIN) {
        return StepStatus::NeedWrite;
        }

        if(send_result < 0) {
            return StepStatus::Error;
        }

    }
    
    /* S2 */
    if(m_rtmp_info.processed_bytes < 1 + 4 + 4 + 1528 + 4) {
        
        std::vector<uint8_t> s2_time1 = m_rtmp_info.c1_time;

        size_t bytes_sent = 0;
        ssize_t send_result = send_buffer(m_socket, s2_time1.data() + (m_rtmp_info.processed_bytes - 1 - 4 - 4 - 1528), 1 + 4 + 4 + 1528 + 4 - m_rtmp_info.processed_bytes, bytes_sent);
        
        m_rtmp_info.processed_bytes += bytes_sent;

        if (send_result == -EAGAIN) {
        return StepStatus::NeedWrite;
        }

        if(send_result < 0) {
            return StepStatus::Error;
        }
    
    }

    if(m_rtmp_info.processed_bytes < 1 + 4 + 4 + 1528 + 4 + 4) {
        
        std::vector<uint8_t> s2_time2 = m_rtmp_info.c1_receive_time;

        size_t bytes_sent = 0;
        ssize_t send_result = send_buffer(m_socket, s2_time2.data() + (m_rtmp_info.processed_bytes - 1 - 4 - 4 - 1528 - 4), 1 + 4 + 4 + 1528 + 4 + 4 - m_rtmp_info.processed_bytes, bytes_sent);
        
        m_rtmp_info.processed_bytes += bytes_sent;

        if (send_result == -EAGAIN) {
        return StepStatus::NeedWrite;
        }

        if(send_result < 0) {
            return StepStatus::Error;
        }
    
    }

    if(m_rtmp_info.processed_bytes < 1 + 4 + 4 + 1528 + 4 + 4 + 1528) {
        
        size_t bytes_sent = 0;
        ssize_t send_result = send_buffer(m_socket, m_rtmp_info.c1_random.data() + (m_rtmp_info.processed_bytes - 1 - 4 - 4 - 1528 - 4 - 4), 1 + 4 + 4 + 1528 + 4 + 4 + 1528 - m_rtmp_info.processed_bytes, bytes_sent);
        
        m_rtmp_info.processed_bytes += bytes_sent;

        if (send_result == -EAGAIN) {
        return StepStatus::NeedWrite;
        }

        if(send_result < 0) {
            return StepStatus::Error;
        }
    
    }

    /* Transition to next state after sending handshake response */
    m_state = Rtmp_state::RTMP_HANDSHAKE_WAIT_C2;

    return StepStatus::Progress;
}

/* Segmented transmission */
ssize_t Rtmp::send_buffer(
      int socket_fd,
      const uint8_t* data,
      size_t length,
      size_t& bytes_sent
  )
  {
      bytes_sent = 0;

      // 空数据视为发送完成
      if (length == 0) {
          return 0;
      }

      while (bytes_sent < length) {
          ssize_t result = send(
              socket_fd,
              data + bytes_sent,
              length - bytes_sent,
              MSG_NOSIGNAL
          );

          if (result > 0) {
              bytes_sent += static_cast<size_t>(result);
              continue;
          }

          if (result == 0) {
              // 非零长度发送却没有发送任何数据，避免死循环
              return -EIO;
          }

          if (errno == EINTR) {
              continue;
          }

          if (errno == EAGAIN || errno == EWOULDBLOCK) {
              return -EAGAIN;
          }

          return -errno;
      }

      // 当前传入的数据已经全部发送完成
      return 0;
  }

/* Handshake business function 2, wait for C2 */
Rtmp::StepStatus Rtmp::handshake2() {
    /* Read from the tcp buffer into the user buffer */
    ssize_t read_result = 0;

    if(m_rtmp_info.receive_bytes < 3073) {
        if(m_trigger_mode == TriggerMode::LT) {
             
            do{
                read_result = read(m_socket, m_rtmp_info.receive_buffer.data() + m_rtmp_info.receive_bytes, 3073 - m_rtmp_info.receive_bytes);
            }while(read_result == -1 && errno == EINTR);
            
            if(read_result == 0) {
                return StepStatus::Error; // Connection closed
            } else if (read_result < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    return StepStatus::NeedRead; // Need to read more data
                } else {
                    return StepStatus::Error; // Read error
                }
            }else{
                m_rtmp_info.receive_bytes += static_cast<size_t>(read_result);
            }

        } else {
            
            while (m_rtmp_info.receive_bytes < 3073) {
                
                do{
                    read_result = read(m_socket, m_rtmp_info.receive_buffer.data() + m_rtmp_info.receive_bytes, 3073 - m_rtmp_info.receive_bytes);
                }while(read_result == -1 && errno == EINTR);
                
                if(read_result == 0) {
                    return StepStatus::Error; // Connection closed
                }else if(read_result == -1) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) {
                        return StepStatus::NeedRead;
                    }else{
                        return StepStatus::Error; // Read error
                    }
                }else{
                    m_rtmp_info.receive_bytes += static_cast<size_t>(read_result);
                }
        
            }
        
        }
    }
    
    /* Analyse and Cache C2 */
    if(m_rtmp_info.receive_bytes == 3073){//Length check

        std::memcpy(m_rtmp_info.c2_time1.data(), m_rtmp_info.receive_buffer.data() + 1537, 4);//c2 time1

        if(m_rtmp_info.c2_time1 != m_rtmp_info.s1_time){
            return StepStatus::Error;
        }

        std::memcpy(m_rtmp_info.c2_time2.data(), m_rtmp_info.receive_buffer.data() + 1541, 4);//c2 time2

        std::memcpy(m_rtmp_info.c2_random.data(), m_rtmp_info.receive_buffer.data() + 1545, 1528);//c2 random

        if(m_rtmp_info.c2_random != m_rtmp_info.s1_random){
            return StepStatus::Error;
        }

        /* Transition to next state after receiving C0 and C1 */
        m_state = Rtmp_state::RTMP_CONNECT_WAIT_CONNECT;

        return StepStatus::Progress;
    }else{
        return StepStatus::NeedRead;
    }

}

bool fill_random(uint8_t* buffer, size_t length)
  {
      size_t offset = 0;

      while (offset < length)
      {
          ssize_t result = getrandom(
              buffer + offset,
              length - offset,
              0
          );

          if (result > 0)
          {
              offset += static_cast<size_t>(result);
              continue;
          }

          if (result == -1 && errno == EINTR)
              continue;

          return false;
      }

      return true;
  }

void write_be32(uint8_t* destination, uint32_t value)
  {
      destination[0] =
          static_cast<uint8_t>((value >> 24) & 0xff);

      destination[1] =
          static_cast<uint8_t>((value >> 16) & 0xff);

      destination[2] =
          static_cast<uint8_t>((value >> 8) & 0xff);

      destination[3] =
          static_cast<uint8_t>(value & 0xff);
  }


/*Establish connection business function*/
Rtmp::StepStatus Rtmp::connect() {
    // Implement the RTMP connect process
    // Wait for connect command and send response
    // Additional connect logic
    switch (m_state) {
        case Rtmp_state::RTMP_CONNECT_WAIT_CONNECT:
            /* Wait for connect command */
            //...
            m_state = Rtmp_state::RTMP_STREAM_WAIT_CREATE_STREAM; // Transition to next state after successful connect
            return StepStatus::Progress;
            break;
        default:
            return StepStatus::Error;
            break;
    }
}

/*Establish the flow business function*/
Rtmp::StepStatus Rtmp::create_stream() {
    // Implement the RTMP create stream process
    // Wait for createStream command and wait for response
    // Additional create stream logic
    switch (m_state) {
        case Rtmp_state::RTMP_STREAM_WAIT_CREATE_STREAM:
            /* Wait for createStream command */
            //...
            m_state = Rtmp_state::RTMP_STREAM_WAIT_PUBLISH_PLAY; // Transition to next state after successful create stream
            return StepStatus::Progress;
            break;
        default:
            return StepStatus::Error;
            break;
    }
}

/*Push-pull flow selection function*/
Rtmp::StepStatus Rtmp::select_role() {
    // Implement the logic to select between publishing and playing
    // This could be based on user input or some other criteria
    // Additional role selection logic
    switch (m_state) {
        case Rtmp_state::RTMP_STREAM_WAIT_PUBLISH_PLAY:
            /* Wait for publish or play command */
            //...
            if(1){
                m_state = Rtmp_state::RTMP_STREAM_PUBLISHING; // Transition to publishing state after successful publish
                return StepStatus::Progress;
            }else if(1){
                m_state = Rtmp_state::RTMP_STREAM_PLAYING; // Transition to playing state after successful play
                return StepStatus::Progress;
            }
            break;
        default:
            return StepStatus::Error;
            break;
    }
}

/*Streaming source streaming business function*/
Rtmp::StepStatus Rtmp::publish() {
    // Implement the RTMP publish process
    // Wait for publish command and stream data
    // Additional publish logic
    return StepStatus::Progress;
}

/*Pulling-end pulling-stream business function*/
Rtmp::StepStatus Rtmp::play() {
    // Implement the RTMP play process
    // Wait for play command and receive stream data
    // Additional play logic
    return StepStatus::Progress;
}

/* Close the socket */
bool Rtmp::close_rtmp() {
    // Close the socket and clean up resources
    if (m_socket != -1) {
        /* Close the socket */
        //...
        close(m_socket);
        m_socket = -1;
        return true;
    }
    return false;
}


