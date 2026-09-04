# RTMPServer

RTMPServer是一个面向学习与工程实践的C++ RTMP服务器项目，目标是在主从Reactor网络架构上实现RTMP连接管理、协议解析、消息处理和流转发。

项目目前处于模块化开发阶段，正在先完成并验证RTMP协议层组件，再逐步集成网络事件层与完整推拉流链路。当前仓库**尚不是可直接部署的完整RTMP服务器**。

## 总体数据流

```text
TCP字节流
  → RTMP握手与连接管理
  → Chunk Parser
  → Message Assembler
  → Message Dispatcher
  → 控制消息 / AMF命令 / 音视频消息处理
  → Stream Context
  → 推流缓存与拉流转发
```

## 当前进度

| 模块 | 状态 | 说明 |
|---|---|---|
| Chunk Parser | ✅ 已实现并验证 | 增量解析入站RTMP Chunk |
| Message Assembler | 🚧 开发中 | 按CSID累积Chunk Payload并组装Message |
| Message Dispatcher | ⏳ 待实现 | 根据Message Type分发完整消息 |
| Message Handler | 🚧 初步设计 | 控制消息、AMF命令及音视频消息处理 |
| Stream Context | ⏳ 待实现 | 流状态、缓存与订阅转发 |
| RTMP连接与握手 | 🚧 初步设计 | 连接级状态机与握手流程 |
| Reactor网络层 | ⏳ 待集成 | 后续逐步集成事件循环、线程池和定时器 |

## 已完成：Chunk Parser

`chunk_parser`面向TCP字节流实现增量式RTMP Chunk解析，支持：

- fmt 0～3 Header及同一CSID上的字段继承。
- 一至三字节Basic Header与扩展CSID。
- 普通时间戳和Extended Timestamp。
- RTMP默认及动态入站Chunk Size。
- 多CSID交错和跨Chunk Message边界跟踪。
- 任意TCP拆包、粘包输入。
- 非阻塞socket的LT/ET读取及`EAGAIN`、`EINTR`处理。
- 使用`NEED_MORE_DATA`、`CHUNK_READY`和`PROTOCOL_ERROR`区分解析结果。

解析器输出Chunk Payload片段及其所属Message的元数据快照；完整Message重组由后续`MessageAssembler`负责。

详细设计见[Chunk Parser说明](rtmp/chunk_parser/README.md)。

## 测试与真实流量验证

### 自动化测试

```bash
bash rtmp/chunk_parser/test/unit/run_tests.sh
```

当前包含25个命名测试用例，覆盖Basic Header、Header继承、Extended Timestamp、动态Chunk Size、Message跨Chunk、多CSID交错、TCP拆包与粘包、非阻塞socket读取和接收缓冲区边界。

测试使用AddressSanitizer和UndefinedBehaviorSanitizer运行；在普通Linux终端显式启用LeakSanitizer后同样通过：

```bash
ASAN_OPTIONS=detect_leaks=1 \
bash rtmp/chunk_parser/test/unit/run_tests.sh
```

验证结果：

```text
25/25 tests passed
```

### OBS/SRS真实抓包

```bash
bash rtmp/chunk_parser/test/real_capture/run_capture_test.sh
```

真实验证从OBS与SRS通信抓包中按TCP Sequence Number重组客户端入站字节流，剥离RTMP握手后交给Chunk Parser解析，并与Wireshark导出的RTMPT字段交叉核对。

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
    ├── message_dispatcher/
    ├── message_handle/
    └── streamcontext/
```

当前只展示已经存在的协议业务层目录。网络层、线程池、定时器和日志模块将在实际集成时加入，不预先创建空模块。

## 后续计划

1. 完成Message Assembler及其自动化测试。
2. 实现Message Dispatcher和控制消息处理。
3. 实现AMF命令、音频消息与视频消息处理。
4. 完成RTMP握手与连接级状态机。
5. 实现Stream Context、GOP缓存和订阅转发。
6. 集成主从Reactor网络层、线程池、定时器和日志模块。
7. 使用OBS、FFmpeg和播放器完成端到端推拉流验证。

## 项目参考

- 网络服务器架构参考[TinyWebServer](https://github.com/c1y1k/tinywebserver)，相关代码将在后续按模块逐步集成。
- Chunk Parser依据RTMP 1.0规范独立实现。设计过程中参考了[SRS](https://github.com/ossrs/srs)对Chunk解析与Message重组数据流的处理方式；本项目将两项职责显式拆分为`ChunkParser`与`MessageAssembler`，并由连接级`Rtmp`模块统一编排。
- SRS同时用于RTMP协议行为研究和真实通信验证，不作为当前项目已经实现功能的一部分。
