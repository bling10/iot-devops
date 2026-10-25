# MQTT OTA v1 协议

状态：已冻结；固件端已实现，发布端待实现
协议版本：1
适用范围：ESP32 实体设备和本项目的 ESP32 模拟设备

## 1. 目的与边界

本协议用于完成最小可实现的多设备 OTA 闭环：Jenkins 将一个已验证的固件版本定向发布给一台或多台在线设备，每台设备分别下载、校验、安装并上报状态。

本版本包含：

- 稳定且唯一的设备 ID；
- 按设备隔离的 OTA 指令、状态和遥测主题；
- 固件大小与 SHA-256 校验；
- 可查询的升级阶段、最终结果和错误原因；
- QoS 1 下可能重复投递时的基本防重要求。

本版本不包含：

- 离线任务补发和任务过期时间；
- 设备分组、灰度比例、批次暂停和自动回退策略；
- 后端持久化的期望状态与实际状态；
- MQTT 认证、ACL、TLS 和固件签名；
- 整批发布的原子性保证。

因此，v1 只保证对当前在线设备进行定向发布和独立观测。设备离线时不会收到非保留的 OTA 指令。

## 2. 术语

| 名称 | 含义 |
|---|---|
| `device_id` | 设备稳定唯一标识，用于 MQTT Topic 和状态关联 |
| `release_id` | 一次 Jenkins 固件发布的唯一标识 |
| `reported_version` | 设备当前实际运行的固件版本 |
| `target_version` | OTA 指令要求设备安装的版本 |
| 终态 | `healthy`、`failed`、`rolled_back` 或 `skipped` |

## 3. 设备 ID

`device_id` 必须匹配以下规则：

```text
^[a-z0-9][a-z0-9-]{0,63}$
```

要求：

- 只能使用小写字母、数字和连字符；
- 长度为 1 至 64 个字符；
- 同一 MQTT Broker 中必须唯一；
- 设备重启或重新连接后不得改变。

实体 ESP32 使用稳定的 Wi-Fi STA MAC 地址生成：

```text
esp32-a1b2c3d4e5f6
```

模拟设备由启动参数显式指定，例如 `sim-001`、`sim-002`。

## 4. Topic 约定

| 用途 | Topic | 方向 | QoS | Retain |
|---|---|---|---:|---|
| OTA 指令 | `iot/v1/devices/{device_id}/ota/command` | Jenkins → 设备 | 1 | false |
| OTA 状态 | `iot/v1/devices/{device_id}/ota/status` | 设备 → 平台 | 1 | false |
| 遥测数据 | `iot/v1/devices/{device_id}/telemetry` | 设备 → 平台 | 1 | false |

规则：

1. 指令 Topic 中的 `device_id` 是目标设备的权威标识。
2. 状态和遥测载荷中的 `device_id` 必须与 Topic 中的值完全一致。
3. OTA 指令不得使用 Retain，避免设备重新上线后执行已经过期的发布。
4. QoS 1 可能产生重复消息，设备不得同时启动两个 OTA 任务。
5. v1 不定义广播 Topic；多设备发布由发布端逐台发送完成。

## 5. 通用编码规则

- MQTT Payload 必须是 UTF-8 编码的 JSON 对象。
- OTA 指令大小不得超过 2048 字节。
- `schema_version` 必须是整数 `1`。
- `message_type` 必须与消息种类一致。
- SHA-256 生成端必须输出 64 位小写十六进制字符串；设备端可以将大写字符规范化为小写后比较。
- 未定义字段允许被接收端忽略，以便添加可选字段。
- 必填字段缺失、类型错误或值越界时，接收端必须拒绝执行。
- 可选字段无值时应直接省略，不应发送 `null` 或空字符串。

## 6. OTA 指令

Topic：

```text
iot/v1/devices/{device_id}/ota/command
```

字段定义：

| 字段 | 类型 | 必填 | 约束 |
|---|---|---|---|
| `schema_version` | integer | 是 | 固定为 `1` |
| `message_type` | string | 是 | 固定为 `ota_command` |
| `release_id` | string | 是 | 1 至 95 字符；同一次发布中保持唯一且不变 |
| `version` | string | 是 | 格式为 `v主版本.次版本.修订号`，例如 `v1.0.40` |
| `target` | string | 是 | v1 固定为 `esp32` |
| `url` | string | 是 | 1 至 255 字符；v1 接受 HTTP 或 HTTPS |
| `artifact_size` | integer | 是 | 大于 0，且不得超过设备 OTA 分区容量 |
| `sha256` | string | 是 | 64 位小写十六进制字符串 |

标准示例见 [ota-command.json](examples/ota-command.json)。

处理规则：

1. 设备必须先完成 JSON、协议版本、消息类型和所有字段校验。
2. 当前存在 OTA 任务时，设备不得启动第二个任务。
3. 当前固件版本等于目标版本时，设备必须上报 `skipped/version_unchanged`。
4. 设备必须先上报 `received`，再启动下载。
5. 下载完成后必须同时校验实际字节数和 SHA-256。
6. 任一检查失败时不得设置新的启动分区。
7. 校验成功后才能上报 `verified` 并进入重启阶段。

如果消息损坏到无法安全取得 `release_id`，设备只记录本地错误日志，不发送无法关联发布任务的状态消息。

## 7. OTA 状态

Topic：

```text
iot/v1/devices/{device_id}/ota/status
```

字段定义：

| 字段 | 类型 | 必填 | 约束 |
|---|---|---|---|
| `schema_version` | integer | 是 | 固定为 `1` |
| `message_type` | string | 是 | 固定为 `ota_status` |
| `release_id` | string | 是 | 必须来自对应 OTA 指令 |
| `device_id` | string | 是 | 必须与 Topic 中的设备 ID 一致 |
| `stage` | string | 是 | 必须使用本节定义的阶段 |
| `result` | string | 是 | `in_progress`、`success`、`failed` 或 `skipped` |
| `reported_version` | string | 是 | 发送状态时实际运行的版本 |
| `target_version` | string | 是 | 对应指令中的 `version` |
| `artifact_size` | integer | 否 | 已知时发送；必须大于 0 |
| `uptime_sec` | integer | 是 | 本次启动后的运行秒数，必须大于等于 0 |
| `reason` | string | 否 | 失败或跳过原因，使用规定错误码 |

阶段和结果映射：

| `stage` | `result` | 是否终态 | 含义 |
|---|---|---|---|
| `received` | `in_progress` | 否 | 指令校验通过 |
| `downloading` | `in_progress` | 否 | 正在下载并写入备用分区 |
| `verified` | `in_progress` | 否 | 大小和 SHA-256 校验通过 |
| `rebooting` | `in_progress` | 否 | 已设置启动分区，即将重启 |
| `rollback` | `in_progress` | 否 | 新固件健康检查失败，准备回滚 |
| `healthy` | `success` | 是 | 新固件启动并通过健康检查 |
| `failed` | `failed` | 是 | 下载、校验或写入失败 |
| `rolled_back` | `failed` | 是 | 已恢复到旧固件 |
| `skipped` | `skipped` | 是 | 指令有效但无需执行 |

成功和失败示例分别见 [ota-status-healthy.json](examples/ota-status-healthy.json) 与 [ota-status-failed.json](examples/ota-status-failed.json)。

### 7.1 reason 错误码

`reason` 必须使用小写蛇形命名。v1 定义以下值：

| 分类 | 错误码 |
|---|---|
| 指令处理 | `unsupported_schema_version`、`invalid_message_type`、`target_mismatch`、`invalid_payload`、`ota_busy`、`version_unchanged`、`task_create_failed` |
| HTTP 下载 | `http_init_failed`、`http_open_failed`、`http_status_invalid`、`content_length_mismatch`、`http_read_failed`、`http_data_incomplete` |
| 分区与写入 | `ota_partition_missing`、`firmware_too_large`、`ota_begin_failed`、`artifact_size_exceeded`、`ota_write_failed`、`ota_end_failed`、`set_boot_partition_failed` |
| 摘要校验 | `sha256_init_failed`、`sha256_update_failed`、`sha256_finish_failed`、`sha256_mismatch`、`artifact_size_mismatch` |
| 设备资源与状态 | `out_of_memory`、`ota_context_save_failed`、`health_check_failed`、`internal_error` |

`internal_error` 仅用于没有更具体错误码的异常分支，不得代替已经能够识别的下载、校验或分区错误。

未列出的新错误码不得直接加入 v1 实现；应先更新本协议，再更新生产端和消费端。

## 8. 遥测消息

Topic：

```text
iot/v1/devices/{device_id}/telemetry
```

v1 沿用当前项目的遥测字段，并增加协议标识和设备 ID：

| 字段 | 类型 | 必填 |
|---|---|---|
| `schema_version` | integer | 是 |
| `message_type` | string | 是，固定为 `telemetry` |
| `device_id` | string | 是 |
| `temperature` | number | 是 |
| `humidity` | number | 是 |
| `rssi` | integer | 是 |
| `free_heap` | integer | 是 |
| `uptime_sec` | integer | 是 |
| `version` | string | 是 |

标准示例见 [telemetry.json](examples/telemetry.json)。

## 9. 重复投递与幂等边界

QoS 1 保证至少一次投递，不保证只投递一次。v1 的最低要求是：

- OTA 执行期间收到任何新指令时，不得并发启动第二个 OTA 任务；
- 收到与当前版本相同的目标版本时，上报 `skipped/version_unchanged`；
- 相同 `release_id` 的状态消息可以重复，采集端必须允许重复记录；
- v1 不保证设备重启后永久记住所有已完成的 `release_id`。

完整的持久化幂等、超时判定和离线补发留给后续发布协调后端实现。

## 10. 旧协议迁移

当前旧固件订阅 `device/esp32/ota`，不能直接接收新的设备专属 Topic。迁移要求：

1. 第一版 v1 过渡固件同时订阅旧 Topic 和自己的 v1 Topic；旧 Topic 仅在迁移期兼容原有五字段指令。
2. 已部署旧固件时，只通过旧 Topic 发布一次过渡固件。
3. 设备确认运行过渡固件后，后续发布改用 v1 专属 Topic。
4. 发布端不得默认同时向旧 Topic 和新 Topic 发送同一任务，以免重复执行。
5. 全部目标设备完成迁移后，删除旧 Topic 订阅。

如果当前没有已部署的实体设备，可以直接启用 v1 Topic，不执行兼容迁移。

## 11. 协议演进规则

- 新增可选字段且不改变原字段语义时，可以保持 `schema_version = 1`。
- 新增必填字段、删除字段或改变字段语义时，必须升级协议版本。
- 协议大版本升级时，Topic 中的版本段也必须同步升级，例如 `iot/v2/...`。
- 接收端遇到不支持的 `schema_version` 时必须拒绝执行，不得猜测兼容。

## 12. 验收场景

### 场景一：定向发布

- GIVEN `sim-001` 至 `sim-005` 均在线
- WHEN 发布端只向 `sim-001` 和 `sim-003` 的指令 Topic 发布同一版本
- THEN 只有这两台设备执行 OTA
- AND 其余设备继续运行且不产生该 `release_id` 的状态

### 场景二：成功升级

- GIVEN 设备收到字段完整且摘要正确的指令
- WHEN 设备完成下载、校验、切换分区和健康检查
- THEN 状态顺序至少包含 `received`、`downloading`、`verified`、`rebooting` 和 `healthy`
- AND `healthy` 的 `result` 为 `success`

### 场景三：摘要不一致

- GIVEN 下载文件的实际 SHA-256 与指令不一致
- WHEN 设备完成下载校验
- THEN 设备不上报 `verified`
- AND 上报 `failed/sha256_mismatch`
- AND 不设置新启动分区

### 场景四：版本相同

- GIVEN 设备当前版本等于指令中的目标版本
- WHEN 设备收到合法指令
- THEN 设备不上载固件
- AND 上报 `skipped/version_unchanged`

### 场景五：无效指令

- GIVEN 指令缺少必填字段或使用不支持的协议版本
- WHEN 设备解析消息
- THEN 设备不得启动下载
- AND 在能够安全取得 `release_id` 时上报对应失败原因，否则只记录本地日志

### 场景六：设备离线

- GIVEN 指令不使用 Retain 且设备处于离线状态
- WHEN 发布端发送 OTA 指令
- THEN 设备重新上线后不会自动执行该指令
- AND 系统不得把“MQTT 发布成功”解释为“设备升级成功”

## 13. 实现状态

当前 ESP32 固件已支持 v1 设备专属 Topic、消息校验、状态回执和遥测格式，并暂时保留旧 Topic 作为迁移入口。Jenkins 发布代码已支持校验目标设备列表、生成完整 v1 指令，并按设备专属 Topic 逐台发布；发布脚本已经过真实 Broker 的 `sim-ci` Topic 验证，正式 Jenkins Job 和模拟设备端到端升级仍待验证。
