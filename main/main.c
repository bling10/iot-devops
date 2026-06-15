#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "esp_wifi.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "mqtt_client.h"

// ================= 1. 核心参数配置区 =================
// ⚠️ 测试阶段硬编码凭据，正式发布时应迁移至 menuconfig (Kconfig) 或 NVS 分区
#define WIFI_SSID      "wifi"                  // ⚠️ 确保是 2.4GHz 频段的热点
#define WIFI_PASS      "888888889999"          // ⚠️ WiFi 密码
#define MQTT_BROKER    "mqtt://192.168.249.60" // ⚠️ 虚拟机 IP（确保桥接模式，两端互通）
#define MQTT_TOPIC     "telemetry/sensors"
#define OTA_TOPIC      "device/esp32/ota"      // OTA 升级通知订阅主题

static const char *TAG = "IOT_NODE";

// 全局句柄与真实连接状态位
esp_mqtt_client_handle_t mqtt_client = NULL;
static volatile bool is_mqtt_connected = false; // 🟢 【修复】volatile 保证多任务/中断间可见性

// ================= 2. 网络状态机事件回调 =================

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
    }
}

// 处理 MQTT 连接状态
static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
    esp_mqtt_event_handle_t event = event_data;
    switch ((esp_mqtt_event_id_t)event_id) {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "🟢 MQTT 服务器连接成功！");
            is_mqtt_connected = true;  // 🟢 【关键】联通后，业务解锁开闸
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
            // TODO [任务三]: 在此解析 OTA JSON，提取 url/md5，触发 esp_https_ota 流式更新
            break;
        default:
            break;
    }
}

// ================= 3. 基础设施初始化 =================
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

// ================= 4. 业务线程区 =================

// 任务一：传感器采集与上报
void vTaskSensor(void *pvParameters) {
    char payload[128];
    float mock_temp = 25.0;
    float mock_hum = 50.0;

    while (1) {
        // 🔴 【核心修改】只认状态位，不认空指针
        if (is_mqtt_connected) {
            mock_temp += 0.5;
            if(mock_temp > 30.0) mock_temp = 25.0;

            snprintf(payload, sizeof(payload), "{\"temperature\": %.2f, \"humidity\": %.2f}", mock_temp, mock_hum);
            
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

// ================= 5. 系统主入口 =================
void app_main(void) {
    ESP_LOGI(TAG, "=========================================");
    ESP_LOGI(TAG, "🚀 嵌入式 IoT 终端启动 | 基础网络状态机打通");
    ESP_LOGI(TAG, "📦 当前固件版本: %s", "v1.1.0-webhook-test");
    ESP_LOGI(TAG, "=========================================");

    // 1. 初始化 NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // 2. 启动 WiFi STA 模式
    wifi_init_sta();
    
    // 3. 启动 MQTT 客户端
    mqtt_app_start();

    // 4. 创建业务线程
    xTaskCreate(vTaskSensor, "Task_Sensor", 8192, NULL, 3, NULL); // 栈深度提升至 8KB，防止 MQTT 内部调用栈溢出
}// trigger ota test
