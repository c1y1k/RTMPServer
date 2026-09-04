# RTMP Chunk Parser

`chunk_parser`是RTMP入站链路的Chunk解析模块，负责把TCP字节流切分为带CSID和长度信息的Chunk Payload片段，并在每个连接内按CSID维护RTMP压缩Header所需的历史状态。

它解决三个核心问题：TCP数据可能半包或一次携带多个Chunk；RTMP Message可能被拆成多个Chunk；不同CSID的Chunk可以交错到达，且后续Header会继承同一CSID的历史字段。

## 输入与输出

输入：

- 配套的`Read(socket_fd)`按照epoll的LT/ET读取策略从非阻塞socket读取字节，处理`EAGAIN`、`EWOULDBLOCK`和`EINTR`，并写入连接级`ReceiveBuffer`。
- `ChunkParse()`消费`ReceiveBuffer`中的待解析数据，不直接依赖socket模式。
- `SetChunkSize()`为后续Type 1控制消息处理层提供入站Chunk Size更新接口，默认值为128字节，并执行协议字段和本地缓冲区上限校验。当前模块不会自动识别并处理完整的`Set Chunk Size`控制消息。

输出为`ChunkParser::ParseResult`：

- `NEED_MORE_DATA`：当前Header或Payload尚未完整，需要继续接收TCP数据。
- `CHUNK_READY`：一个Chunk Payload已经完整，返回`body_pos`、`body_length`和`csid`。
- `PROTOCOL_ERROR`：Header继承或连接级解析状态不合法，应由上层结束连接。

`body_pos`是Payload在`ReceiveBuffer`中的位置。调用方应在缓冲区下一次整理或扩容前消费或复制该段数据。

## 数据流

```text
TCP字节流
  → 接收缓冲区 ReceiveBuffer
  → Basic Header解析（fmt、CSID）
  → Message Header解析（时间戳、长度、类型、Stream ID）
  → 每个CSID的Header继承与Message剩余长度更新
  → Chunk Payload就绪（当前模块输出）
  → Payload累积（后续MessageAssembler）
  → 完整RtmpMessage（后续模块输出）
```

当前模块的职责边界止于`Chunk Payload就绪`，不负责生成完整`RtmpMessage`。

## 核心数据结构

### `ChunkParser`

每个RTMP连接持有一个解析器，保存当前物理Chunk的解析上下文：`phase`、当前`csid`、当前Chunk Body长度和入站`in_chunk_size`。解析分为Basic Header、Message Header、Extended Timestamp和Payload四个阶段，可跨多次TCP读取继续执行。

### `ReceiveBuffer`

连接级线性接收缓冲区，通过`length`和`parse_pos`区分已写入数据与待解析数据。空间耗尽时先整理已消费区域，再按上限动态扩容；达到上限后向上层报告`buffer_full`。

### `current_chunk_header`

以CSID为键保存`ChunkHeader`，包括归一化后的绝对时间戳、时间戳增量、Message长度、类型、Message Stream ID、Extended Timestamp类型和Message剩余字节数。它为fmt 1、2、3提供同一CSID上的Header继承状态。

fmt 3有两种用途：该CSID的Message仍有剩余字节时，它表示当前Message的后续Chunk；剩余字节为零时，它表示继承历史长度、类型、Stream ID和时间戳增量的新Message首Chunk。如果最近的fmt 0/1/2使用了Extended Timestamp，fmt 3也会继承该标志并消费对应四字节字段，但当前Message的后续Chunk不会再次更新时间戳。

### Message累积缓冲区

不属于当前模块。后续`MessageAssembler`按CSID接收`ParseResult`指向的Payload片段，累积到Message声明长度后输出完整`RtmpMessage`。

## 支持范围

- RTMP Basic Header的fmt 0～3。
- 一字节、两字节和三字节Basic Header及扩展CSID。
- fmt 0/1/2的Message Header解析和fmt 3继承。fmt 3可作为当前Message后续Chunk或继承参数的新Message首Chunk。
- 绝对时间戳、时间戳增量及fmt 0～3的Extended Timestamp字段消费。
- TCP半包和单次读取包含多个Chunk的连续解析。
- 多CSID的Header状态和Message剩余长度跟踪。
- 配套`Read()`支持非阻塞socket的LT/ET读取；`ChunkParse()`只消费用户态缓冲区，不感知触发模式。
- 有上限的接收缓冲区整理与扩容。
- 入站Chunk Size默认值及更新接口。零值或32位最高位非零属于协议字段非法；协议值合法但超过`max_cap`时属于本地资源策略拒绝。

## 职责范围之外

- 多个Chunk到完整`RtmpMessage`的Payload重组。
- 完整Message的类型分发、控制消息、AMF命令及音视频业务处理。
- RTMP握手和出站Chunk编码。

这些能力分别属于MessageAssembler、MessageDispatcher、业务消息处理器、握手模块和出站编码模块，不计划放入Chunk Parser。

## 当前版本限制

- 独立于socket的完整`Append(data, length)`输入接口。
- 非法输入下的事务式Header提交；当前解析过程会直接修改每个CSID的历史状态，后续计划通过当前Chunk工作副本完善。
- 模块内部状态尚未完全封装，`Read()`仍使用字符串表达读取结果。
- 尚未进行模糊测试和大规模恶意输入验证。

## 后续集成契约

`ParseResult`已经向后续MessageAssembler提供：

- `message_start`：当前Chunk是否为新Message的首个Chunk。
- `csid`：Chunk Stream ID。
- `body_pos`和`body_length`：Payload在接收缓冲区中的位置和长度。
- `metadata.timestamp`：归一化后的绝对时间戳。
- `metadata.message_length`：所属Message声明的总长度。
- `metadata.message_type_id`：Message Type ID。
- `metadata.message_stream_id`：Message Stream ID。

MessageAssembler应在下一次可能整理或扩容接收缓冲区的操作前，根据`body_pos`和`body_length`立即复制Payload。它使用`message_start`初始化对应CSID的Message累积状态，其余Chunk继续追加到同一Message。

## 编译与测试

运行自动化单元测试：

```bash
bash rtmp/chunk_parser/test/unit/run_tests.sh
```

运行OBS与SRS真实抓包验证：

```bash
bash rtmp/chunk_parser/test/real_capture/run_capture_test.sh
```

当前验证结果：

```text
25/25 automated tests passed
23/23 Wireshark Chunk Headers matched
53 physical Chunks mapped to their source TCP Frames
```

自动化测试使用AddressSanitizer和UndefinedBehaviorSanitizer运行，并已在普通Linux终端显式启用LeakSanitizer验证，未发现地址越界、未定义行为或内存泄漏。

详细测试范围见[测试说明与矩阵](test/README.md)，真实抓包逐字段结果见[Chunk Header与TCP Frame对比报告](test/real_capture/real_capture_comparison.md)。

## 当前状态

`v0.1`：核心Chunk解析流程已经实现，并通过自动化边界测试、Sanitizer检查及OBS/SRS真实抓包验证。MessageAssembler集成、模块接口进一步封装、事务式Header提交和模糊测试留待后续版本完成。
