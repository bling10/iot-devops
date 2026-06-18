#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "esp_wifi.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"

#include "esp_log.h"
#include "mqtt_client.h"
// ===== OTA 新增头文件 =====
#include "esp_ota_ops.h"        // OTA 操作 API：写分区、设置启动分区
#include "esp_http_client.h"    // HTTP 客户端：从 Nginx 下载固件
#include "esp_app_desc.h"       // 读取当前固件版本描述
#include "cJSON.h"              // 解析 Jenkins 推送的 OTA JSON 消息

void mqtt_app_start(void);      // 前置声明

// ================= 1. 核心参数配置区 =================
// ⚠️ 测试阶段硬编码凭据，正式发布时应迁移至 menuconfig (Kconfig) 或 NVS 分区
#define WIFI_SSID          "wifi"                  // ⚠️ 确保是 2.4GHz 频段的热点
#define WIFI_PASS          "888888889999"          // ⚠️ WiFi 密码
#define MQTT_BROKER        "mqtt://192.168.249.60" // ⚠️ 虚拟机 IP（确保桥接模式，两端互通）
#define MQTT_TOPIC         "telemetry/sensors"
#define OTA_TOPIC          "device/esp32/ota"      // OTA 升级通知订阅主题
#define FIRMWARE_VERSION   "v1.0.0"                // 固件版本号（Jenkins 编译时会自动替换）
#define OTA_BUF_SIZE       1024                    // OTA 下载缓冲区大小

static const char *TAG = "IOT_NODE";

// 全局句柄与真实连接状态位
esp_mqtt_client_handle_t mqtt_client = NULL;
static volatile bool is_mqtt_connected = false; // 🟢 【修复】volatile 保证多任务/中断间可见性
static volatile bool is_ota_in_progress = false; // 🔒 OTA 进行中标志，防止重复触发
static char ota_url[256] = {0};                   // 缓存 OTA 固件下载地址

// ⏱️ 防变砖看门狗定时器
TimerHandle_t rollback_timer = NULL;

static void rollback_timer_cb(TimerHandle_t xTimer) {
    ESP_LOGE(TAG, "💀 新固件 3 分钟内未能连上 MQTT，判定为死锁/断网！触发 App Rollback 强制回退！");
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
static void ota_task(void *pvParameters)
{
    ESP_LOGI(TAG, "🚀 ========== OTA 升级任务启动 ==========");
    ESP_LOGI(TAG, "📥 固件下载地址: %s", ota_url);

    esp_err_t err;

    // ---------- 1. 配置 HTTP 客户端 ----------
    esp_http_client_config_t http_config = {
        .url = ota_url,
        .timeout_ms = 30000,  // 30 秒超时，防止网络卡死
    };
    esp_http_client_handle_t client = esp_http_client_init(&http_config);
    if (client == NULL) {
        ESP_LOGE(TAG, "❌ HTTP 客户端初始化失败！");
        is_ota_in_progress = false;
        vTaskDelete(NULL);
        return;
    }

    // ---------- 2. 打开 HTTP 连接 ----------
    err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "❌ HTTP 连接失败: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        is_ota_in_progress = false;
        vTaskDelete(NULL);
        return;
    }

    int content_length = esp_http_client_fetch_headers(client);
    int status_code = esp_http_client_get_status_code(client);
    ESP_LOGI(TAG, "📊 HTTP 状态码: %d | 固件大小: %d 字节", status_code, content_length);

    if (status_code != 200) {
        ESP_LOGE(TAG, "❌ HTTP 请求异常，状态码: %d", status_code);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        is_ota_in_progress = false;
        vTaskDelete(NULL);
        return;
    }

    // ---------- 3. 获取下一个可用的 OTA 分区 ----------
    const esp_partition_t *update_partition = esp_ota_get_next_update_partition(NULL);
    if (update_partition == NULL) {
        ESP_LOGE(TAG, "❌ 找不到可用的 OTA 分区！请检查分区表配置");
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        is_ota_in_progress = false;
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "📦 目标 OTA 分区: %s (地址: 0x%08"PRIx32", 大小: %"PRIu32" 字节)",
             update_partition->label, update_partition->address, update_partition->size);

    // ---------- 4. 开始 OTA 写入 ----------
    esp_ota_handle_t ota_handle;
    err = esp_ota_begin(update_partition, OTA_WITH_SEQUENTIAL_WRITES, &ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "❌ OTA 写入初始化失败: %s", esp_err_to_name(err));
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        is_ota_in_progress = false;
        vTaskDelete(NULL);
        return;
    }

    // ---------- 5. 循环读取 HTTP 数据流并逐块写入 Flash ----------
    char *buf = malloc(OTA_BUF_SIZE);
    if (buf == NULL) {
        ESP_LOGE(TAG, "❌ 内存分配失败！");
        esp_ota_abort(ota_handle);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        is_ota_in_progress = false;
        vTaskDelete(NULL);
        return;
    }

    int total_read = 0;
    int read_len;
    int last_progress = -1;

    while (1) {
        read_len = esp_http_client_read(client, buf, OTA_BUF_SIZE);
        if (read_len < 0) {
            ESP_LOGE(TAG, "❌ HTTP 数据读取出错！");
            break;
        }
        if (read_len == 0) {
            // 数据读取完毕
            if (esp_http_client_is_complete_data_received(client)) {
                ESP_LOGI(TAG, "✅ 固件数据接收完毕！");
            } else {
                ESP_LOGE(TAG, "❌ 连接中断，数据不完整！");
            }
            break;
        }

        // 写入 Flash
        err = esp_ota_write(ota_handle, buf, read_len);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "❌ Flash 写入失败: %s", esp_err_to_name(err));
            break;
        }

        total_read += read_len;

        // 每 10% 打印一次进度
        if (content_length > 0) {
            int progress = (total_read * 100) / content_length;
            if (progress / 10 != last_progress / 10) {
                last_progress = progress;
                ESP_LOGI(TAG, "⏳ OTA 下载进度: %d%% (%d/%d 字节)", progress, total_read, content_length);
            }
        }
    }

    free(buf);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    // ---------- 6. 完成 OTA 写入 ----------
    if (err == ESP_OK && esp_http_client_is_complete_data_received(client) == false && total_read > 0) {
        // 部分情况下 chunked encoding 不会设 content_length，但数据可能已完整
        ESP_LOGW(TAG, "⚠️ 数据接收标志未置位，但已读取 %d 字节，继续尝试完成 OTA", total_read);
    }

    err = esp_ota_end(ota_handle);
    if (err != ESP_OK) {
        if (err == ESP_ERR_OTA_VALIDATE_FAILED) {
            ESP_LOGE(TAG, "❌ 固件校验失败！镜像可能已损坏");
        } else {
            ESP_LOGE(TAG, "❌ OTA 结束失败: %s", esp_err_to_name(err));
        }
        is_ota_in_progress = false;
        vTaskDelete(NULL);
        return;
    }

    // ---------- 7. 设置新分区为下次启动分区 ----------
    err = esp_ota_set_boot_partition(update_partition);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "❌ 设置启动分区失败: %s", esp_err_to_name(err));
        is_ota_in_progress = false;
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "🎉 ========== OTA 升级成功！==========");
    ESP_LOGI(TAG, "📦 新固件已写入分区: %s", update_partition->label);
    ESP_LOGI(TAG, "🔄 3 秒后自动重启...");

    vTaskDelay(pdMS_TO_TICKS(3000));
    esp_restart();
}

/**
 * 解析 OTA 通知 JSON 并启动升级任务
 * 
 * 预期 JSON 格式（由 Jenkins 流水线生成）：
 * {
 *   "version": "v1.0.12",
 *   "url": "http://192.168.249.60:8081/firmware_v1.0.12.bin",
 *   "md5": "d41d8cd98f00b204e9800998ecf8427e"
 * }
 */
static void handle_ota_message(const char *data, int data_len)
{
    // 防止重复触发
    if (is_ota_in_progress) {
        ESP_LOGW(TAG, "⚠️ OTA 升级正在进行中，忽略重复通知");
        return;
    }

    // 安全拷贝 JSON 数据（确保以 \0 结尾）
    char *json_str = malloc(data_len + 1);
    if (json_str == NULL) {
        ESP_LOGE(TAG, "❌ 内存分配失败，无法解析 OTA 消息");
        return;
    }
    memcpy(json_str, data, data_len);
    json_str[data_len] = '\0';

    // 解析 JSON
    cJSON *root = cJSON_Parse(json_str);
    free(json_str);

    if (root == NULL) {
        ESP_LOGE(TAG, "❌ OTA JSON 解析失败！");
        return;
    }

    cJSON *version_item = cJSON_GetObjectItem(root, "version");
    cJSON *url_item = cJSON_GetObjectItem(root, "url");

    if (!cJSON_IsString(version_item) || !cJSON_IsString(url_item)) {
        ESP_LOGE(TAG, "❌ OTA JSON 格式错误：缺少 version 或 url 字段");
        cJSON_Delete(root);
        return;
    }

    const char *new_version = version_item->valuestring;
    const char *new_url = url_item->valuestring;

    ESP_LOGI(TAG, "📋 收到 OTA 升级通知:");
    ESP_LOGI(TAG, "   远端版本: %s", new_version);
    ESP_LOGI(TAG, "   当前版本: %s", FIRMWARE_VERSION);
    ESP_LOGI(TAG, "   固件地址: %s", new_url);

    // 版本比对：如果版本号相同则跳过
    if (strcmp(new_version, FIRMWARE_VERSION) == 0) {
        ESP_LOGW(TAG, "⚠️ 远端版本与当前版本一致 (%s)，跳过升级", FIRMWARE_VERSION);
        cJSON_Delete(root);
        return;
    }

    // 保存 URL 并启动 OTA 任务
    strncpy(ota_url, new_url, sizeof(ota_url) - 1);
    ota_url[sizeof(ota_url) - 1] = '\0';

    is_ota_in_progress = true;
    cJSON_Delete(root);

    ESP_LOGI(TAG, "🔧 正在启动 OTA 下载任务...");
    // 栈深度 8KB，OTA 任务优先级设为 5（高于传感器任务的 3）
    xTaskCreate(ota_task, "OTA_Task", 8192, NULL, 5, NULL);
}

// ================= 3. 网络状态机事件回调 =================

// 处理 WiFi 连接状态
static void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "❌ WiFi 断开，3秒后尝试重连...");
        is_mqtt_connected = false; // WiFi 断开，MQTT 必然失效
        vTaskDelay(pdMS_TO_TICKS(3000)); // 【优化】延时 3 秒再连，防止疯狂空转死循环
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "🔹 WiFi 连接成功! 获取到本地 IP: " IPSTR, IP2STR(&event->ip_info.ip));
        
        // 【核心修改】确保获取到 IP 后才启动/恢复 MQTT
        if (mqtt_client == NULL) {
            ESP_LOGI(TAG, "🚀 网络就绪，首次启动 MQTT 客户端...");
            mqtt_app_start();
        }
    }
}

// 处理 MQTT 连接状态
static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
    esp_mqtt_event_handle_t event = event_data;
    switch ((esp_mqtt_event_id_t)event_id) {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "🟢 MQTT 服务器连接成功！");
            is_mqtt_connected = true;  // 🟢 【关键】联通后，业务解锁开闸
            
            // 【自救拆弹】连上云端说明新固件网络栈健康，取消回滚并标记为合法！
            if (rollback_timer != NULL) {
                xTimerStop(rollback_timer, 0);
                ESP_LOGI(TAG, "💣 已停止变砖看门狗定时器。");
            }
            esp_ota_mark_app_valid_cancel_rollback();
            ESP_LOGI(TAG, "✅ 当前固件已被标记为稳定版 (VALID)！");

            // 订阅 OTA 升级通知主题，用于接收 Jenkins 流水线下发的固件更新 JSON
            esp_mqtt_client_subscribe(mqtt_client, OTA_TOPIC, 1);
            ESP_LOGI(TAG, "📥 已订阅 OTA 主题: %s", OTA_TOPIC);
            break;
        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "🔴 MQTT 服务器已断开！");
            is_mqtt_connected = false; // 🔴 【关键】断开后，业务立刻上锁挂起
            break;
        case MQTT_EVENT_DATA:
            ESP_LOGI(TAG, "📨 收到 MQTT 消息 -> 主题: %.*s", event->topic_len, event->topic);
            ESP_LOGI(TAG, "📨 消息内容: %.*s", event->data_len, event->data);

            // 🔧 判断是否为 OTA 升级通知
            if (event->topic_len == strlen(OTA_TOPIC) &&
                strncmp(event->topic, OTA_TOPIC, event->topic_len) == 0) {
                ESP_LOGI(TAG, "🔔 检测到 OTA 升级通知，开始处理...");
                handle_ota_message(event->data, event->data_len);
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
    };
    mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_register_event(mqtt_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(mqtt_client);
}

// ================= 5. 业务线程区 =================

// 任务一：传感器采集与上报
void vTaskSensor(void *pvParameters) {
    char payload[256];
    float mock_temp = 99.0;
    float mock_hum = 50.0;

    while (1) {
        // 🔴 【核心修改】只认状态位，不认空指针
        if (is_mqtt_connected) {
            mock_temp += 0.5;
            if(mock_temp > 30.0) mock_temp = 25.0;

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

            snprintf(payload, sizeof(payload), 
                "{\"temperature\": %.2f, \"humidity\": %.2f, \"rssi\": %d, \"free_heap\": %lu, \"uptime_sec\": %lu, \"version\": \"%s\"}", 
                mock_temp, mock_hum, rssi, free_heap, uptime_sec, FIRMWARE_VERSION);
            
            int msg_id = esp_mqtt_client_publish(mqtt_client, MQTT_TOPIC, payload, 0, 1, 0);
            if (msg_id < 0) {
                ESP_LOGE(TAG, "❌ 数据上报失败！将在下个周期重试");
            } else {
                ESP_LOGI(TAG, "📡 成功上报数据 -> msg_id=%d | 报文: %s", msg_id, payload);
            }
        } else {
            // 网络断开时静默等待，拒绝盲目向死连接发数据
            ESP_LOGW(TAG, "⚠️ [业务挂起] 当前网络未就绪，暂停采集上报...");
        }
        
        // 保持 2 秒采样周期
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

// ================= 6. 系统主入口 =================
void app_main(void) {
    ESP_LOGI(TAG, "=========================================");
    ESP_LOGI(TAG, "🚀 嵌入式 IoT 终端启动 | OTA 空中升级已就绪");
    ESP_LOGI(TAG, "📦 当前固件版本: %s", FIRMWARE_VERSION);

    // 打印当前运行的 OTA 分区信息
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (running != NULL) {
        ESP_LOGI(TAG, "💾 当前运行分区: %s (地址: 0x%08"PRIx32")", running->label, running->address);
    }
    ESP_LOGI(TAG, "=========================================");

    // 0. 启动防变砖生命周期看门狗 (3 分钟)
    ESP_LOGI(TAG, "⏱️ 启动防变砖看门狗，如果 3 分钟内未连上 MQTT，将自动回退到上一版固件！");
    rollback_timer = xTimerCreate("rollback_timer", pdMS_TO_TICKS(180000), pdFALSE, NULL, rollback_timer_cb);
    if (rollback_timer != NULL) {
        xTimerStart(rollback_timer, 0);
    } else {
        ESP_LOGE(TAG, "❌ 无法创建防变砖定时器，系统内存不足，主动重启回滚...");
        esp_restart();
    }

    // 1. 初始化 NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // 2. 启动 WiFi STA 模式（连接成功后会在回调中启动 MQTT）
    wifi_init_sta();

    // 4. 创建业务线程
    xTaskCreate(vTaskSensor, "Task_Sensor", 8192, NULL, 3, NULL); // 栈深度提升至 8KB，防止 MQTT 内部调用栈溢出
}
