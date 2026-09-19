# MessageAssembler测试说明

本目录只验证`MessageAssembler`模块。测试直接构造中立的`MessageChunk`，不依赖Chunk Parser、网络读取或完整Rtmp编排层。

## 运行方式

从`RTMPServer`项目根目录运行：

```bash
bash rtmp/message_assembler/test/unit/run_tests.sh
```

脚本使用C++17编译，开启`-Wall`、`-Wextra`、`-Wpedantic`、AddressSanitizer和UndefinedBehaviorSanitizer。测试二进制生成在`/tmp`并在脚本退出时删除。受限环境默认关闭LeakSanitizer；普通Linux终端可以显式启用：

```bash
ASAN_OPTIONS=detect_leaks=1 \
bash rtmp/message_assembler/test/unit/run_tests.sh
```

## 测试矩阵

| 维度 | 场景 | 状态 |
|---|---|---:|
| 单Chunk | 一次Append完成并输出完整Message | ✅ |
| 多Chunk | 继续接收、提前提取和最终完成 | ✅ |
| 多路复用 | 多CSID交错组装且状态隔离 | ✅ |
| CSID复用 | 完成并删除状态后组装下一条Message | ✅ |
| 零长度 | 合法零长度Message立即完成 | ✅ |
| 输入校验 | 非零长度空指针 | ✅ |
| 输入校验 | 非空Message的零长度片段 | ✅ |
| 输入校验 | 空Message携带非零Payload | ✅ |
| 状态校验 | 未建立Message便追加后续Chunk | ✅ |
| 状态校验 | 同一CSID重复开始Message | ✅ |
| 长度校验 | 越界片段被拒绝且不污染缓冲区 | ✅ |
| 提取校验 | 不存在的CSID不创建缓冲区 | ✅ |
| 生命周期 | Append立即复制源Payload | ✅ |
| 输出校验 | 元数据与Payload内容一致 | ✅ |
| 内存安全 | AddressSanitizer | ✅ |
| 未定义行为 | UndefinedBehaviorSanitizer | ✅ |

## 本轮测试结果

2026.9.5实际运行结果：

```text
11/11 tests passed
```

测试使用C++17编译且无编译警告，AddressSanitizer和UndefinedBehaviorSanitizer均无报告。Chunk Parser到MessageAssembler的模块边界测试不属于本轮范围。
