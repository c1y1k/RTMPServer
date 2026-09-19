#ifndef RTMP_H
#define RTMP_H

#include "chunk_parser.h"
#include "message_assembler.h"

class Rtmp {
public:
    /* state data */
    class RtmpInfo {
    public:
        /* receive */
        std::vector<uint8_t> receive_buffer = std::vector<uint8_t>(3073, 0); //Buffer for receiving C0C1C2
        size_t receive_bytes = 0;//received bytes

        uint8_t version = 0;//c0

        /*c1*/
        std::vector<uint8_t> c1_time = std::vector<uint8_t>(4, 0);//c1 time
        std::vector<uint8_t> c1_receive_time = std::vector<uint8_t>(4, 0);//c1 receive time
        std::vector<uint8_t> c1_random = std::vector<uint8_t>(1528, 0);//c1 random

        /* c2 */
        std::vector<uint8_t> c2_time1 = std::vector<uint8_t>(4, 0);//c2 time1
        std::vector<uint8_t> c2_time2 = std::vector<uint8_t>(4, 0);//c2 time2
        std::vector<uint8_t> c2_random = std::vector<uint8_t>(1528, 0);//c2 random

        /* send response */
        size_t processed_bytes = 0;//processed bytes

        uint8_t s0_version = 3;//s0 version
        std::vector<uint8_t> s1_time = std::vector<uint8_t>(4, 0);//s1 time
        std::vector<uint8_t> s1_random = std::vector<uint8_t>(1528, 0);//s1 random
    };

    enum class Rtmp_state
    {
        RTMP_HANDSHAKE_WAIT_C0C1 = 0,
        RTMP_HANDSHAKE_SEND,
        RTMP_HANDSHAKE_WAIT_C2,

        RTMP_CONNECT_WAIT_CONNECT,
        RTMP_STREAM_WAIT_CREATE_STREAM,
        RTMP_STREAM_WAIT_PUBLISH_PLAY,
        RTMP_STREAM_PUBLISHING,
        RTMP_STREAM_PLAYING
    };
    
    using TriggerMode = ChunkParser::TriggerMode;

    /* Result data */
    enum class StepStatus {
      Progress,
      NeedRead,
      NeedWrite,

      Error,
      PeerClosed,
      ProtocolError,
      SystemError,
      ResourceLimit,
      InternalError
    };

    enum class ProcessStage {
      Handshake,
      ChunkRead,
      ChunkParse,
      MessageAssemble,
      MessageDispatch,
      MessageHandle
    };

    struct ProcessResult {
      StepStatus  status;
      ProcessStage stage;
      int sys_errno = 0;  // Only valid when SystemError occurs
      MessageAssembler::MessagePtr message_ptr = nullptr;
    };

private:

    /* state data */
    Rtmp_state m_state;

    TriggerMode m_trigger_mode;
    int m_socket;   //socket fd

    int m_stream_id;   //stream ID
    RtmpInfo m_rtmp_info;//Record the connection-level information of the rtmp protocol
    
    /* Sub-module encapsulation */
    ChunkParser m_chunk_parser;   //chunk parser for handling chunk parsing
    MessageAssembler m_message_assembler;   //Message assembler for handling message fragmentation

public:
    /*Constructor and destructor functions*/
    Rtmp(int socket_fd, TriggerMode trigger_mode);
    ~Rtmp();
    
    /* External operation interface */
    ProcessResult process();//process all data
    bool close_rtmp();//close the rtmp connect and clean up resources

private:
    /* Specific business functions */
    StepStatus handshake1();//Shake hands, handle C0C1
    StepStatus send_handshake_response();//Send server response
    StepStatus handshake2();//Handle C2
    StepStatus connect();//Establish connection, wait for the "connect" command
    StepStatus create_stream();//Create stream
    StepStatus select_role();//select between publishing and playing
    StepStatus publish();
    StepStatus play();

    ssize_t send_buffer(
      int socket_fd,
      const uint8_t* data,
      size_t length,
      size_t& bytes_sent
    );//Segmented transmission, S0S1S2
};

#endif