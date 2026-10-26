## Handoff Checkpoint

**更新时间**: 2026-10-26 18:30
**当前目标**: 完成正式 Jenkins 到模拟设备的非 dry-run 定向发布复验
**当前阶段**: 多设备模拟与监控入库闭环已完成，进入正式发布入口验证阶段
**完成度**: 多设备模拟主线 100%；完整毕业设计仍缺后端、真机实验和材料收口

### 已完成

- MAC 设备身份、动态 Topic、v1 消息和迁移兼容：6 个契约测试通过。
- 协议文档与实现字段一致：JSON 解析和静态契约检查通过。
- 固定 ESP-IDF 5.5 镜像全量构建和 `verify-firmware.sh` 通过；固件为 950,928 字节。
- Jenkins 参数、完整 v1 指令生成、设备 ID 校验/去重和逐设备发布已实现。
- 发布脚本行为测试和真实 Broker 的两个 `sim-ci` Topic 集成测试通过。
- 正式 Jenkins Job `#42` 已完成构建、人工审批和两个目标设备的 dry-run。
- 自定义 Jenkins 镜像固定安装 Python 3；发布上下文已随 stash 保存并在 Release 阶段恢复。
- 自定义镜像已在服务器隔离构建验证：默认用户为 `jenkins`（UID 1000），Python 为 `/usr/bin/python3` 3.13.5；未影响正式容器。
- Python 模拟器支持 5 个设备、真实 HTTP 下载、大小/SHA-256 校验、状态持久化和故障注入；本地及服务器 33 个测试通过。
- 正式 Jenkins 已重建，补充组 GID 991 可访问 Docker API，Server 版本为 29.5.3。
- Docker daemon 失效代理已停用，核心容器在重启后恢复。
- `iot-simulator` 已部署；5 台设备遥测、18 条 OTA 状态、异常注入和容器重启后的版本恢复均已验证。
- Telegraf Docker socket 权限及 MQTT OTA v1 Topic 已修复；InfluxDB 查到 5 台设备遥测和按设备 `5/3/3/6/1` 的 OTA 状态。

### 未完成

- 通过正式 Jenkins 人工审批，以非 dry-run 模式仅向 `sim-*` 设备完成一次定向发布。
- 核对 Grafana 面板能否按 `device_id`、`stage` 和 `result` 展示多设备数据。
- 从 Release OTA 阶段重启一次构建，验证仍使用原始版本和制品目录。

### 关键决策

- 正式 Topic 使用 `iot/v1/devices/{device_id}/...`；旧 Topic 仅保留一个迁移窗口。
- 实体设备 ID 使用 Wi-Fi STA MAC，模拟设备后续使用启动参数指定 ID。
- 多设备发布尝试全部合法目标后汇总结果；任一设备发布失败时流水线失败。
- Release 阶段只使用随已验证制品 stash 的原始发布元数据，不根据重启后的新 `BUILD_NUMBER` 重新推导版本。

### 恢复入口

- **首读文件**: `Jenkinsfile`、`ci/release-firmware.sh`、`docs/protocol/mqtt-ota-v1.md`
- **关键命令**: `py -3 -B -m unittest discover -s tests -p "test_*.py" -v`
- **验证路径**: Jenkins 选择 `sim-001` 至 `sim-005` 且关闭 dry-run，审批后核对 MQTT/InfluxDB 18 条状态，再从 Release OTA 阶段重启并核对原始版本。

### 阻塞项

- 第三方 Docker registry mirror 当前返回 403；现有固定镜像已通过可信备用来源缓存，不阻塞本阶段，但后续新增镜像仍需治理。

## 交接

- **已完成项**（带验证证据）：Docker 代理停用；模拟器部署；33/33 测试；5 台遥测；两轮各 18 条 OTA 状态；InfluxDB 入库验证。
- **相关文件**（精确路径 + 行号）：`deploy/iot/compose.yml:23`、`deploy/iot/telegraf/telegraf.conf:23`、`simulator/main.py:1`、`simulator/device.py:1`。
- **未完成项**（下一步 + 入口）：通过 Jenkins 非 dry-run 仅发布到 `sim-*`，随后检查 Grafana 面板和 Restart from Stage。
- **阻塞项**（决策点）：正式 Jenkins 发布仍需人工审批；失效 registry mirror 不影响已缓存镜像，但会影响未来首次拉取。
