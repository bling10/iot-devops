# IoT 项目核心架构图集 (Mermaid)

本篇档包含了可以直接渲染、也可以直接粘贴到您论文或 Markdown 编辑器中的 Mermaid 图表代码。

## 1. 物理拓扑架构图
展示了现实中硬件设备的连接层级。

```mermaid
graph LR
    subgraph "感知层 (Edge)"
        S1[温度传感器] --> ESP[ESP32 核心计算板]
        S2[湿度传感器] --> ESP
    end

    subgraph "传输层 (Network)"
        ESP -. "Wi-Fi (2.4GHz)" .-> Router[无线路由器]
        Router -. "TCP/IP" .-> Internet((公网/局域网))
    end

    subgraph "云端资源层 (Cloud)"
        Internet --> Host[Linux 服务器]
        Host --> Docker[Docker 守护进程]
    end

    subgraph "用户终端 (Client)"
        PC[PC 浏览器] --> Internet
        Phone[手机钉钉 App] --> Internet
    end
```

## 2. 整体软件架构图
纵向切层的微服务/容器化设计。

```mermaid
graph TB
    subgraph "表现层 (Presentation Layer)"
        Grafana[Grafana 面板]
        Ding[钉钉告警]
    end

    subgraph "数据持久层 (Storage Layer)"
        InfluxDB[(InfluxDB v2 时序数据库)]
        Nginx[(Nginx 固件文件库)]
    end

    subgraph "中间件层 (Middleware Layer)"
        Telegraf[Telegraf 采集引擎]
        Mosquitto[Mosquitto MQTT 消息总线]
        Jenkins[Jenkins CI/CD 服务]
    end

    subgraph "设备应用层 (Device Layer)"
        App[ESP32 FreeRTOS 应用]
    end

    Grafana -->|Flux 查询| InfluxDB
    Telegraf -->|定时写入| InfluxDB
    Mosquitto -->|订阅消费| Telegraf
    App -->|推送 JSON| Mosquitto
    Jenkins -->|分发固件| Nginx
    Jenkins -->|触发通知| Mosquitto
    Jenkins -->|Webhook| Ding
```

## 3. 核心数据流转图
分为上行链路（遥测）和下行链路（OTA），使用时序图展示最佳。

```mermaid
sequenceDiagram
    participant S as 传感器模块
    participant E as ESP32 节点
    participant M as MQTT Broker
    participant T as Telegraf
    participant I as InfluxDB
    participant J as Jenkins
    participant N as Nginx

    rect rgb(200, 220, 240)
    Note over S, I: 链路一：上行遥测数据流 (周期性)
    S ->> E: 读取温湿度 (周期 2s)
    E ->> E: 组装 JSON 报文
    E ->> M: Publish (Topic: telemetry/sensors)
    M ->> T: 触发数据推送
    T ->> T: 解析 JSON 并清洗
    T ->> I: 写入时序数据库
    end

    rect rgb(240, 220, 200)
    Note over J, E: 链路二：下行 OTA 固件分发流 (事件驱动)
    J ->> N: 编译完毕，推送 .bin 固件至目录
    J ->> M: 发布升级指令 (Topic: device/esp32/ota)
    M ->> E: 下发 OTA JSON 报文
    E ->> E: 校验版本号差异
    E ->> N: 发起 HTTP GET 请求
    N -->> E: 下发固件二进制流 (1KB Chunks)
    E ->> E: 双分区轮换写入并重启
    end
```

## 4. DevOps 自动化流水线架构图
展示了代码从提交到设备升级的全生命周期。

```mermaid
graph LR
    Dev([开发者]) -->|Git Push| Git[GitLab 源码库]
    Git -->|Webhook 触发| Jenkins[Jenkins CI 服务器]

    subgraph "Jenkins Pipeline (Docker Agent)"
        B[拉取代码] --> C[idf.py 编译]
        C -->|生成 bin| D[部署至 Nginx]
        D --> E[发送 MQTT 升级指令]
    end

    Jenkins --> B
    E --> Ding[钉钉成功/失败群告警]
    E -.-> ESP[ESP32 设备自动拉取升级]
```

## 5. Docker 容器网络隔离架构图
展示内部安全设计：哪些端口是对外暴露的，哪些是严格内网隔离的。

```mermaid
graph TB
    Internet((宿主机外部网络))

    Internet -->|访问大屏: 3000| P3000(宿主机端口 3000)
    Internet -->|拉取固件: 8081| P8081(宿主机端口 8081)
    Internet -->|设备通讯: 1883| P1883(宿主机端口 1883)

    subgraph "宿主机 (Host)"
        subgraph "Docker Bridge 网络"
            G[Grafana]
            N[iot-nginx]
            M[Mosquitto]
            T[Telegraf]
            I[(InfluxDB)]
        end

        P3000 ==> G
        P8081 ==> N
        P1883 ==> M
    end

    T -.->|纯内网通信| M
    T -.->|纯内网通信 8086| I
    G -.->|纯内网通信 8086| I

    classDef isolated fill:#f9f,stroke:#333,stroke-width:2px,stroke-dasharray: 5 5;
    class I,T isolated;
```

## 6. 嵌入式设备端软件架构图
展示 ESP32 内部基于 FreeRTOS 的层级和并发调度。

```mermaid
graph TB
    subgraph "应用层 (App Layer)"
        Main[app_main 主入口]
        TaskS[vTaskSensor 数据采集线程]
        TaskO[OTA_Task 空中升级线程]
        Timer[rollback_timer 看门狗定时器]
    end

    subgraph "系统服务层 (System Services)"
        WiFi_CB[WiFi Event Loop]
        MQTT_CB[MQTT Event Loop]
        NVS[NVS KV 存储管理]
        HTTP[HTTP Client]
    end

    subgraph "硬件抽象与外设层 (HAL & Drivers)"
        OTA_API[OTA 分区管理器]
        Flash[(SPI Flash A/B 分区)]
        TCP[LwIP TCP/IP 协议栈]
        GPIO[GPIO / ADC 控制器]
    end

    Main --> Timer
    Main --> TaskS
    Main --> WiFi_CB
    WiFi_CB --> MQTT_CB
    MQTT_CB --> TaskO

    TaskS --> GPIO
    TaskO --> HTTP
    HTTP --> TCP
    TaskO --> OTA_API
    OTA_API --> Flash
```
