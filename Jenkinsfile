/*
 * 流水线主流程：
 * 1. 检出源码，完成源码检查、固件编译和制品校验。
 * 2. 归档已验证固件，并等待指定人员人工审批。
 * 3. 审批通过后发布固件，通过 MQTT 下发 OTA 指令。
 * 4. 本流水线只负责发布指令；设备升级的最终结果需要由设备状态回执确认。
 */

// 统一封装钉钉通知，并从 Jenkins Credentials 中临时读取机器人 Token。
// Token 只在 withCredentials 作用域内可用，不写入仓库。
def sendDingTalk(String title, String message) {
    withCredentials([string(credentialsId: 'dingtalk-robot-token', variable: 'DINGTALK_TOKEN')]) {
        sh "./ci/notify-dingtalk.sh '${title}' '${message}'"
    }
}

// 仅用于审批和通知展示；发布脚本仍负责执行严格校验，避免界面展示逻辑成为安全边界。
def normalizeTargetIds(String rawTargetIds) {
    return (rawTargetIds ?: '')
        .split(/[\s,]+/)
        .findAll { it }
        .unique()
}

pipeline {
    // 顶层不长期占用执行节点，每个阶段根据需要单独申请运行环境。
    agent none

    parameters {
        text(
            name: 'OTA_TARGET_DEVICE_IDS',
            defaultValue: 'sim-001\nsim-003',
            description: '目标设备 ID，支持逗号、空格或换行分隔；设备 ID 只能包含小写字母、数字和连字符。'
        )
        booleanParam(
            name: 'OTA_RELEASE_DRY_RUN',
            defaultValue: true,
            description: '开启时只校验固件、指令和目标 Topic，不复制固件且不发送 MQTT 消息。'
        )
    }

    options {
        // 代码由构建阶段显式检出，避免 Jenkins 在不合适的节点上自动检出一次。
        skipDefaultCheckout(true)
        // 为控制台日志增加时间戳，方便计算构建、审批和发布耗时。
        timestamps()
        // 保留最近 5 次构建的 stash，使流水线重启后仍可继续使用已验证制品。
        preserveStashes(buildCount: 5)
        // 限制历史构建和归档制品数量，避免 Jenkins 磁盘持续增长。
        buildDiscarder(logRotator(numToKeepStr: '20', artifactNumToKeepStr: '10'))
    }

    environment {
        // BUILD_NUMBER 由 Jenkins 自动生成，用它保证每次构建的版本号和目录唯一。
        VERSION = "v1.0.${BUILD_NUMBER}"
        PROJECT_DIR = 'firmware/esp32-node'
        DIST_DIR = "dist/v1.0.${BUILD_NUMBER}"
        // 该地址会写入发布清单和 OTA 通知，设备必须能够访问它。
        OTA_SERVER_HOST = '192.168.100.60'
    }

    stages {
        stage('Build and Verify Firmware') {
            agent {
                docker {
                    // 在固定 ESP-IDF 版本的容器中编译，减少不同机器环境造成的结果差异。
                    image 'espressif/idf:release-v5.5'
                    // 与 Jenkins 宿主进程使用相同 UID/GID，避免生成 root 所有的工作区文件。
                    args '-u 1000:1000 -e HOME=/tmp'
                    // 容器复用当前 Jenkins 工作区，构建结果可直接归档和暂存。
                    reuseNode true
                }
            }
            steps {
                // 清理旧构建残留后重新检出本次提交，保证输入可重复。
                deleteDir()
                checkout scm

                // Docker 容器内的 Git 可能认为挂载目录所有者不可信，只放行当前工作区。
                sh 'git config --global --add safe.directory "$WORKSPACE"'

                script {
                    // 完整 Commit 用于发布追溯，短 Commit 用于通知和 release_id 展示。
                    env.GIT_SHA = sh(returnStdout: true, script: 'git rev-parse HEAD').trim()
                    env.GIT_SHA_SHORT = sh(returnStdout: true, script: 'git rev-parse --short=12 HEAD').trim()
                }

                // 依次执行源码基础检查、协议测试、发布脚本行为测试、ESP-IDF 编译和固件检查。
                // 任一脚本返回非 0，流水线立即停止，不进入人工审批。
                sh './ci/check-source.sh'
                sh 'PYTHONDONTWRITEBYTECODE=1 python3 -B -m unittest discover -s tests -p "test_*.py" -v'
                sh 'bash tests/test-release-firmware.sh'
                sh './ci/build-firmware.sh "$PROJECT_DIR" "$VERSION"'
                sh './ci/verify-firmware.sh "$PROJECT_DIR" "$DIST_DIR" "$VERSION" "$GIT_SHA" "$BUILD_NUMBER" "$OTA_SERVER_HOST"'

                script {
                    def targetIds = normalizeTargetIds(params.OTA_TARGET_DEVICE_IDS)
                    env.OTA_TARGET_DISPLAY = targetIds.join(', ')
                    env.OTA_TARGET_COUNT = targetIds.size().toString()

                    // 构建阶段固定执行 dry-run，让非法或空设备列表在进入人工审批前失败。
                    withEnv([
                        "OTA_TARGET_DEVICE_IDS=${params.OTA_TARGET_DEVICE_IDS}",
                        'RELEASE_DRY_RUN=1'
                    ]) {
                        sh './ci/release-firmware.sh "$DIST_DIR" "$VERSION"'
                    }

                    // 保存已验证固件的摘要，供审批通知和发布通知复用。
                    env.FIRMWARE_SHA256 = sh(
                        returnStdout: true,
                        script: 'cut -d " " -f 1 "$DIST_DIR/firmware.sha256"'
                    ).trim()
                }

                // archiveArtifacts 面向人工下载和长期追溯；stash 用于后续阶段传递同一份制品。
                archiveArtifacts artifacts: "${env.DIST_DIR}/**", fingerprint: true
                stash name: 'verified-firmware', includes: "${env.DIST_DIR}/**,ci/release-firmware.sh,ci/notify-dingtalk.sh"

                // 当新构建到达相同里程碑时，较旧且仍停留在前面阶段的构建会失效。
                milestone ordinal: 10
            }
            post {
                success {
                    script {
                        try {
                            // 这里只通知固件待审批，不代表 OTA 已经发布或设备升级成功。
                            sendDingTalk(
                                'ESP32 固件待审批',
                                "版本: ${env.VERSION}<br>Commit: ${env.GIT_SHA_SHORT}<br>SHA-256: ${env.FIRMWARE_SHA256}<br>目标设备(${env.OTA_TARGET_COUNT}): ${env.OTA_TARGET_DISPLAY}<br>发布模式: ${params.OTA_RELEASE_DRY_RUN ? 'dry-run' : 'publish'}<br>审批地址: ${env.BUILD_URL}input/"
                            )
                        } catch (err) {
                            // 通知失败不销毁已归档制品，审批人仍可从 Jenkins 页面检查构建结果。
                            echo "钉钉待审批通知发送失败，但固件仍保留在 Jenkins 归档中: ${err.message}"
                        }
                    }
                }
            }
        }

        stage('Approve OTA Release') {
            // 等待人工输入期间不占用 Jenkins executor。
            agent none
            options {
                // 24 小时无人审批则终止本次发布，避免过期制品被误发布。
                timeout(time: 24, unit: 'HOURS')
            }
            input {
                message "是否处理已验证的 ESP32 固件？目标设备: ${env.OTA_TARGET_DISPLAY}；请先检查 manifest.json、检查报告和 SHA-256。"
                ok '批准 OTA 发布'
                // 只有列出的 Jenkins 用户可以批准；审批人写入 APPROVED_BY。
                submitter 'ota-release-approver,root'
                submitterParameter 'APPROVED_BY'
            }
            steps {
                echo "OTA release approved by ${env.APPROVED_BY}"
                // 阻止较旧构建在新构建已经获批后继续发布。
                milestone ordinal: 20
            }
        }

        stage('Release OTA') {
            // 发布不需要 ESP-IDF 工具链，使用任意可运行 Shell 和 Docker 命令的节点即可。
            agent any
            steps {
                // 发布阶段可能运行在另一节点，因此清理工作区后取回构建阶段暂存的已验证制品。
                deleteDir()
                unstash 'verified-firmware'

                script {
                    // dry-run 只验证目标 Topic；正式模式才复制固件并逐设备发布 MQTT 指令。
                    withEnv([
                        "OTA_TARGET_DEVICE_IDS=${params.OTA_TARGET_DEVICE_IDS}",
                        "RELEASE_DRY_RUN=${params.OTA_RELEASE_DRY_RUN ? '1' : '0'}"
                    ]) {
                        sh './ci/release-firmware.sh "$DIST_DIR" "$VERSION"'
                    }

                    try {
                        def notificationTitle = params.OTA_RELEASE_DRY_RUN ? 'ESP32 OTA 发布演练完成' : 'ESP32 OTA 指令已发布'
                        def releaseStatus = params.OTA_RELEASE_DRY_RUN ? 'dry-run 已通过，未复制固件且未发送 MQTT 指令。' : '固件已发布并逐设备下发 OTA 指令，设备最终结果仍待回执。'
                        sendDingTalk(
                            notificationTitle,
                            "版本: ${env.VERSION}<br>SHA-256: ${env.FIRMWARE_SHA256}<br>目标设备(${env.OTA_TARGET_COUNT}): ${env.OTA_TARGET_DISPLAY}<br>状态: ${releaseStatus}<br>构建地址: ${env.BUILD_URL}"
                        )
                    } catch (err) {
                        echo "钉钉发布通知发送失败: ${err.message}"
                    }
                }
            }
        }
    }

    post {
        // 构建、校验或发布阶段发生失败时发送统一失败通知。
        failure {
            script {
                node {
                    try {
                        // 顶层 agent 为 none，此处需要申请节点并检出通知脚本。
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
        // 人工拒绝、审批超时或 milestone 使旧构建失效时，进入 aborted 分支。
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
