# 实验室兼容工具

此目录保留原有 NFS/Telnet 调试通路，独立于生产代理与发行。工具被调用时会真实操作指定板卡，`--activate` 会由实验工具生成测试安全状态，`--reboot` 会重启目标板；不能把实验脚本当成生产安全授权来源。本次没有调用这些板端操作。

配置 `config/host.local.conf` 的 LAB_BOARD_IP、LAB_DEVICE_ID、LAB_BOARD_LIST、NFS_SERVER、NFS_EXPORT_ROOT 和外部 SIGN_KEY。可用 devices.local.conf 做单机覆盖。没有默认板卡或生产设备列表。主机需要已经配置 NFS 导出和对应网络访问。

```sh
make lab-control
compat/lab/ota-lab.sh --source /path/to/deploy --board BOARD_IP --version V1.0.0
```

`ota-board-setup.sh` 从 `.build/lab-control` 的平面控制包部署，保留原受限 rootfs 下从 `/mnt/local/ota` bind-mount 控制程序的方式，默认停止网络代理并启用开发模式。控制包需要位于 NFS_EXPORT_ROOT 之下。如需在板端持久生效，要确认启动流程包含 ota-bind-mounts.sh。重刷生产 rootfs 时应去除这些实验覆盖。

`ota-dev-setup.sh` 仍是旧式实验主机系统准备工具，会安装软件包、修改 `/etc/exports`、重载 NFS、修改 shell PATH；可先 `--dry-run`。它不属于生产部署，独立提取后不再自动通过 SCP 获取签名私钥，必须使用预先配置的外部测试密钥。

`--local` 保留了从外部 SDK 项目中寻找 board/jmapp 的便利路径；独立项目推荐始终指定 `--source`。实际业务应用和模型都由外部构建输出提供。
