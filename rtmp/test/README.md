# Rtmp集成测试

本目录用于验证`Rtmp`连接控制层及其子模块编排。

当前已经建立：

- `handshake/`：RTMP简单握手正确性测试与性能测试。
- `orchestration/`：握手、ChunkParser和MessageAssembler的连接级编排测试与性能测试。

## 运行当前全部测试

从`RTMPServer`项目根目录执行：

```bash
bash rtmp/test/run_all_tests.sh
```

测试二进制生成在`/tmp`，脚本退出时自动删除，不会写入项目目录。
