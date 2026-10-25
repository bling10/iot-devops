#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <inttypes.h>
#include "esp_wifi.h"
#include "esp_system.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "esp_log.h"
#include "mqtt_client.h"
// ===== OTA 新增头文件 =====
#include "esp_ota_ops.h"        // OTA 操作 API：写分区、设置启动分区
#include "esp_http_client.h"    // HTTP 客户端：从 Nginx 下载固件
#include "esp_app_desc.h"       // 读取当前固件版本描述
#include "cJSON.h"              // 解析 Jenkins 推送的 OTA JSON 消息
#include "mbedtls/sha256.h"     // 下载过程中计算固件 SHA-256

void mqtt_app_start(void);      // 前置声明

// ================= 1. 核心参数配置区 =================
// 测试阶段硬编码凭据，正式发布时应迁移至 menuconfig (Kconfig) 或 NVS 分区
#define WIFI_SSID          "wifi"                  // 确保是 2.4GHz 频段的热点
#define WIFI_PASS          "888888889999"          // WiFi 密码
#define MQTT_BROKER        "mqtt://192.168.249.60" // 虚拟机 IP（确保桥接模式，两端互通）
#define MQTT_TELEMETRY_TOPIC_FORMAT "iot/v1/devices/%s/telemetry"
#define MQTT_OTA_TOPIC_FORMAT       "iot/v1/devices/%s/ota/command"
#define MQTT_OTA_STATUS_TOPIC_FORMAT "iot/v1/devices/%s/ota/status"
// @decision MQTT-OTA-V1-TRANSITION 旧 Topic 仅保留一个迁移窗口，并只兼容原有五字段指令
#define LEGACY_OTA_TOPIC    "device/esp32/ota"
#define MQTT_TOPIC_MAX_LEN  128
#define DEVICE_ID_MAX_LEN   65
#define OTA_BUF_SIZE       1024                    // OTA 下载缓冲区大小
#define OTA_MESSAGE_MAX_LEN 2048
#define OTA_HEALTH_TIMEOUT_MS 180000
#define OTA_SCHEMA_VERSION 1
#define OTA_MESSAGE_TYPE   "ota_command"
#define OTA_TARGET         "esp32"
#define OTA_RELEASE_ID_MAX_LEN 96
#define OTA_VERSION_MAX_LEN 32
#define OTA_URL_MAX_LEN    256
#define OTA_SHA256_HEX_LEN 64

#define HEALTH_SENSOR_STARTED_BIT BIT0
#define HEALTH_MQTT_CONNECTED_BIT BIT1
#define HEALTH_OTA_SUBSCRIBED_BIT BIT2
#define HEALTH_TELEMETRY_ACK_BIT BIT3
#define HEALTH_REQUIRED_BITS (HEALTH_SENSOR_STARTED_BIT | HEALTH_MQTT_CONNECTED_BIT | \
                              HEALTH_OTA_SUBSCRIBED_BIT | HEALTH_TELEMETRY_ACK_BIT)

#define OTA_NVS_NAMESPACE  "ota_state"
#define OTA_NVS_RELEASE_ID "release_id"
#define OTA_NVS_VERSION    "version"

typedef struct {
    char release_id[OTA_RELEASE_ID_MAX_LEN];
    char version[OTA_VERSION_MAX_LEN];
    char url[OTA_URL_MAX_LEN];
    size_t artifact_size;
    char sha256[OTA_SHA256_HEX_LEN + 1];
} ota_request_t;

typedef enum {
    OTA_BOOT_REPORT_NONE = 0,
    OTA_BOOT_REPORT_HEALTHY,
    OTA_BOOT_REPORT_ROLLED_BACK,
} ota_boot_report_t;

static const char *TAG = "IOT_NODE";

// 全局句柄与真实连接状态位
esp_mqtt_client_handle_t mqtt_client = NULL;
static char device_id[DEVICE_ID_MAX_LEN] = {0};
static char telemetry_topic[MQTT_TOPIC_MAX_LEN] = {0};
static char ota_topic[MQTT_TOPIC_MAX_LEN] = {0};
static char ota_status_topic[MQTT_TOPIC_MAX_LEN] = {0};
static volatile bool is_mqtt_connected = false; // 【修复】volatile 保证多任务/中断间可见性
static volatile bool is_ota_in_progress = false; // OTA 进行中标志，防止重复触发
static volatile bool is_pending_verify = false;
static EventGroupHandle_t health_event_group = NULL;
static int ota_subscribe_msg_id = -1;
static int legacy_ota_subscribe_msg_id = -1;
static int health_telemetry_msg_id = -1;
static ota_request_t boot_ota_request = {0};
static ota_boot_report_t boot_report_pending = OTA_BOOT_REPORT_NONE;

static esp_err_t initialize_device_identity(void)
{
    uint8_t mac[6];
    esp_err_t err = esp_read_mac(mac, ESP_MAC_WIFI_STA);
    if (err != ESP_OK) {
        return err;
    }

    int written = snprintf(
        device_id,
        sizeof(device_id),
        "esp32-%02x%02x%02x%02x%02x%02x",
        (unsigned int)mac[0], (unsigned int)mac[1], (unsigned int)mac[2],
        (unsigned int)mac[3], (unsigned int)mac[4], (unsigned int)mac[5]
    );
    if (written < 0 || (size_t)written >= sizeof(device_id)) {
        return ESP_ERR_INVALID_SIZE;
    }

    written = snprintf(telemetry_topic, sizeof(telemetry_topic),
                       MQTT_TELEMETRY_TOPIC_FORMAT, device_id);
    if (written < 0 || (size_t)written >= sizeof(telemetry_topic)) {
        return ESP_ERR_INVALID_SIZE;
    }

    written = snprintf(ota_topic, sizeof(ota_topic), MQTT_OTA_TOPIC_FORMAT, device_id);
    if (written < 0 || (size_t)written >= sizeof(ota_topic)) {
        return ESP_ERR_INVALID_SIZE;
    }

    written = snprintf(ota_status_topic, sizeof(ota_status_topic),
                       MQTT_OTA_STATUS_TOPIC_FORMAT, device_id);
    if (written < 0 || (size_t)written >= sizeof(ota_status_topic)) {
        return ESP_ERR_INVALID_SIZE;
    }

    return ESP_OK;
}

static bool mqtt_topic_matches(esp_mqtt_event_handle_t event, const char *expected_topic)
{
    size_t expected_length = strlen(expected_topic);
    return event->topic_len == (int)expected_length &&
           strncmp(event->topic, expected_topic, expected_length) == 0;
}

static const char *get_firmware_version(void)
{
    const esp_app_desc_t *app_desc = esp_app_get_description();
    return app_desc != NULL ? app_desc->version : "unknown";
}

static const char *ota_result_for_stage(const char *stage)
{
    if (strcmp(stage, "healthy") == 0) {
        return "success";
    }
    if (strcmp(stage, "failed") == 0 || strcmp(stage, "rolled_back") == 0) {
        return "failed";
    }
    if (strcmp(stage, "skipped") == 0) {
        return "skipped";
    }
    return "in_progress";
}

static bool publish_ota_status(const ota_request_t *request, const char *stage, const char *reason)
{
    if (!is_mqtt_connected || mqtt_client == NULL || request == NULL) {
        return false;
    }

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        ESP_LOGE(TAG, "无法创建 OTA 状态回执");
        return false;
    }

    cJSON_AddNumberToObject(root, "schema_version", OTA_SCHEMA_VERSION);
    cJSON_AddStringToObject(root, "message_type", "ota_status");
    cJSON_AddStringToObject(root, "release_id", request->release_id);
    cJSON_AddStringToObject(root, "device_id", device_id);
    cJSON_AddStringToObject(root, "stage", stage);
    cJSON_AddStringToObject(root, "result", ota_result_for_stage(stage));
    cJSON_AddStringToObject(root, "reported_version", get_firmware_version());
    cJSON_AddStringToObject(root, "target_version", request->version);
    if (request->artifact_size > 0) {
        cJSON_AddNumberToObject(root, "artifact_size", (double)request->artifact_size);
    }
    cJSON_AddNumberToObject(root, "uptime_sec", (double)(esp_timer_get_time() / 1000000));
    if (reason != NULL && reason[0] != '\0') {
        cJSON_AddStringToObject(root, "reason", reason);
    }

    char *payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (payload == NULL) {
        ESP_LOGE(TAG, "无法序列化 OTA 状态回执");
        return false;
    }

    int msg_id = esp_mqtt_client_publish(mqtt_client, ota_status_topic, payload, 0, 1, 0);
    cJSON_free(payload);
    if (msg_id < 0) {
        ESP_LOGE(TAG, "OTA 状态回执发送失败: %s", stage);
        return false;
    }

    ESP_LOGI(TAG, "OTA 状态已提交: %s", stage);
    return true;
}

static esp_err_t save_ota_boot_context(const ota_request_t *request)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(OTA_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_str(handle, OTA_NVS_RELEASE_ID, request->release_id);
    if (err == ESP_OK) {
        err = nvs_set_str(handle, OTA_NVS_VERSION, request->version);
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

static bool load_ota_boot_context(ota_request_t *request)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(OTA_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return false;
    }

    size_t release_id_len = sizeof(request->release_id);
    size_t version_len = sizeof(request->version);
    err = nvs_get_str(handle, OTA_NVS_RELEASE_ID, request->release_id, &release_id_len);
    if (err == ESP_OK) {
        err = nvs_get_str(handle, OTA_NVS_VERSION, request->version, &version_len);
    }
    nvs_close(handle);
    return err == ESP_OK;
}

static void clear_ota_boot_context(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(OTA_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "无法打开 OTA 状态存储: %s", esp_err_to_name(err));
        return;
    }

    err = nvs_erase_all(handle);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "无法清理 OTA 状态存储: %s", esp_err_to_name(err));
    }
}

static void try_publish_boot_result(void)
{
    if (boot_report_pending == OTA_BOOT_REPORT_NONE) {
        return;
    }

    const char *state = boot_report_pending == OTA_BOOT_REPORT_HEALTHY ? "healthy" : "rolled_back";
    if (publish_ota_status(&boot_ota_request, state, NULL)) {
        clear_ota_boot_context();
        boot_report_pending = OTA_BOOT_REPORT_NONE;
    }
}

static void ota_health_task(void *pvParameters)
{
    EventBits_t bits = xEventGroupWaitBits(
        health_event_group,
        HEALTH_REQUIRED_BITS,
        pdFALSE,
        pdTRUE,
        pdMS_TO_TICKS(OTA_HEALTH_TIMEOUT_MS)
    );

    if ((bits & HEALTH_REQUIRED_BITS) == HEALTH_REQUIRED_BITS) {
        esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
        if (err == ESP_OK) {
            is_pending_verify = false;
            boot_report_pending = OTA_BOOT_REPORT_HEALTHY;
            ESP_LOGI(TAG, "新固件通过业务健康检查，已标记为 VALID");
            try_publish_boot_result();
            vTaskDelete(NULL);
            return;
        }
        ESP_LOGE(TAG, "无法确认新固件有效: %s", esp_err_to_name(err));
    } else {
        ESP_LOGE(TAG, "新固件健康检查超时，已通过条件位: 0x%02x", (unsigned int)bits);
    }

    publish_ota_status(&boot_ota_request, "rollback", "health_check_failed");
    esp_err_t rollback_err = esp_ota_mark_app_invalid_rollback_and_reboot();
    ESP_LOGE(TAG, "OTA 回滚失败: %s", esp_err_to_name(rollback_err));
    esp_restart();
}


// ================= 2. OTA 空中升级核心逻辑 =================

/**
 * OTA 下载与烧录任务
 * 
 * 工作流程：
 * 1. 通过 HTTP GET 从 Nginx 下载固件二进制流
 * 2. 获取下一个可用的 OTA 分区（ota_0 或 ota_1）
 * 3. 逐块写入 Flash
 * 4. 写入完成后标记新分区为启动分区
 * 5. 自动重启，运行新固件
 */
static void sha256_to_hex(const unsigned char digest[32], char output[OTA_SHA256_HEX_LEN + 1])
{
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < 32; i++) {
        output[i * 2] = hex[digest[i] >> 4];
        output[i * 2 + 1] = hex[digest[i] & 0x0f];
    }
    output[OTA_SHA256_HEX_LEN] = '\0';
}

static void ota_task(void *pvParameters)
{
    ota_request_t *request = pvParameters;
    esp_http_client_handle_t client = NULL;
    const esp_partition_t *update_partition = NULL;
    esp_ota_handle_t ota_handle = 0;
    mbedtls_sha256_context sha_context;
    char *buffer = NULL;
    const char *failure_reason = "internal_error";
    bool ota_started = false;
    size_t total_read = 0;
    int last_progress = -1;

    mbedtls_sha256_init(&sha_context);
    ESP_LOGI(TAG, "========== OTA 升级任务启动 ==========");
    ESP_LOGI(TAG, "release_id=%s | version=%s | size=%u",
             request->release_id, request->version, (unsigned int)request->artifact_size);
    publish_ota_status(request, "downloading", NULL);

    esp_http_client_config_t http_config = {
        .url = request->url,
        .timeout_ms = 30000,
    };
    client = esp_http_client_init(&http_config);
    if (client == NULL) {
        ESP_LOGE(TAG, "HTTP 客户端初始化失败");
        failure_reason = "http_init_failed";
        goto cleanup;
    }

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP 连接失败: %s", esp_err_to_name(err));
        failure_reason = "http_open_failed";
        goto cleanup;
    }

    int content_length = esp_http_client_fetch_headers(client);
    int status_code = esp_http_client_get_status_code(client);
    ESP_LOGI(TAG, "HTTP 状态码: %d | Content-Length: %d", status_code, content_length);
    if (status_code != 200) {
        ESP_LOGE(TAG, "HTTP 请求异常，状态码: %d", status_code);
        failure_reason = "http_status_invalid";
        goto cleanup;
    }
    if (content_length > 0 && (size_t)content_length != request->artifact_size) {
        ESP_LOGE(TAG, "HTTP 固件大小与发布清单不一致: %d != %u",
                 content_length, (unsigned int)request->artifact_size);
        failure_reason = "content_length_mismatch";
        goto cleanup;
    }

    update_partition = esp_ota_get_next_update_partition(NULL);
    if (update_partition == NULL) {
        ESP_LOGE(TAG, "找不到可用的 OTA 分区");
        failure_reason = "ota_partition_missing";
        goto cleanup;
    }
    if (request->artifact_size > update_partition->size) {
        ESP_LOGE(TAG, "固件超过 OTA 分区容量: %u > %" PRIu32,
                 (unsigned int)request->artifact_size, update_partition->size);
        failure_reason = "firmware_too_large";
        goto cleanup;
    }
    ESP_LOGI(TAG, "目标 OTA 分区: %s (地址: 0x%08" PRIx32 ", 大小: %" PRIu32 " 字节)",
             update_partition->label, update_partition->address, update_partition->size);

    err = esp_ota_begin(update_partition, OTA_WITH_SEQUENTIAL_WRITES, &ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA 写入初始化失败: %s", esp_err_to_name(err));
        failure_reason = "ota_begin_failed";
        goto cleanup;
    }
    ota_started = true;

    buffer = malloc(OTA_BUF_SIZE);
    if (buffer == NULL) {
        ESP_LOGE(TAG, "OTA 缓冲区分配失败");
        failure_reason = "out_of_memory";
        goto cleanup;
    }

    if (mbedtls_sha256_starts(&sha_context, 0) != 0) {
        ESP_LOGE(TAG, "SHA-256 初始化失败");
        failure_reason = "sha256_init_failed";
        goto cleanup;
    }
    while (true) {
        int read_len = esp_http_client_read(client, buffer, OTA_BUF_SIZE);
        if (read_len < 0) {
            ESP_LOGE(TAG, "HTTP 数据读取失败");
            failure_reason = "http_read_failed";
            goto cleanup;
        }
        if (read_len == 0) {
            bool http_complete = esp_http_client_is_complete_data_received(client);
            if (!http_complete) {
                ESP_LOGE(TAG, "HTTP 连接提前结束，固件数据不完整");
                failure_reason = "http_data_incomplete";
                goto cleanup;
            }
            break;
        }

        if (total_read + (size_t)read_len > request->artifact_size) {
            ESP_LOGE(TAG, "下载数据超过发布清单中的固件大小");
            failure_reason = "artifact_size_exceeded";
            goto cleanup;
        }

        err = esp_ota_write(ota_handle, buffer, (size_t)read_len);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Flash 写入失败: %s", esp_err_to_name(err));
            failure_reason = "ota_write_failed";
            goto cleanup;
        }
        if (mbedtls_sha256_update(&sha_context, (const unsigned char *)buffer, (size_t)read_len) != 0) {
            ESP_LOGE(TAG, "SHA-256 计算失败");
            failure_reason = "sha256_update_failed";
            goto cleanup;
        }

        total_read += (size_t)read_len;
        int progress = (int)((total_read * 100U) / request->artifact_size);
        if (progress / 10 != last_progress / 10) {
            last_progress = progress;
            ESP_LOGI(TAG, "OTA 下载进度: %d%% (%u/%u 字节)", progress,
                     (unsigned int)total_read, (unsigned int)request->artifact_size);
        }
    }

    if (total_read != request->artifact_size) {
        ESP_LOGE(TAG, "实际下载大小与发布清单不一致: %u != %u",
                 (unsigned int)total_read, (unsigned int)request->artifact_size);
        failure_reason = "artifact_size_mismatch";
        goto cleanup;
    }

    unsigned char digest[32];
    if (mbedtls_sha256_finish(&sha_context, digest) != 0) {
        ESP_LOGE(TAG, "SHA-256 计算收尾失败");
        failure_reason = "sha256_finish_failed";
        goto cleanup;
    }
    char actual_sha256[OTA_SHA256_HEX_LEN + 1];
    sha256_to_hex(digest, actual_sha256);
    if (strcmp(actual_sha256, request->sha256) != 0) {
        ESP_LOGE(TAG, "固件 SHA-256 与发布清单不一致");
        failure_reason = "sha256_mismatch";
        goto cleanup;
    }

    free(buffer);
    buffer = NULL;
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    client = NULL;

    err = esp_ota_end(ota_handle);
    ota_started = false;
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA 镜像校验失败: %s", esp_err_to_name(err));
        failure_reason = "ota_end_failed";
        goto cleanup;
    }

    err = save_ota_boot_context(request);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "无法保存 OTA 发布上下文: %s", esp_err_to_name(err));
        failure_reason = "ota_context_save_failed";
        goto cleanup;
    }

    err = esp_ota_set_boot_partition(update_partition);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "设置启动分区失败: %s", esp_err_to_name(err));
        clear_ota_boot_context();
        failure_reason = "set_boot_partition_failed";
        goto cleanup;
    }

    publish_ota_status(request, "verified", NULL);
    publish_ota_status(request, "rebooting", NULL);
    ESP_LOGI(TAG, "OTA 下载、大小与 SHA-256 校验全部通过");
    ESP_LOGI(TAG, "新固件已写入分区: %s，3 秒后重启", update_partition->label);
    free(request);
    mbedtls_sha256_free(&sha_context);
    vTaskDelay(pdMS_TO_TICKS(3000));
    esp_restart();
    return;

cleanup:
    mbedtls_sha256_free(&sha_context);
    if (ota_started) {
        esp_ota_abort(ota_handle);
    }
    if (buffer != NULL) {
        free(buffer);
    }
    if (client != NULL) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
    }
    publish_ota_status(request, "failed", failure_reason);
    ESP_LOGE(TAG, "OTA 升级已中止: %s", failure_reason);
    free(request);
    is_ota_in_progress = false;
    vTaskDelete(NULL);
}

/**
 * 解析 OTA 通知 JSON 并启动升级任务
 * 
 * 预期 JSON 格式（由 Jenkins 流水线生成）：
 * {
 *   "schema_version": 1,
 *   "message_type": "ota_command",
 *   "release_id": "jenkins-36-abc123def456",
 *   "version": "v1.0.12",
 *   "target": "esp32",
 *   "url": "http://192.168.249.60:8081/firmware_v1.0.12.bin",
 *   "artifact_size": 927104,
 *   "sha256": "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
 * }
 */
static bool copy_required_string(cJSON *root, const char *name, char *destination, size_t capacity)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(root, name);
    if (!cJSON_IsString(item) || item->valuestring == NULL) {
        ESP_LOGE(TAG, "OTA JSON 缺少字符串字段: %s", name);
        return false;
    }

    size_t length = strlen(item->valuestring);
    if (length == 0 || length >= capacity) {
        ESP_LOGE(TAG, "OTA JSON 字段长度非法: %s", name);
        return false;
    }

    memcpy(destination, item->valuestring, length + 1);
    return true;
}

static bool normalize_sha256(char sha256[OTA_SHA256_HEX_LEN + 1])
{
    if (strlen(sha256) != OTA_SHA256_HEX_LEN) {
        return false;
    }

    for (size_t i = 0; i < OTA_SHA256_HEX_LEN; i++) {
        unsigned char value = (unsigned char)sha256[i];
        if (!isxdigit(value)) {
            return false;
        }
        sha256[i] = (char)tolower(value);
    }
    return true;
}

static bool is_valid_firmware_version(const char *version)
{
    if (version == NULL || version[0] != 'v') {
        return false;
    }

    int dot_count = 0;
    int component_length = 0;
    for (size_t i = 1; version[i] != '\0'; i++) {
        unsigned char value = (unsigned char)version[i];
        if (isdigit(value)) {
            component_length++;
            continue;
        }
        if (value != '.' || component_length == 0 || dot_count >= 2) {
            return false;
        }
        dot_count++;
        component_length = 0;
    }

    return dot_count == 2 && component_length > 0;
}

static const char *validate_ota_envelope(cJSON *root, bool allow_legacy_envelope,
                                         bool *uses_legacy_envelope)
{
    cJSON *schema_version = cJSON_GetObjectItemCaseSensitive(root, "schema_version");
    cJSON *message_type = cJSON_GetObjectItemCaseSensitive(root, "message_type");
    cJSON *target = cJSON_GetObjectItemCaseSensitive(root, "target");
    bool has_envelope = schema_version != NULL || message_type != NULL || target != NULL;

    *uses_legacy_envelope = allow_legacy_envelope && !has_envelope;
    if (*uses_legacy_envelope) {
        return NULL;
    }

    if (!cJSON_IsNumber(schema_version) ||
        schema_version->valuedouble != (double)schema_version->valueint) {
        return "invalid_payload";
    }
    if (schema_version->valueint != OTA_SCHEMA_VERSION) {
        return "unsupported_schema_version";
    }
    if (!cJSON_IsString(message_type) || message_type->valuestring == NULL) {
        return "invalid_payload";
    }
    if (strcmp(message_type->valuestring, OTA_MESSAGE_TYPE) != 0) {
        return "invalid_message_type";
    }
    if (!cJSON_IsString(target) || target->valuestring == NULL) {
        return "invalid_payload";
    }
    if (strcmp(target->valuestring, OTA_TARGET) != 0) {
        return "target_mismatch";
    }

    return NULL;
}

static void handle_ota_message(const char *data, int data_len, bool allow_legacy_envelope)
{
    if (data == NULL || data_len <= 0 || data_len > OTA_MESSAGE_MAX_LEN) {
        ESP_LOGE(TAG, "OTA 消息长度非法: %d", data_len);
        return;
    }

    char *json_str = malloc((size_t)data_len + 1U);
    if (json_str == NULL) {
        ESP_LOGE(TAG, "内存分配失败，无法解析 OTA 消息");
        return;
    }
    memcpy(json_str, data, (size_t)data_len);
    json_str[data_len] = '\0';

    cJSON *root = cJSON_Parse(json_str);
    free(json_str);
    if (root == NULL) {
        ESP_LOGE(TAG, "OTA JSON 解析失败");
        return;
    }

    ota_request_t *request = calloc(1, sizeof(ota_request_t));
    if (request == NULL) {
        ESP_LOGE(TAG, "无法分配 OTA 请求上下文");
        cJSON_Delete(root);
        return;
    }

    bool fields_valid =
        copy_required_string(root, "release_id", request->release_id, sizeof(request->release_id)) &&
        copy_required_string(root, "version", request->version, sizeof(request->version)) &&
        copy_required_string(root, "url", request->url, sizeof(request->url)) &&
        copy_required_string(root, "sha256", request->sha256, sizeof(request->sha256));

    cJSON *artifact_size_item = cJSON_GetObjectItemCaseSensitive(root, "artifact_size");
    if (!cJSON_IsNumber(artifact_size_item) || artifact_size_item->valuedouble <= 0 ||
        artifact_size_item->valuedouble > UINT32_MAX ||
        artifact_size_item->valuedouble != (double)(size_t)artifact_size_item->valuedouble) {
        ESP_LOGE(TAG, "OTA JSON 的 artifact_size 非法");
        fields_valid = false;
    } else {
        request->artifact_size = (size_t)artifact_size_item->valuedouble;
    }

    bool uses_legacy_envelope = false;
    const char *envelope_error = validate_ota_envelope(
        root, allow_legacy_envelope, &uses_legacy_envelope
    );
    cJSON_Delete(root);
    if (!fields_valid || !normalize_sha256(request->sha256) ||
        !is_valid_firmware_version(request->version)) {
        ESP_LOGE(TAG, "OTA 发布清单校验失败");
        if (request->release_id[0] != '\0' && request->version[0] != '\0') {
            publish_ota_status(request, "failed", "invalid_payload");
        }
        free(request);
        return;
    }
    if (strncmp(request->url, "http://", 7) != 0 && strncmp(request->url, "https://", 8) != 0) {
        ESP_LOGE(TAG, "OTA URL 必须使用 HTTP 或 HTTPS");
        publish_ota_status(request, "failed", "invalid_payload");
        free(request);
        return;
    }
    if (envelope_error != NULL) {
        ESP_LOGE(TAG, "OTA v1 信封校验失败: %s", envelope_error);
        publish_ota_status(
            request,
            strcmp(envelope_error, "target_mismatch") == 0 ? "skipped" : "failed",
            envelope_error
        );
        free(request);
        return;
    }
    if (uses_legacy_envelope) {
        ESP_LOGW(TAG, "收到旧格式 OTA 指令，仅用于协议迁移");
    }

    if (is_ota_in_progress) {
        ESP_LOGW(TAG, "OTA 升级正在进行中，拒绝并发任务");
        publish_ota_status(request, "skipped", "ota_busy");
        free(request);
        return;
    }

    ESP_LOGI(TAG, "收到 OTA 升级通知:");
    ESP_LOGI(TAG, "   release_id: %s", request->release_id);
    ESP_LOGI(TAG, "   远端版本: %s", request->version);
    const char *current_version = get_firmware_version();
    ESP_LOGI(TAG, "   当前版本: %s", current_version);
    ESP_LOGI(TAG, "   固件大小: %u", (unsigned int)request->artifact_size);

    if (strcmp(request->version, current_version) == 0) {
        ESP_LOGW(TAG, "远端版本与当前版本一致 (%s)，跳过升级", current_version);
        publish_ota_status(request, "skipped", "version_unchanged");
        free(request);
        return;
    }

    is_ota_in_progress = true;
    publish_ota_status(request, "received", NULL);
    BaseType_t task_created = xTaskCreate(ota_task, "OTA_Task", 8192, request, 5, NULL);
    if (task_created != pdPASS) {
        ESP_LOGE(TAG, "OTA 任务创建失败");
        publish_ota_status(request, "failed", "task_create_failed");
        is_ota_in_progress = false;
        free(request);
    }
}

// ================= 3. 网络状态机事件回调 =================

// 处理 WiFi 连接状态
static void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "WiFi 断开，尝试重连...");
        is_mqtt_connected = false; // WiFi 断开，MQTT 必然失效
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "WiFi 连接成功! 获取到本地 IP: " IPSTR, IP2STR(&event->ip_info.ip));
        
        // 【核心修改】确保获取到 IP 后才启动/恢复 MQTT
        if (mqtt_client == NULL) {
            ESP_LOGI(TAG, "网络就绪，首次启动 MQTT 客户端...");
            mqtt_app_start();
        }
    }
}

// 处理 MQTT 连接状态
static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
    esp_mqtt_event_handle_t event = event_data;
    switch ((esp_mqtt_event_id_t)event_id) {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "MQTT 服务器连接成功");
            is_mqtt_connected = true;
            if (is_pending_verify && health_event_group != NULL) {
                xEventGroupSetBits(health_event_group, HEALTH_MQTT_CONNECTED_BIT);
            }
            ota_subscribe_msg_id = esp_mqtt_client_subscribe(mqtt_client, ota_topic, 1);
            if (ota_subscribe_msg_id < 0) {
                ESP_LOGE(TAG, "OTA v1 主题订阅请求失败");
            } else {
                ESP_LOGI(TAG, "OTA v1 主题订阅请求已提交: %s", ota_topic);
            }
            legacy_ota_subscribe_msg_id = esp_mqtt_client_subscribe(
                mqtt_client, LEGACY_OTA_TOPIC, 1
            );
            if (legacy_ota_subscribe_msg_id < 0) {
                ESP_LOGW(TAG, "旧 OTA 主题订阅请求失败");
            } else {
                ESP_LOGI(TAG, "旧 OTA 主题订阅请求已提交: %s", LEGACY_OTA_TOPIC);
            }
            try_publish_boot_result();
            break;
        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "MQTT 服务器已断开");
            is_mqtt_connected = false;
            ota_subscribe_msg_id = -1;
            legacy_ota_subscribe_msg_id = -1;
            health_telemetry_msg_id = -1;
            if (health_event_group != NULL) {
                xEventGroupClearBits(health_event_group,
                                     HEALTH_MQTT_CONNECTED_BIT | HEALTH_OTA_SUBSCRIBED_BIT);
            }
            break;
        case MQTT_EVENT_SUBSCRIBED:
            if (is_pending_verify && health_event_group != NULL &&
                event->msg_id == ota_subscribe_msg_id) {
                xEventGroupSetBits(health_event_group, HEALTH_OTA_SUBSCRIBED_BIT);
                ESP_LOGI(TAG, "OTA v1 主题订阅已确认");
            } else if (event->msg_id == legacy_ota_subscribe_msg_id) {
                ESP_LOGI(TAG, "旧 OTA 主题订阅已确认");
            }
            break;
        case MQTT_EVENT_PUBLISHED:
            if (is_pending_verify && health_event_group != NULL &&
                event->msg_id == health_telemetry_msg_id) {
                xEventGroupSetBits(health_event_group, HEALTH_TELEMETRY_ACK_BIT);
                ESP_LOGI(TAG, "健康检查遥测消息已由 Broker 确认");
            }
            break;
        case MQTT_EVENT_DATA:
            ESP_LOGI(TAG, "收到 MQTT 消息 -> 主题: %.*s", event->topic_len, event->topic);
            ESP_LOGI(TAG, "消息内容: %.*s", event->data_len, event->data);

            bool is_v1_topic = mqtt_topic_matches(event, ota_topic);
            bool is_legacy_topic = mqtt_topic_matches(event, LEGACY_OTA_TOPIC);
            if (is_v1_topic || is_legacy_topic) {
                ESP_LOGI(TAG, "检测到 OTA 升级通知，开始处理...");
                handle_ota_message(event->data, event->data_len, is_legacy_topic);
            }
            break;
        default:
            break;
    }
}

// ================= 4. 基础设施初始化 =================
void wifi_init_sta(void) {
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
}

void mqtt_app_start(void) {
    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = MQTT_BROKER,
        .credentials.client_id = device_id,
    };
    mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_register_event(mqtt_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(mqtt_client);
}

// ================= 5. 业务线程区 =================

// 任务一：传感器采集与上报
void vTaskSensor(void *pvParameters) {
    char payload[384];
    float mock_temp = 99.0;
    float mock_hum = 50.0;

    if (is_pending_verify && health_event_group != NULL) {
        xEventGroupSetBits(health_event_group, HEALTH_SENSOR_STARTED_BIT);
    }

    while (1) {
        // 【核心修改】只认状态位，不认空指针
        if (is_mqtt_connected) {
            // mock_temp += 0.5;
            // if(mock_temp > 30.0) mock_temp = 25.0;
            mock_temp = 99.9; // 制造极限异常温度

            // 获取 Wi-Fi RSSI
            wifi_ap_record_t ap;
            int8_t rssi = 0;
            if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
                rssi = ap.rssi;
            }

            // 获取可用堆内存
            uint32_t free_heap = esp_get_free_heap_size();

            // 获取连续运行时间 (秒)
            uint32_t uptime_sec = (uint32_t)(esp_timer_get_time() / 1000000);

            int payload_length = snprintf(
                payload,
                sizeof(payload),
                "{\"schema_version\":1,\"message_type\":\"telemetry\",\"device_id\":\"%s\","
                "\"temperature\":%.2f,\"humidity\":%.2f,\"rssi\":%d,"
                "\"free_heap\":%" PRIu32 ",\"uptime_sec\":%" PRIu32 ","
                "\"version\":\"%s\"}",
                device_id, mock_temp, mock_hum, rssi, free_heap, uptime_sec,
                get_firmware_version()
            );
            if (payload_length < 0 || (size_t)payload_length >= sizeof(payload)) {
                ESP_LOGE(TAG, "遥测消息生成失败或超出缓冲区");
                vTaskDelay(pdMS_TO_TICKS(2000));
                continue;
            }
            
            int msg_id = esp_mqtt_client_publish(
                mqtt_client, telemetry_topic, payload, 0, 1, 0
            );
            if (msg_id < 0) {
                ESP_LOGE(TAG, "数据上报失败！将在下个周期重试");
            } else {
                if (is_pending_verify && health_telemetry_msg_id < 0) {
                    health_telemetry_msg_id = msg_id;
                }
                ESP_LOGI(TAG, "成功上报数据 -> msg_id=%d | 报文: %s", msg_id, payload);
            }
        } else {
            // 网络断开时静默等待，拒绝盲目向死连接发数据
            ESP_LOGW(TAG, "[业务挂起] 当前网络未就绪，暂停采集上报...");
        }
        
        // 保持 2 秒采样周期
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

// ================= 6. 系统主入口 =================
void app_main(void) {
    ESP_LOGI(TAG, "=========================================");
    ESP_LOGI(TAG, "嵌入式 IoT 终端启动 | OTA 空中升级已就绪");
    ESP_LOGI(TAG, "当前固件版本: %s", get_firmware_version());

    // 打印当前运行的 OTA 分区信息
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (running != NULL) {
        ESP_LOGI(TAG, "当前运行分区: %s (地址: 0x%08"PRIx32")", running->label, running->address);
    }
    ESP_LOGI(TAG, "=========================================");

    // 1. 初始化 NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(initialize_device_identity());
    ESP_LOGI(TAG, "设备 ID: %s", device_id);
    ESP_LOGI(TAG, "遥测主题: %s", telemetry_topic);
    ESP_LOGI(TAG, "OTA 指令主题: %s", ota_topic);
    ESP_LOGI(TAG, "OTA 状态主题: %s", ota_status_topic);

    bool has_boot_context = load_ota_boot_context(&boot_ota_request);
    esp_ota_img_states_t ota_state = ESP_OTA_IMG_UNDEFINED;
    ret = running == NULL ? ESP_ERR_NOT_FOUND : esp_ota_get_state_partition(running, &ota_state);
    if (ret == ESP_OK && ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
        is_pending_verify = true;
        if (!has_boot_context) {
            snprintf(boot_ota_request.release_id, sizeof(boot_ota_request.release_id), "unknown");
            snprintf(boot_ota_request.version, sizeof(boot_ota_request.version), "%s", get_firmware_version());
            ESP_LOGW(TAG, "新固件缺少持久化发布上下文，将使用 unknown 标识");
        }

        ESP_LOGI(TAG, "当前镜像处于 PENDING_VERIFY，启动 180 秒业务健康检查");
        health_event_group = xEventGroupCreate();
        if (health_event_group == NULL ||
            xTaskCreate(ota_health_task, "OTA_Health", 4096, NULL, 6, NULL) != pdPASS) {
            ESP_LOGE(TAG, "无法启动 OTA 健康检查，执行回滚");
            esp_err_t rollback_err = esp_ota_mark_app_invalid_rollback_and_reboot();
            ESP_LOGE(TAG, "OTA 回滚失败: %s", esp_err_to_name(rollback_err));
            esp_restart();
        }
    } else {
        ESP_LOGI(TAG, "当前镜像无需 OTA 首次启动验证");
        if (has_boot_context) {
            boot_report_pending = strcmp(boot_ota_request.version, get_firmware_version()) == 0
                ? OTA_BOOT_REPORT_HEALTHY
                : OTA_BOOT_REPORT_ROLLED_BACK;
        }
    }

    // 2. 启动 WiFi STA 模式（连接成功后会在回调中启动 MQTT）
    wifi_init_sta();

    // 3. 创建业务线程；待验证镜像只有在业务线程实际启动后才允许确认
    if (xTaskCreate(vTaskSensor, "Task_Sensor", 8192, NULL, 3, NULL) != pdPASS) {
        ESP_LOGE(TAG, "传感器任务创建失败");
    }
}
