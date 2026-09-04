# Chunk Parser测试说明

本目录使用直接向`ReceiveBuffer`写入RTMP字节流的方式测试`ChunkParse()`，使协议解析测试不依赖TCP包边界、epoll或完整RTMP服务器。`Read()`将在独立的`socketpair()`测试中验证。

测试按验证来源分为两个子目录：

```text
test/
├── unit/          # 人工构造输入、socketpair和边界条件测试
└── real_capture/  # OBS/SRS真实抓包、Wireshark基准及对比报告
```

## 运行方式

```bash
bash rtmp/chunk_parser/test/unit/run_tests.sh
```

脚本使用C++17编译，开启`-Wall`、`-Wextra`和`-Wpedantic`，并通过AddressSanitizer和UndefinedBehaviorSanitizer检查地址越界与未定义行为。部分受限沙箱或调试环境无法启用LeakSanitizer，因此脚本默认设置`detect_leaks=0`；普通Linux终端可显式启用：

```bash
ASAN_OPTIONS=detect_leaks=1 \
bash rtmp/chunk_parser/test/unit/run_tests.sh
```

最近验证结果（2026.9.2）：

```text
25/25 tests passed
```

这里的25/25表示25个命名测试用例全部通过。单个命名用例会包含多个协议字段、状态转换和边界断言，因此测试矩阵的行数多于命名用例数。

编译无警告，AddressSanitizer和UndefinedBehaviorSanitizer均无报告；2026.9.2在普通终端开启LeakSanitizer运行完整测试，25/25通过且无泄漏报告。

测试环境：Ubuntu 22.04.5 LTS，x86-64，GCC 11.4.0，C++17。测试脚本未显式指定优化选项，使用GCC默认的`-O0`，并开启编译警告、AddressSanitizer和UndefinedBehaviorSanitizer。

## 真实抓包验证

目录中的`rtmp_capture.pcapng`来自OBS与SRS之间的实际RTMP通信，公开版本裁剪并保留Frame 1～168；`wireshark_rtmp_obs_srs.json`是Wireshark对同一范围导出的RTMPT解析结果。验证选取包含完整握手的推流入站方向：

```text
172.30.80.1:51946 -> 172.30.95.121:1935
```

运行方式：

```bash
bash rtmp/chunk_parser/test/real_capture/run_capture_test.sh
```

完整的逐字段结果和每个物理Chunk对应的原始TCP Frame见[real_capture_comparison.md](real_capture/real_capture_comparison.md)。

脚本执行以下步骤：

1. 直接读取pcapng的原始Packet Block，保留与Wireshark一致的Frame编号并筛选目标方向。
2. 按TCP Sequence Number重组字节流，检查数据间隙和冲突重传，同时记录每段字节来自哪个TCP Frame。
3. 剥离客户端`C0+C1+C2`共3073字节的RTMP握手数据。
4. 使用保留重复JSON键的方式提取Wireshark实际导出的23个RTMP Chunk Header。Wireshark会在同一个TCP Frame包含多个可解析Chunk Header时导出多个同名`rtmpt`层，不能使用普通字典覆盖这些层。
5. 将真实字节流交给`ChunkParser`，遇到完整的Set Chunk Size消息时由测试驱动调用`SetChunkSize()`。
6. 按消息出现顺序比较fmt、CSID、timestamp、timestamp delta、Message Length、Message Type ID和Message Stream ID。
7. 根据每次`ChunkParse()`消费的字节范围，将每个物理Chunk映射到一个或多个原始TCP Frame并生成对比报告。

最近验证结果（2026.9.2）：

```text
prepared 134352 RTMP bytes and 23 Wireshark headers
compared 23/23 Wireshark Chunk Headers across 53 physical chunks
```

裁剪后的抓包在剥离握手后包含134,352字节RTMP数据，完整覆盖本轮23个可对比Chunk Header和53个物理Chunk。Wireshark实际导出字段的23个Chunk Header与ChunkParser输出逐字段一致。其余fmt 3延续Chunk没有独立的Wireshark字段，不声称进行Wireshark逐条对比；全部53个物理Chunk均记录了所属Message和原始TCP Frame。编译无警告，AddressSanitizer和UndefinedBehaviorSanitizer均无报告。中间字节流、参考TSV、Frame映射和测试程序生成在`/tmp`并在运行结束后删除，仓库保留裁剪后的原始抓包、Wireshark导出、可复现脚本和生成的Markdown对比报告。

字段对应关系：

| Wireshark字段 | ChunkParser字段 |
|---|---|
| `rtmpt.header.format` | `current_chunk_header[csid].fmt` |
| `rtmpt.header.csid` | `ParseResult::csid` |
| `rtmpt.header.timestamp` | `ParseResult::metadata.timestamp` |
| `rtmpt.header.timestampdelta` | `current_chunk_header[csid].timestamp_delta` |
| `rtmpt.header.bodysize` | `ParseResult::metadata.message_length` |
| `rtmpt.header.typeid` | `ParseResult::metadata.message_type_id` |
| `rtmpt.header.streamid` | `ParseResult::metadata.message_stream_id` |

## 状态说明

- ✅ 已通过：存在自动化测试，并已实际编译运行通过。
- ◐ 部分覆盖：相关能力已有测试，但仍缺少重要分支。
- ⏳ 待测试：当前没有对应的自动化验证。

## 测试矩阵

| 测试维度 | 场景 | 状态 | 对应用例或说明 |
|---|---|---:|---|
| Basic Header | 普通CSID 2～63 | ✅ | `single fmt0 chunk` |
| Basic Header | 两字节扩展CSID解析成功（边界64、319） | ✅ | `extended CSID boundaries` |
| Basic Header | 三字节扩展CSID解析成功（边界320、65599） | ✅ | `extended CSID boundaries` |
| Basic Header | 三字节扩展CSID使用非对称值验证低字节在前 | ✅ | `three-byte CSID little-endian` |
| Basic Header | 两字节和三字节扩展CSID截断 | ✅ | `truncated headers and payload` |
| TCP拆包 | Basic Header分段输入 | ✅ | `truncated headers and payload` |
| TCP拆包 | Message Header分段输入 | ✅ | `one chunk fragmented input` |
| TCP拆包 | Payload分段输入 | ✅ | `one chunk fragmented input`、`truncated headers and payload` |
| TCP粘包 | 多个Chunk一次输入 | ✅ | `multiple chunks in one input` |
| Message分片 | Message跨多个Chunk | ✅ | `message across chunks`；测试侧拼接Payload并校验内容 |
| Header继承 | fmt 0建立完整历史状态 | ✅ | `single fmt0 chunk`、`fmt1-fmt3 inheritance` |
| Header继承 | fmt 1继承Message Stream ID | ✅ | `fmt1-fmt3 inheritance` |
| Header继承 | fmt 2继承长度、类型和Stream ID | ✅ | `fmt1-fmt3 inheritance` |
| Header继承 | fmt 3延续当前Message | ✅ | `message across chunks` |
| Header继承 | fmt 3开始继承参数的新Message | ✅ | `fmt1-fmt3 inheritance` |
| 多路复用 | 多CSID的Chunk交错 | ✅ | `interleaved CSIDs` |
| 时间戳 | fmt 0绝对Extended Timestamp | ✅ | `extended timestamps` |
| 时间戳 | fmt 1扩展timestamp delta | ✅ | `extended timestamps` |
| 时间戳 | fmt 2扩展timestamp delta及半包 | ✅ | `fmt2 extended timestamp delta` |
| 时间戳 | fmt 3延续Chunk继承扩展字段 | ✅ | `extended timestamps` |
| 时间戳 | fmt 3新Message继承扩展delta | ✅ | `extended timestamps` |
| Message边界 | fmt 0/1/2及fmt 3新Message返回`message_start=true` | ✅ | `single fmt0 chunk`、`fmt1-fmt3 inheritance` |
| Message边界 | fmt 3延续Chunk返回`message_start=false` | ✅ | `message across chunks`、`extended timestamps` |
| Message元数据 | timestamp、length、type ID和Stream ID快照 | ✅ | 现有正常解析、继承、半包和多CSID用例均有断言 |
| Message元数据 | Message Stream ID使用非对称值验证小端解析 | ✅ | `Message Stream ID little-endian` |
| Message边界 | 零长度Message | ✅ | `zero-length message` |
| 非法状态 | 新CSID不以fmt 0开始 | ✅ | `invalid fields and lengths` |
| 非法状态 | Message尚未结束却出现非fmt 3 | ✅ | `invalid fields and lengths` |
| 数据不足 | Header、Extended Timestamp或Payload截断 | ✅ | `truncated headers and payload` |
| Chunk Size接口校验 | 合法值、零值、最高位和本地上限 | ✅ | `invalid fields and lengths`；只验证参数校验与存储 |
| Chunk Size解析行为 | 默认值128，Message长度129输出128+1两个Payload片段 | ✅ | `default chunk size payload boundary` |
| Chunk Size解析行为 | 更新为256后，Message长度300输出256+44两个Payload片段 | ✅ | `updated chunk size payload boundary` |
| Chunk Size解析行为 | 其他CSID的Message未结束时更新Chunk Size，验证新值作用于后续Chunk | ✅ | `chunk size update with unfinished CSID` |
| 真实抓包 | Frame 1～168的客户端TCP字节流按Sequence Number重组并剥离握手 | ✅ | `prepare_real_capture.py`，得到134352字节RTMP数据 |
| 真实抓包 | Wireshark实际导出的Chunk Header与ChunkParser逐字段对比 | ✅ | `run_capture_test.sh`，23/23 Header一致 |
| 真实抓包 | 每个物理Chunk记录所属Message并映射到原始TCP Frame | ✅ | `real_capture_comparison.md`，共53个Chunk，覆盖一帧多Chunk和一个Chunk跨多帧 |
| 网络读取 | LT模式读取与`EAGAIN` | ✅ | `Read LT nonblocking` |
| 网络读取 | ET模式读取到`EAGAIN` | ✅ | `Read ET drains until EAGAIN` |
| 网络读取 | LT和ET下的对端正常关闭 | ✅ | `Read peer close` |
| 网络读取 | LT和ET的`EINTR`重试 | ✅ | `Read LT retries EINTR`、`Read ET retries EINTR` |
| 缓冲区 | LT和ET下的已消费空间整理 | ✅ | `ReceiveBuffer compaction` |
| 缓冲区 | LT和ET下的动态扩容及`max_cap`截断 | ✅ | `ReceiveBuffer growth` |
| 缓冲区 | LT和ET达到`max_cap`返回`buffer_full`且不消费socket数据 | ✅ | `ReceiveBuffer full limit` |
| 内存安全 | AddressSanitizer | ✅ | `run_tests.sh`，未发现地址越界 |
| 未定义行为 | UndefinedBehaviorSanitizer | ✅ | `run_tests.sh`，无报告 |
| 内存泄漏 | LeakSanitizer | ✅ | 普通终端使用`ASAN_OPTIONS=detect_leaks=1`运行，25/25通过且无泄漏报告 |

## 本轮测试状态

当前25个命名测试均已通过，Chunk Size接口校验、实际Payload边界、Message Stream ID小端解析和三字节扩展CSID低字节在前均已覆盖。后续增加协议能力、修改缓冲区实现或调整模块接口时，应同步增加对应测试场景。

新增协议能力时，应先在矩阵中增加对应场景。测试代码写好但尚未实际运行时保持为⏳，只有在编译和运行通过后才能标记为✅。
