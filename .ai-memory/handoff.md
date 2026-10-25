## Handoff Checkpoint

**更新时间**: 2026-10-25 19:55
**当前目标**: 完成 MQTT OTA v1 的多设备最小闭环
**当前阶段**: Wave 3 已在本地提交，等待同步并运行正式 Jenkins Job dry-run
**完成度**: Wave 3 为 80%（代码、行为测试、制品生成和 Broker 发布已验证；正式 Jenkins Job 未运行）

### 已完成

- MAC 设备身份、动态 Topic、v1 消息和迁移兼容：6 个契约测试通过。
- 协议文档与实现字段一致：JSON 解析和静态契约检查通过。
- 固定 ESP-IDF 5.5 镜像全量构建和 `verify-firmware.sh` 通过；固件为 950,928 字节。
- Jenkins 参数、完整 v1 指令生成、设备 ID 校验/去重和逐设备发布已实现。
- 发布脚本行为测试和真实 Broker 的两个 `sim-ci` Topic 集成测试通过。

### 未完成

- 同步 Wave 3 提交，让正式 Jenkins Job 读取新 Jenkinsfile。
- 使用 `OTA_RELEASE_DRY_RUN=true` 完成一次正式人工审批流水线验证。
- 后续实现多设备模拟器并验证设备状态回执。

### 关键决策

- 正式 Topic 使用 `iot/v1/devices/{device_id}/...`；旧 Topic 仅保留一个迁移窗口。
- 实体设备 ID 使用 Wi-Fi STA MAC，模拟设备后续使用启动参数指定 ID。
- 多设备发布尝试全部合法目标后汇总结果；任一设备发布失败时流水线失败。

### 恢复入口

- **首读文件**: `Jenkinsfile`、`ci/release-firmware.sh`、`docs/protocol/mqtt-ota-v1.md`
- **关键命令**: `py -3 -B -m unittest discover -s tests -p "test_*.py" -v`
- **验证路径**: 提交后同步到 Jenkins SCM，使用 `sim-001`、`sim-003` 和 `OTA_RELEASE_DRY_RUN=true` 运行完整审批流程。

### 阻塞项

- Jenkins 校验端点匿名访问返回 403，因此 Jenkinsfile 尚未由正式 Jenkins 解析；需提交同步后通过真实 Job 验证。

## 交接

- **已完成项**（带验证证据）：固件 v1 适配、Python 契约测试 9/9、发布行为测试、真实 Broker 双 Topic 发布、ESP-IDF 5.5 制品生成均通过。
- **相关文件**（精确路径 + 行号）：`Jenkinsfile:29`、`Jenkinsfile:93`、`ci/release-firmware.sh:8`、`ci/verify-firmware.sh:146`、`tests/test-release-firmware.sh:1`、`tests/test-broker-integration.sh:1`。
- **未完成项**（下一步 + 入口）：同步 Wave 3，然后以 dry-run 参数运行正式 Jenkins Job。
- **阻塞项**（决策点）：正式 Jenkinsfile 解析与完整 Job 运行必须在同步后验证。
