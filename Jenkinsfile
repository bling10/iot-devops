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
}
