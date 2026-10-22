## [10:11] 动作: README 补充质量门禁工具分工

- 修改文件：`README.md`。
- 决策：源码使用 Cppcheck 扫描、Jenkins Warnings NG 展示；固件使用 esptool、ESP-IDF 容量报告、分区检查和 SHA-256 校验。
- 状态边界：Cppcheck 与 Warnings NG 尚未接入；现有固件门禁已完成隔离构建验证，但真实 Jenkins Job 和真实设备结果仍待复验。
- 验证：`git diff --check` 通过，并回读关键章节确认 Markdown 内容完整。
