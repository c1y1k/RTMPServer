# Rtmp编排测试

本目录验证`Rtmp::process()`能否把握手、`ChunkParser`和`MessageAssembler`连接成完整的数据通路。测试通过本地非阻塞`socketpair`模拟客户端，并通过临时的`ProcessResult::message_ptr`检查完整Message。

测试边界止于Message组装完成，不包含尚未实现的MessageDispatcher和具体业务处理器。

## 运行方式

从`RTMPServer`项目根目录执行：

```bash
bash rtmp/test/orchestration/run_tests.sh
```

只运行正确性测试：

```bash
bash rtmp/test/orchestration/run_tests.sh correctness
```

只运行性能测试，并指定Message数量和Payload长度：

```bash
bash rtmp/test/orchestration/run_tests.sh benchmark 5000 1024
```

## 正确性测试矩阵

| 编号 | 场景 | 主要检查 |
|---|---|---|
| ORCH-01 | LT单Chunk Message | 握手后完成解析、适配和组装 |
| ORCH-02 | ET单Chunk Message | ET读取后返回完整Message |
| ORCH-03 | LT跨Chunk Message | 默认128字节Chunk Size下正确重组 |
| ORCH-04 | ET跨Chunk Message | 缓存中的后续Chunk被持续处理 |
| ORCH-05 | Chunk TCP分片 | Basic Header、Message Header和Payload半包可继续推进 |
| ORCH-06 | 多CSID交错 | 两条逻辑通道的Message状态相互隔离 |
| ORCH-07 | 同CSID连续Message | 第一条返回后继续处理缓存中的下一条 |
| ORCH-08 | C2与首个Chunk粘包 | 握手完成后在同一次`process()`中进入Chunk处理 |
| ORCH-09 | 元数据适配 | timestamp、length、type ID和stream ID映射正确 |
| ORCH-10 | Payload生命周期 | Parser视图在下一次读取前被Assembler复制 |
| ORCH-11 | 非法首个fmt 3 | 映射为`ProtocolError/ChunkParse` |
| ORCH-12 | 内存与未定义行为 | AddressSanitizer和UndefinedBehaviorSanitizer无报告 |

## 性能测试

性能程序先完成一次握手，再在同一连接上重复发送跨Chunk Message。计时范围不包含握手，但包含socket读写、Chunk解析、元数据适配、Message组装和Payload复制。

输出指标：

- 完成的Message数量；
- Payload总字节数；
- 总耗时；
- 单条Message平均耗时；
- Message/s；
- Payload MiB/s。

性能程序使用`-O2`且不启用Sanitizer，不设置与机器性能绑定的通过阈值。

## 本轮验证结果

2026.9.19通过顶层测试入口运行结果：

```text
9/9 orchestration tests passed
messages: 5000
payload_bytes_per_message: 1024
total_payload_mib: 4.88
total_seconds: 0.03
average_us_per_message: 6.97
messages_per_second: 143384.11
payload_mib_per_second: 140.02
```

正确性测试使用C++17及`-Wall -Wextra -Wpedantic`编译，AddressSanitizer和UndefinedBehaviorSanitizer均无报告。性能数据仅作为当前环境中后续版本对比的样例基线。
