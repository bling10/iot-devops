#!/bin/bash

echo "开始模拟 ESP32 传感器数据推送..."
echo "按 Ctrl+C 可以停止模拟"

# 发送设备上线状态
docker exec iot-mqtt mosquitto_pub -t 'device/esp32/status' -m "online"

while true; do
  # 生成 20.0 到 30.0 之间的随机温度 (带1位小数)
  TEMP=$(awk -v min=200 -v max=300 'BEGIN{srand(); printf "%.1f", (min+rand()*(max-min))/10}')

  # 生成 40.0 到 60.0 之间的随机湿度 (带1位小数)
  HUM=$(awk -v min=400 -v max=600 'BEGIN{srand(); printf "%.1f", (min+rand()*(max-min))/10}')

  PAYLOAD="{\"temperature\": $TEMP, \"humidity\": $HUM}"

  # 通过 mosquitto_pub 发布消息
  docker exec iot-mqtt mosquitto_pub -t 'telemetry/esp32' -m "$PAYLOAD"

  echo "已推送数据: $PAYLOAD"

  # 每 2 秒推送一次
  sleep 2
done
