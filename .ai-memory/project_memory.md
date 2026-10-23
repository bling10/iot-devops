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

## Glossary

- **源码检测**：对构建前的 C/C++ 源文件和编译诊断进行检查。
- **固件检测**：对构建生成的 ESP32 二进制镜像、容量、版本和摘要进行检查。
- **真实设备验证**：在实体 ESP32 上完成 OTA 写入、重启、健康确认或回滚并保留证据。
