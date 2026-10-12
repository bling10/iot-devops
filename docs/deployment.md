# 部署目录说明

项目将可版本控制的配置与运行数据分开保存：

```text
/data/devops/iot-devops   Git 仓库
/data/devops/runtime      容器持久化数据
/data/devops/artifacts    固件制品
/data/devops/secrets      本机环境变量与秘密配置
```

复制 `.env.example` 的变量到 `/data/devops/secrets/iot-devops.env`，填入本机实际值后，通过 `scripts/start.sh` 启动服务。

不要将 `runtime`、`artifacts` 或 `secrets` 添加到 Git 仓库。
