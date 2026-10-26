# 项目记忆

## Decision Record: 源码与固件质量门禁工具分工

**日期**: 2026-10-22
**问题**: ESP-IDF/C 源码和 ESP32 固件制品需要使用不同方式检测，同时避免为了展示报告而堆叠平台。

### 决策

- 源码静态分析以 Cppcheck 为主，ESP-IDF/GCC 编译诊断为补充，Jenkins Warnings NG 负责报告展示和新增问题门禁。
- ESP-IDF Clang-Tidy 只作为后续增强；SonarQube Community 暂不进入核心流水线。
- 固件制品由 `ci/verify-firmware.sh` 串联 esptool、ESP-IDF 容量报告、分区容量检查和 SHA-256 校验。
- 静态固件门禁不能替代真实 ESP32 的 OTA 启动、健康确认和 Bootloader 回滚实验。

### 影响范围

- `README.md` 中的组件状态、预期流水线、源码质量门禁、固件质量门禁和实施顺序。
- 后续 Jenkinsfile、CI 脚本及 Jenkins 插件配置。

### 撤销条件

若 Cppcheck 对 ESP-IDF 工程误报率无法通过编译数据库和抑制规则控制，或 Warnings NG 与服务器 Jenkins 版本不兼容，则重新评估 ESP-IDF Clang-Tidy 或外部质量平台，但保留源码检测与固件检测分层原则。

## Decision Record: OTA 下载完整性与首次启动健康确认

**日期**: 2026-10-23
**问题**: 设备端在 HTTP 句柄释放后继续访问该句柄，下载读取失败或不完整时仍可能进入 `esp_ota_end()`；回滚逻辑在所有启动中运行，并把 MQTT 连接直接当作新固件健康。

### 决策

- OTA 请求必须包含 `release_id`、版本、URL、固件大小和 SHA-256；设备流式计算摘要并核对实际大小。
- HTTP 完整性检查必须发生在 `esp_http_client_cleanup()` 前；读取、写入、长度或摘要任一失败统一调用 `esp_ota_abort()`。
- 仅当运行镜像处于 `ESP_OTA_IMG_PENDING_VERIFY` 时启动 180 秒健康检查。
- 健康确认要求业务任务启动、MQTT 连接、OTA 订阅确认和 QoS 1 遥测确认全部满足；失败显式调用 ESP-IDF 回滚 API。
- `release_id` 与目标版本在切换启动分区前写入独立 NVS namespace，用于新固件健康或旧固件回滚后的最小状态回执。

### 验证边界

- 已完成静态契约检查、括号平衡检查和 `git diff --check`。
- 2026-10-23 已获授权将当前工作区上传到服务器隔离临时目录，并使用固定 `espressif/idf:release-v5.5` 镜像完成源码基线检查、完整编译和固件验证。测试固件大小为 949,040 字节，OTA 分区上限为 1,835,008 字节，容量、镜像结构、内部版本和 SHA-256 均通过。
- 尚未执行真实 ESP32 烧录、下载中断、摘要不匹配和健康超时回滚实验；服务器测试未发布固件或发送 OTA 指令。

### 撤销条件

若真实构建证明 Mbed TLS 直接 SHA-256 API 与固定 ESP-IDF 5.5 镜像不兼容，则在不改变 OTA 消息协议的前提下切换为 ESP-IDF 支持的 PSA Crypto 或分区摘要实现。

## Decision Record: MQTT OTA v1 多设备寻址

**日期**: 2026-10-25
**问题**: 固定设备 ID 和共享 Topic 无法区分多台 ESP32，也无法对指定设备下发和关联 OTA 状态。

### 选项分析

| 选项 | 优势 | 劣势 | 复杂度 |
|---|---|---|---|
| 共享广播 Topic | 改动少 | 无法定向发布，设备状态相互混淆 | 低 |
| 每设备独立 Topic | 可定向发布和独立观测，结构清晰 | 发布端需要逐设备发送 | 中 |
| 设备组与后端任务系统 | 支持离线补发和批次控制 | 超出当前最小闭环范围 | 高 |

### 决策

**选择**: 使用 `iot/v1/devices/{device_id}/...` 每设备独立 Topic，实体设备 ID 由 Wi-Fi STA MAC 派生，MQTT Client ID 与设备 ID 保持一致。

**理由**: 不增加设备预配置和外部依赖，即可满足在线多设备定向 OTA 与状态关联。
**Trade-offs**: v1 不保证离线补发和整批发布原子性；旧 Topic 在一个迁移窗口内仍可广播。

### 影响范围

- `firmware/esp32-node/main/main.c`（`@decision MQTT-OTA-V1-TRANSITION`）
- `firmware/esp32-node/main/CMakeLists.txt`
- `docs/protocol/mqtt-ota-v1.md`
- 后续 Jenkins 发布脚本、模拟器和 Telegraf 配置

### 验证证据

- 2026-10-25 使用固定 `espressif/idf:release-v5.5` 镜像完成全量构建。
- `main.c` 编译、ELF 链接、ESP32 镜像生成和 `ci/verify-firmware.sh` 全部通过。
- 测试固件为 950,928 字节，OTA 分区上限为 1,835,008 字节，容量状态为 `PASS`。
- 产物 SHA-256 为 `90c1de669e01ba2457b0d01412f434014aa23c87f935bad0119d94c4a3d79106`，编译后二进制包含三个 v1 Topic 模板和迁移期旧 Topic。

### 撤销条件

当发布协调后端提供设备注册、分组、离线任务和持久化幂等后，重新评估 Topic 路由与设备身份来源，并移除旧 Topic 兼容订阅。

## Decision Record: Jenkins 多设备发布失败语义

**日期**: 2026-10-25
**问题**: Jenkins 需要向多台设备逐一发布 OTA 指令，同时避免非法目标、重复目标和单台失败掩盖整体结果。

### 选项分析

| 选项 | 优势 | 劣势 | 复杂度 |
|---|---|---|---|
| 首台失败立即停止 | 实现简单 | 后续目标没有结果，排障信息不完整 | 低 |
| 尝试全部目标后汇总失败 | 每台设备均有明确发布结果 | 可能出现部分成功，需要后端进一步记录 | 中 |
| 后端事务化发布 | 可持久化逐设备状态和重试 | 超出当前无后端的最小闭环 | 高 |

### 决策

**选择**: 发布脚本校验并去重全部设备 ID，使用 QoS 1 和非 Retain 消息逐设备发布；单台失败不阻止其余目标尝试，但最终返回非零状态并列出失败设备。开发阶段 Jenkins 的 `OTA_RELEASE_DRY_RUN` 默认开启。

**理由**: 在尚无发布协调后端时，最大化单次运行的可观测信息，同时不把部分成功误报为流水线成功。
**Trade-offs**: 脚本无法提供整批原子性或离线补发；重复运行可能再次投递同一 `release_id`。

### 影响范围

- `Jenkinsfile`
- `ci/verify-firmware.sh`
- `ci/release-firmware.sh`
- `tests/test-release-firmware.sh`
- `tests/test-broker-integration.sh`

### 验证证据

- Python 契约测试 9/9 通过。
- Linux Bash 语法检查和伪 Docker 行为测试通过，覆盖空输入、非法 ID、去重、dry-run、部分失败和非 Retain 发布。
- 服务器真实 Mosquitto Broker 上，`sim-ci-001` 与 `sim-ci-003` 各收到一条有效 v1 指令。
- 固定 ESP-IDF 5.5 容器复用已构建固件重新运行制品检查，生成的 `ota-notify.json` 八个 v1 字段全部匹配。

### 撤销条件

当发布协调后端接管设备列表、持久化发布任务和 MQTT 下发后，Jenkins 改为只登记发布任务，逐设备失败汇总逻辑迁移到后端。

## Decision Record: Jenkins 发布运行环境与阶段重启上下文

**日期**: 2026-10-25
**问题**: Release OTA 阶段调用 Python 3 校验发布指令，但基础 Jenkins 镜像不包含 Python；从失败阶段重启会产生新的 `BUILD_NUMBER`，而恢复的 stash 仍属于原始构建，导致版本目录错位。

### 选项分析

| 选项 | 优势 | 劣势 | 复杂度 |
|---|---|---|---|
| 在运行容器中临时安装 Python | 可立即恢复 | 容器重建后丢失，无法复现 | 低 |
| 自定义 Jenkins 镜像并随制品保存发布元数据 | 依赖持久、阶段重启仍使用已审批制品 | 增加一个镜像构建步骤和少量元数据代码 | 中 |
| 拆分独立发布 Job 或交给后端 | 职责最清晰 | 超出当前最小闭环范围 | 高 |

### 决策

**选择**: 基于固定 Jenkins 版本构建项目镜像并安装 Python 3；构建阶段将版本、制品目录、SHA-256、目标设备和 dry-run 模式写入 `release-metadata.properties`，与固件一同 stash，Release 阶段恢复后覆盖重新计算的环境变量。

**理由**: 同时消除临时安装和阶段重启目录错位，且不改变现有构建、审批和发布边界。
**Trade-offs**: 自定义镜像中的 Debian 软件包版本由镜像构建时的软件源决定；Jenkinsfile 元数据读取逻辑仍需正式 Job 解析和 Restart from Stage 实测。

### 影响范围

- `deploy/jenkins/Dockerfile`
- `deploy/jenkins/compose.yml`
- `Jenkinsfile`
- `tests/test_release_pipeline_contract.py`

### 撤销条件

当发布协调后端或独立 Release Job 接管已验证制品时，移除 Jenkins 内的发布上下文恢复逻辑；若 Jenkins 节点改为外部 Agent，则把 Python 3 依赖迁移到对应固定 Agent 镜像。

## Decision Record: Python 多设备模拟器与 Jenkins Docker socket 权限

**日期**: 2026-10-26

### 决策

- 使用单进程、多 Paho MQTT Client 模拟 5 台设备，每台设备拥有独立 Topic、版本、状态文件和 OTA 互斥锁。
- 固件通过标准库 HTTP 流式下载并校验实际大小和 SHA-256；重启与 Bootloader 回滚只做明确标注的状态模拟。
- Paho 固定为 2.1.0，使用 Callback API v2；状态消息使用 QoS 1、非 Retain，并等待 PUBACK。
- Jenkins 保持非 root 用户，通过 Compose `group_add` 获得宿主机 Docker socket 的数字 GID；`scripts/start.sh` 每次启动动态读取该 GID。

### 验证证据与边界

- 本地 Python 测试 31/31 通过，包含真实本地 HTTP 传输、摘要错误、健康失败回滚和并发 OTA 拒绝。
- Jenkins 重建后为 `uid=1000`、补充组 `991`，容器内 Docker API 可返回 Server 29.5.3，原权限错误已消失。
- 宿主机 Docker daemon 仍配置不可达代理，未缓存镜像拉取会超时；停用代理需要重启全部 Docker 容器，尚未获得明确授权。

### 撤销条件

当最小后端接管设备状态持久化与故障编排后，模拟器仅保留设备行为；当 Jenkins 迁移到独立 Agent 时，移除控制器容器的 Docker socket 挂载与补充组。

## Glossary

- **源码检测**：对构建前的 C/C++ 源文件和编译诊断进行检查。
- **固件检测**：对构建生成的 ESP32 二进制镜像、容量、版本和摘要进行检查。
- **真实设备验证**：在实体 ESP32 上完成 OTA 写入、重启、健康确认或回滚并保留证据。
