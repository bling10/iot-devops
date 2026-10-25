## Handoff Checkpoint

**更新时间**: 2026-10-25
**当前目标**: 完成 MQTT OTA v1 的多设备最小闭环
**当前阶段**: Wave 3 正式 dry-run 已完成；Jenkins 运行镜像与阶段重启加固已实现，待部署复验
**完成度**: Wave 3 为 100%；流水线加固为 70%（代码、契约测试和隔离镜像验证完成，正式部署与 Restart from Stage 未验证）

### 已完成

- MAC 设备身份、动态 Topic、v1 消息和迁移兼容：6 个契约测试通过。
- 协议文档与实现字段一致：JSON 解析和静态契约检查通过。
- 固定 ESP-IDF 5.5 镜像全量构建和 `verify-firmware.sh` 通过；固件为 950,928 字节。
- Jenkins 参数、完整 v1 指令生成、设备 ID 校验/去重和逐设备发布已实现。
- 发布脚本行为测试和真实 Broker 的两个 `sim-ci` Topic 集成测试通过。
- 正式 Jenkins Job `#42` 已完成构建、人工审批和两个目标设备的 dry-run。
- 自定义 Jenkins 镜像固定安装 Python 3；发布上下文已随 stash 保存并在 Release 阶段恢复。
- 自定义镜像已在服务器隔离构建验证：默认用户为 `jenkins`（UID 1000），Python 为 `/usr/bin/python3` 3.13.5；未影响正式容器。

### 未完成

- 提交并同步当前流水线加固改动。
- 在正式 Compose 目录构建自定义 Jenkins 镜像，重建容器后确认 Python 3 持久存在。
- 从 Release OTA 阶段重启一次构建，验证仍使用原始版本和制品目录。
- 后续实现多设备模拟器并验证设备状态回执。

### 关键决策

- 正式 Topic 使用 `iot/v1/devices/{device_id}/...`；旧 Topic 仅保留一个迁移窗口。
- 实体设备 ID 使用 Wi-Fi STA MAC，模拟设备后续使用启动参数指定 ID。
- 多设备发布尝试全部合法目标后汇总结果；任一设备发布失败时流水线失败。
- Release 阶段只使用随已验证制品 stash 的原始发布元数据，不根据重启后的新 `BUILD_NUMBER` 重新推导版本。

### 恢复入口

- **首读文件**: `Jenkinsfile`、`ci/release-firmware.sh`、`docs/protocol/mqtt-ota-v1.md`
- **关键命令**: `py -3 -B -m unittest discover -s tests -p "test_*.py" -v`
- **验证路径**: 部署自定义 Jenkins 镜像后检查 `python3 --version`，完整运行一次 dry-run，再从 Release OTA 阶段重启并核对原始版本。

### 阻塞项

- 当前加固版 Jenkinsfile 尚未由正式 Jenkins 解析；需要同步并通过真实 Job 验证 Groovy 和阶段重启路径。

## 交接

- **已完成项**（带验证证据）：Wave 3 正式 Job `#42` dry-run 通过；Jenkins 固定 Python 3 镜像和发布元数据恢复代码已通过本地契约测试。
- **相关文件**（精确路径 + 行号）：`deploy/jenkins/Dockerfile:1`、`deploy/jenkins/compose.yml:3`、`Jenkinsfile:25`、`tests/test_release_pipeline_contract.py:1`。
- **未完成项**（下一步 + 入口）：提交同步加固改动，构建并部署 Jenkins 镜像，再复验完整 dry-run 和 Release OTA 阶段重启。
- **阻塞项**（决策点）：服务器 Jenkins 容器重建会影响正在运行的 Job，部署前应确认当前无活跃构建。
