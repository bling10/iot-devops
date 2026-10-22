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

## Glossary

- **源码检测**：对构建前的 C/C++ 源文件和编译诊断进行检查。
- **固件检测**：对构建生成的 ESP32 二进制镜像、容量、版本和摘要进行检查。
- **真实设备验证**：在实体 ESP32 上完成 OTA 写入、重启、健康确认或回滚并保留证据。
