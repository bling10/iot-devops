## Handoff Checkpoint

**更新时间**: 2026-10-26
**当前目标**: 完成 MQTT OTA v1 的多设备最小闭环
**当前阶段**: Python 多设备模拟器已实现；Jenkins Docker 权限已修复，Docker daemon 代理待处理
**完成度**: 模拟器代码与本地测试 100%；服务器端到端验证待 Docker 代理恢复后执行

### 已完成

- MAC 设备身份、动态 Topic、v1 消息和迁移兼容：6 个契约测试通过。
- 协议文档与实现字段一致：JSON 解析和静态契约检查通过。
- 固定 ESP-IDF 5.5 镜像全量构建和 `verify-firmware.sh` 通过；固件为 950,928 字节。
- Jenkins 参数、完整 v1 指令生成、设备 ID 校验/去重和逐设备发布已实现。
- 发布脚本行为测试和真实 Broker 的两个 `sim-ci` Topic 集成测试通过。
- 正式 Jenkins Job `#42` 已完成构建、人工审批和两个目标设备的 dry-run。
- 自定义 Jenkins 镜像固定安装 Python 3；发布上下文已随 stash 保存并在 Release 阶段恢复。
- 自定义镜像已在服务器隔离构建验证：默认用户为 `jenkins`（UID 1000），Python 为 `/usr/bin/python3` 3.13.5；未影响正式容器。
- Python 模拟器支持 5 个设备、真实 HTTP 下载、大小/SHA-256 校验、状态持久化和故障注入；本地 31 个测试通过。
- 正式 Jenkins 已重建，补充组 GID 991 可访问 Docker API，Server 版本为 29.5.3。

### 未完成

- 停用宿主机 Docker daemon 的失效代理并重启 Docker；该操作会短暂影响全部容器，等待明确授权。
- 构建并启动正式模拟器容器，完成 MQTT OTA 五设备端到端验证。
- 从 Release OTA 阶段重启一次构建，验证仍使用原始版本和制品目录。

### 关键决策

- 正式 Topic 使用 `iot/v1/devices/{device_id}/...`；旧 Topic 仅保留一个迁移窗口。
- 实体设备 ID 使用 Wi-Fi STA MAC，模拟设备后续使用启动参数指定 ID。
- 多设备发布尝试全部合法目标后汇总结果；任一设备发布失败时流水线失败。
- Release 阶段只使用随已验证制品 stash 的原始发布元数据，不根据重启后的新 `BUILD_NUMBER` 重新推导版本。

### 恢复入口

- **首读文件**: `Jenkinsfile`、`ci/release-firmware.sh`、`docs/protocol/mqtt-ota-v1.md`
- **关键命令**: `py -3 -B -m unittest discover -s tests -p "test_*.py" -v`
- **验证路径**: Docker 代理恢复后拉取 ESP-IDF 镜像，启动模拟器并观察 5 台设备状态，再从 Release OTA 阶段重启并核对原始版本。

### 阻塞项

- Docker daemon 的 systemd drop-in 指向不可达代理 `192.168.100.1:7890`；修改后必须重启 Docker。

## 交接

- **已完成项**（带验证证据）：Jenkins Docker API 权限恢复；Python 模拟器与本地 31 个测试完成。
- **相关文件**（精确路径 + 行号）：`deploy/jenkins/compose.yml:7`、`scripts/start.sh:8`、`simulator/main.py:1`、`simulator/device.py:1`。
- **未完成项**（下一步 + 入口）：停用 Docker daemon 失效代理、重启 Docker 并复验全部容器，再构建模拟器。
- **阻塞项**（决策点）：重启 Docker 会短暂中断全部容器，需用户明确授权。
