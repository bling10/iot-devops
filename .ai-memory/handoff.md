## Handoff Checkpoint

**更新时间**: 2026-10-25 19:22
**当前目标**: 完成 MQTT OTA v1 的多设备最小闭环
**当前阶段**: Wave 2 固件实现与构建验证完成，准备进入 Wave 3
**完成度**: Wave 2 为 100%（身份、协议、ESP-IDF 构建与固件门禁全部验证）

### 已完成

- MAC 设备身份、动态 Topic、v1 消息和迁移兼容：6 个契约测试通过。
- 协议文档与实现字段一致：JSON 解析和静态契约检查通过。
- 固定 ESP-IDF 5.5 镜像全量构建和 `verify-firmware.sh` 通过；固件为 950,928 字节。

### 未完成

- 真机或后续模拟器验证 MQTT 消息流。
- Wave 3 为 Jenkins 增加目标设备列表和逐设备发布。

### 关键决策

- 正式 Topic 使用 `iot/v1/devices/{device_id}/...`；旧 Topic 仅保留一个迁移窗口。
- 实体设备 ID 使用 Wi-Fi STA MAC，模拟设备后续使用启动参数指定 ID。

### 恢复入口

- **首读文件**: `docs/protocol/mqtt-ota-v1.md`、`firmware/esp32-node/main/main.c`
- **关键命令**: `py -3 -m unittest discover -s tests -p "test_*.py" -v`
- **验证路径**: 先跑 6 个契约测试，再从服务器临时目录读取已验证产物；进入 Wave 3 后对发布脚本做 dry-run 和 Broker 集成验证。

### 阻塞项

- 无。服务器临时构建目录为 `/tmp/iot-devops-mqtt-ota-v1-20261025-1625`，未修改正式仓库。

## 交接

- **已完成项**（带验证证据）：固件 v1 适配、契约测试 6/6、ESP-IDF 5.5 全量构建和固件门禁通过。
- **相关文件**（精确路径 + 行号）：`firmware/esp32-node/main/main.c:98`、`firmware/esp32-node/main/main.c:164`、`firmware/esp32-node/main/main.c:652`、`tests/test_mqtt_ota_contract.py:1`。
- **未完成项**（下一步 + 入口）：Wave 3 修改 Jenkins 参数与 `release-firmware.sh`，实现目标设备列表逐台发布。
- **阻塞项**（决策点）：无。
