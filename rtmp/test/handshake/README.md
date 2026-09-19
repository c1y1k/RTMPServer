# RTMP握手测试

本目录通过本地`socketpair`模拟RTMP客户端与服务器端连接，只使用`Rtmp::process()`公开接口验证简单握手状态机。测试不依赖外部网络服务。

## 运行方式

从`RTMPServer`项目根目录执行：

```bash
bash rtmp/test/handshake/run_tests.sh
```

只运行正确性测试：

```bash
bash rtmp/test/handshake/run_tests.sh correctness
```

只运行性能测试，并指定握手次数：

```bash
bash rtmp/test/handshake/run_tests.sh benchmark 1000
```

## 正确性测试矩阵

| 编号 | 场景 | 主要检查 |
|---|---|---|
| HS-01 | LT模式分片握手 | C0C1和C2跨多次读取仍能完成 |
| HS-02 | ET模式分片握手 | 每轮读取至`EAGAIN`并正确保存进度 |
| HS-03 | 非法C0版本 | 非version 3输入返回握手错误 |
| HS-04 | 非法C1 Zero | C1 Zero非零时拒绝握手 |
| HS-05 | 非法C2回显 | C2 Random未回显S1 Random时拒绝握手 |
| HS-06 | 发送缓冲区满 | 返回`NeedWrite`，恢复可写后从正确偏移续发 |
| HS-07 | S0/S1字段 | S0为3，S1 Zero为四个零字节 |
| HS-08 | S2字段 | S2 Time和Random分别回显C1对应字段 |
| HS-09 | 状态推进 | 完成C2后进入`ChunkRead`阶段 |
| HS-10 | 内存与未定义行为 | AddressSanitizer和UndefinedBehaviorSanitizer无报告 |

## 性能测试

`handshake_benchmark.cpp`重复执行以下完整流程：

1. 创建本地非阻塞连接；
2. 客户端发送C0C1；
3. 服务器构造并发送S0S1S2；
4. 客户端构造并发送C2；
5. 服务器验证C2并进入Chunk读取阶段。

输出指标：

- 完成的握手总数；
- 总耗时；
- 单次握手平均耗时；
- 每秒完成握手数。

性能程序使用`-O2`且不启用Sanitizer。指标包含`socketpair`创建、系统调用和随机数生成成本，适合用于同一环境中的版本前后对比，不设置与机器性能绑定的通过阈值。

## 本轮验证结果

2026.9.19运行结果：

```text
6/6 handshake tests passed
handshakes: 1000
total_seconds: 0.05
average_us_per_handshake: 54.65
handshakes_per_second: 18297.68
```

正确性测试使用C++17及`-Wall -Wextra -Wpedantic`编译，AddressSanitizer和UndefinedBehaviorSanitizer均无报告。性能数据仅作为当前环境的样例基线。
