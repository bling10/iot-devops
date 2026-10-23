session-id: 20261023-2134

## [21:34] 动作: 修复 ESP32 OTA 正确性与回滚状态机

- 文件：`firmware/esp32-node/main/main.c`、`firmware/esp32-node/main/CMakeLists.txt`、`README.md`
- 决策：下载完整性、大小和 SHA-256 全部通过后才结束 OTA；失败统一中止；仅 `PENDING_VERIFY` 镜像运行联合健康检查。
- 验证：9 项源码契约断言、括号配对与 `git diff --check` 通过；服务器 ESP-IDF 5.5 全量编译、源码基线和固件门禁通过，测试固件为 949,040 字节。
- 未验证：真实 ESP32 正常升级、下载中断、摘要不匹配和 Bootloader 回滚。
