# RTMPServer

RTMPServer是一个面向学习与工程实践的C++ RTMP服务器项目，目标是在主从Reactor网络架构上实现RTMP连接管理、协议解析、消息处理和流转发。

当前版本为**v0.2：RTMP简单握手与Chunk拆包重组**。项目已经贯通非阻塞RTMP握手、Chunk增量解析、Message组装和连接级编排，并通过模块测试与连接级测试验证；Message Dispatcher、业务消息处理和完整推拉流链路仍待实现，因此当前版本尚不是可直接部署的完整RTMP服务器。

## 总体数据流

```text
TCP字节流
  → Rtmp::process()连接级编排
  → RTMP简单握手                       [v0.2已完成]
  → Chunk Parser                       [v0.2已完成]
  → Message Assembler                  [v0.2已完成]
  → 完整RtmpMessage                    [v0.2已完成]
  → Message Dispatcher                 [待实现]
  → 控制消息 / AMF命令 / 音视频消息处理 [待实现]
  → Stream Context与推拉流转发          [待实现]
```

## 当前进度

| 模块 | 状态 | 说明 |
|---|---|---|
| RTMP简单握手 | ✅ 已实现并验证 | C0C1、S0S1S2、C2及非阻塞收发状态推进 |
| Chunk Parser | ✅ 已实现并验证 | 增量解析入站RTMP Chunk |
| Message Assembler | ✅ 已实现并验证 | 按CSID组装完整Message并转移Payload所有权 |
| Rtmp连接级编排 | ✅ v0.2已贯通 | 串联握手、Chunk解析和Message组装 |
| Message Dispatcher | ⏳ 待实现 | 根据Message Type分发完整消息 |
| Message Handler | 🚧 初步设计 | 控制消息、AMF命令及音视频消息处理 |
| Stream Context | ⏳ 待实现 | 流状态、GOP缓存与订阅转发 |
| Reactor网络层 | ⏳ 待集成 | 后续集成事件循环、线程池和定时器 |

## v0.2已实现能力

### RTMP简单握手

- 解析C0和C1，校验RTMP version 3与C1 Zero字段。
- 构造并发送S0、S1和S2，正确处理网络字节序及握手回显字段。
- 校验C2对S1 Time和Random的回显。
- 为每条连接保存跨调用接收长度、发送偏移和握手状态。
- 支持非阻塞socket的LT/ET读取。
- 处理`EINTR`、`EAGAIN/EWOULDBLOCK`和发送短写；等待可写后从原偏移继续发送。

### Chunk Parser

`chunk_parser`面向TCP字节流实现增量式RTMP Chunk解析，支持：

- fmt 0～3 Header及同一CSID上的字段继承。
- 一至三字节Basic Header与扩展CSID。
- 普通时间戳和Extended Timestamp。
- RTMP默认及动态入站Chunk Size。
- 多CSID交错和跨Chunk Message边界跟踪。
- 任意TCP拆包、粘包输入。
- 非阻塞socket的LT/ET读取及`EAGAIN`、`EINTR`处理。
- 使用`NEED_MORE_DATA`、`CHUNK_READY`和`PROTOCOL_ERROR`区分解析结果。
- 通过受边界检查的非拥有Payload视图向编排层交付当前Chunk数据。

详细设计见[Chunk Parser说明](rtmp/chunk_parser/README.md)。

### Message Assembler与连接级编排

- 使用中立的`MessageChunk`结构解除Assembler对ChunkParser具体类型的依赖。
- 按CSID维护未完成Message，支持跨Chunk组装和多个CSID交错。
- 校验Message边界、首Chunk和Payload长度，失败路径不提交越界数据。
- Message完成后移动Payload到独立拥有数据的`RtmpMessage`，并清理对应CSID状态。
- `Rtmp::process()`优先处理用户缓冲区中的已有数据，只在数据不足时继续读取socket。
- 编排层把子模块结果映射为`StepStatus`和`ProcessStage`，区分等待读、等待写、协议错误、系统错误及资源限制。
- 当前测试边界在完整`RtmpMessage`输出处结束，后续由Message Dispatcher继续处理。

## 自动化测试

### 运行握手与编排测试

从项目根目录执行：

```bash
bash rtmp/test/run_all_tests.sh
```

### 测试结果

| 测试组 | 用例数 | 覆盖范围 |
|---|---:|---|
| Chunk Parser单元测试 | 25/25 | Header、时间戳、Chunk Size、拆包粘包、多CSID及缓冲区边界 |
| Message Assembler单元测试 | 11/11 | 单/多Chunk、CSID交错、状态复用、错误输入及Payload生命周期 |
| RTMP握手测试 | 6/6 | LT/ET、分片握手、字段回显、非法输入、短写和`EAGAIN`续发 |
| Rtmp编排测试 | 9/9 | 握手到Message输出、TCP分片、跨Chunk、多CSID及阶段衔接 |

正确性测试使用C++17、`-Wall -Wextra -Wpedantic`、AddressSanitizer和UndefinedBehaviorSanitizer。测试二进制生成在`/tmp`并在脚本退出时自动删除。

各模块也可以单独运行：

```bash
bash rtmp/chunk_parser/test/unit/run_tests.sh
bash rtmp/message_assembler/test/unit/run_tests.sh
bash rtmp/test/handshake/run_tests.sh
bash rtmp/test/orchestration/run_tests.sh
```

握手与编排目录还提供使用`-O2`独立编译的轻量性能测试，输出平均耗时、处理速率和Payload吞吐量。性能数据用于同一环境中的版本前后对比，不作为跨机器通过门槛。

测试矩阵与具体结果见：

- [Message Assembler测试说明](rtmp/message_assembler/test/README.md)
- [RTMP握手测试说明](rtmp/test/handshake/README.md)
- [Rtmp编排测试说明](rtmp/test/orchestration/README.md)

## OBS/SRS真实流量验证

```bash
bash rtmp/chunk_parser/test/real_capture/run_capture_test.sh
```

真实验证从OBS与SRS通信抓包中按TCP Sequence Number重组客户端入站字节流，剥离RTMP握手后交给Chunk Parser解析，并与Wireshark导出的RTMP字段交叉核对。

验证结果：

- 公开验证抓包保留Frame 1～168，剥离握手后重组并解析134,352字节RTMP数据。
- Wireshark实际导出字段的23个Chunk Header与Chunk Parser逐字段一致。
- 记录53个物理Chunk的Message归属及原始TCP Frame来源。
- 覆盖一个TCP Frame包含多个Chunk和一个Chunk跨多个TCP Frame的情况。

测试矩阵与运行说明见[Chunk Parser测试说明](rtmp/chunk_parser/test/README.md)，逐字段结果见[真实抓包对比报告](rtmp/chunk_parser/test/real_capture/real_capture_comparison.md)。

## 目录结构

```text
RTMPServer/
├── README.md
└── rtmp/
    ├── readme.md
    ├── rtmp.h
    ├── rtmp.cpp
    ├── chunk_parser/
    │   ├── chunk_parser.h
    │   ├── chunk_parser.cpp
    │   ├── README.md
    │   └── test/
    ├── message_assembler/
    │   ├── message_assembler.h
    │   ├── message_assembler.cpp
    │   └── test/
    └── test/
        ├── README.md
        ├── run_all_tests.sh
        ├── handshake/
        └── orchestration/
```

当前目录结构只展示已经提交并参与v0.2数据通路的模块。Message Dispatcher、业务处理器、Stream Context和网络事件层将在实际实现时加入，不预先创建空模块。

## 后续计划

1. 实现Message Dispatcher，并移除编排测试使用的临时`ProcessResult::message_ptr`出口。
2. 实现Set Chunk Size、Acknowledgement等RTMP协议控制消息处理。
3. 实现AMF0命令解析以及`connect`、`createStream`、`publish`和`play`流程。
4. 实现音频、视频和Data Message处理。
5. 实现Stream Context、GOP缓存和订阅转发。
6. 集成主从Reactor网络层、线程池、定时器和日志模块。
7. 使用OBS、FFmpeg和播放器完成端到端推拉流验证。

## 项目参考

- 网络服务器架构参考[TinyWebServer](https://github.com/c1y1k/tinywebserver)，相关代码将在后续按模块逐步集成。
- Chunk Parser依据RTMP 1.0规范独立实现。设计过程中参考了[SRS](https://github.com/ossrs/srs)对Chunk解析与Message重组数据流的处理方式；本项目将两项职责显式拆分为`ChunkParser`与`MessageAssembler`，并由连接级`Rtmp`模块统一编排。
- SRS同时用于RTMP协议行为研究和真实通信验证，不作为当前项目已经实现功能的一部分。
