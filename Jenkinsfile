pipeline {
    agent {
        docker {
            image 'espressif/idf:release-v5.5'
            args '-u root -v /data/devops/nginx/html:/opt/nginx-data'
        }
    }
    
    environment {
        PROJECT_DIR = "${WORKSPACE}"
        VERSION = "v1.0.${BUILD_NUMBER}"
    }

    stages {
        stage('Build Firmware') {
            steps {
                echo 'Building firmware using ESP-IDF...'
                sh '''
                    . $IDF_PATH/export.sh
                    cd ${PROJECT_DIR}
                    # 将固件版本号同步为 Jenkins 构建版本（用于 OTA 版本比对）
                    sed -i "s/#define FIRMWARE_VERSION.*/#define FIRMWARE_VERSION   \\"${VERSION}\\"/" main/main.c
                    echo "📦 固件版本已同步为: ${VERSION}"
                    rm -rf build/
                    idf.py set-target esp32
                    idf.py build
                '''
            }
        }
        
        stage('Deploy to Nginx') {
            steps {
                echo 'Copying firmware.bin to Nginx server...'
                sh '''
                    cp ${PROJECT_DIR}/build/sample_project.bin /opt/nginx-data/firmware_${VERSION}.bin
                    echo "MD5 Checksum:"
                    md5sum /opt/nginx-data/firmware_${VERSION}.bin
                '''
            }
        }
        
        stage('MQTT Notification') {
            steps {
                echo 'Triggering OTA via MQTT...'
                sh '''
                    FIRMWARE_MD5=$(md5sum /opt/nginx-data/firmware_${VERSION}.bin | awk '{print $1}')
                    cat <<EOF > ota_notify.json
{
  "version": "${VERSION}",
  "url": "http://192.168.249.60:8081/firmware_${VERSION}.bin",
  "md5": "${FIRMWARE_MD5}"
}
EOF
                    # Install Mosquitto clients if not already present
                    if ! command -v mosquitto_pub &> /dev/null; then
                        apt-get update && apt-get install -y mosquitto-clients
                    fi
                    
                    # Publish MQTT message
                    mosquitto_pub -h 192.168.249.60 -p 1883 -t "device/esp32/ota" -f ota_notify.json
                '''
            }
        }
    }
    post {
        success {
            echo '✅ Pipeline execution successful, sending DingTalk notification...'
            sh """
                curl -H 'Content-Type: application/json' \\
                     -d '{"msgtype": "markdown", "markdown": {"title":"🚀 OTA 固件发布成功", "text":"### 🚀 ESP32 OTA 固件发布成功\\n\\n**版本号:** `${VERSION}`\\n\\n**状态:** ✅ 构建与推送完成\\n\\n设备即将开始自动拉取升级。"}}' \\
                     https://oapi.dingtalk.com/robot/send?access_token=b81864f4a2086e542dca30e11216c5a2c7f46c9d8489c816dfa136b0b3934318
            """
        }
        failure {
            echo '❌ Pipeline execution failed, sending DingTalk alert...'
            sh """
                curl -H 'Content-Type: application/json' \\
                     -d '{"msgtype": "markdown", "markdown": {"title":"💥 固件构建失败", "text":"### 💥 ESP32 固件构建失败\\n\\n**版本号:** `${VERSION}`\\n\\n**状态:** ❌ 编译不通过或环境异常\\n\\n请开发人员立即检查 Jenkins 日志！"}}' \\
                     https://oapi.dingtalk.com/robot/send?access_token=b81864f4a2086e542dca30e11216c5a2c7f46c9d8489c816dfa136b0b3934318
            """
        }
    }
}
