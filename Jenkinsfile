def sendDingTalk(String title, String message) {
    withCredentials([string(credentialsId: 'dingtalk-robot-token', variable: 'DINGTALK_TOKEN')]) {
        sh "./ci/notify-dingtalk.sh '${title}' '${message}'"
    }
}

pipeline {
    agent none

    options {
        skipDefaultCheckout(true)
        timestamps()
        preserveStashes(buildCount: 5)
        buildDiscarder(logRotator(numToKeepStr: '20', artifactNumToKeepStr: '10'))
    }

    environment {
        VERSION = "v1.0.${BUILD_NUMBER}"
        PROJECT_DIR = 'firmware/esp32-node'
        DIST_DIR = "dist/v1.0.${BUILD_NUMBER}"
        OTA_SERVER_HOST = '192.168.100.60'
    }

    stages {
        stage('Build and Verify Firmware') {
            agent {
                docker {
                    image 'espressif/idf:release-v5.5'
                    args '-u root'
                    reuseNode true
                }
            }
            steps {
                deleteDir()
                checkout scm

                script {
                    env.GIT_SHA = sh(returnStdout: true, script: 'git rev-parse HEAD').trim()
                    env.GIT_SHA_SHORT = sh(returnStdout: true, script: 'git rev-parse --short=12 HEAD').trim()
                }

                sh './ci/check-source.sh'
                sh './ci/build-firmware.sh "$PROJECT_DIR" "$VERSION"'
                sh './ci/verify-firmware.sh "$PROJECT_DIR" "$DIST_DIR" "$VERSION" "$GIT_SHA" "$BUILD_NUMBER" "$OTA_SERVER_HOST"'

                script {
                    env.FIRMWARE_SHA256 = sh(
                        returnStdout: true,
                        script: 'cut -d " " -f 1 "$DIST_DIR/firmware.sha256"'
                    ).trim()
                }

                archiveArtifacts artifacts: "${env.DIST_DIR}/**", fingerprint: true
                stash name: 'verified-firmware', includes: "${env.DIST_DIR}/**,ci/release-firmware.sh,ci/notify-dingtalk.sh"
                milestone ordinal: 10
            }
            post {
                success {
                    script {
                        try {
                            sendDingTalk(
                                'ESP32 固件待审批',
                                "版本: ${env.VERSION}<br>Commit: ${env.GIT_SHA_SHORT}<br>SHA-256: ${env.FIRMWARE_SHA256}<br>审批地址: ${env.BUILD_URL}input/"
                            )
                        } catch (err) {
                            echo "钉钉待审批通知发送失败，但固件仍保留在 Jenkins 归档中: ${err.message}"
                        }
                    }
                }
            }
        }

        stage('Approve OTA Release') {
            agent none
            options {
                timeout(time: 24, unit: 'HOURS')
            }
            input {
                message '是否发布已验证的 ESP32 固件？请先检查本次构建归档中的 manifest.json、检查报告和 SHA-256。'
                ok '批准 OTA 发布'
                submitter 'ota-release-approver,root'
                submitterParameter 'APPROVED_BY'
            }
            steps {
                echo "OTA release approved by ${env.APPROVED_BY}"
                milestone ordinal: 20
            }
        }

        stage('Release OTA') {
            agent any
            steps {
                deleteDir()
                unstash 'verified-firmware'
                sh './ci/release-firmware.sh "$DIST_DIR" "$VERSION"'

                script {
                    try {
                        sendDingTalk(
                            'ESP32 OTA 指令已发布',
                            "版本: ${env.VERSION}<br>SHA-256: ${env.FIRMWARE_SHA256}<br>状态: 固件已发布并下发 OTA 指令，设备最终结果仍待回执。<br>构建地址: ${env.BUILD_URL}"
                        )
                    } catch (err) {
                        echo "钉钉发布通知发送失败: ${err.message}"
                    }
                }
            }
        }
    }

    post {
        failure {
            script {
                node {
                    try {
                        checkout scm
                        sendDingTalk(
                            'ESP32 固件流水线失败',
                            "版本: ${env.VERSION}<br>状态: 构建、校验或发布阶段失败。<br>日志: ${env.BUILD_URL}console"
                        )
                    } catch (err) {
                        echo "钉钉失败通知发送失败: ${err.message}"
                    }
                }
            }
        }
        aborted {
            script {
                node {
                    try {
                        checkout scm
                        sendDingTalk(
                            'ESP32 OTA 发布已取消',
                            "版本: ${env.VERSION}<br>状态: 审批被拒绝、超时或旧构建已失效，未发布 OTA。<br>构建地址: ${env.BUILD_URL}"
                        )
                    } catch (err) {
                        echo "钉钉取消通知发送失败: ${err.message}"
                    }
                }
            }
        }
    }
}
