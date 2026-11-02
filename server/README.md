# OTA 服务端集成

服务端依赖已有 Nginx 和 Mosquitto；本项目提供配置模板与发布工具，不自动部署、申请证书、重载服务或更改 NFS 导出。

- `nginx.conf.example`：HTTPS/mTLS 下载只读目录，需替换域名、证书和文件目录。
- `mosquitto.conf.example` / `mosquitto.acl.example`：设备用独立证书；证书 CN 为设备 ID，发布者 CN 为 `publisher`。设备只订阅自己的 Campaign。
- `host/release/publish_network_ota.sh`：校验签名与散列，发布不可变目录，写设备 notice，探测 HTTPS/mTLS，再发 MQTT QoS 1 retained 消息。

证书签发、设备烧录和权限分配属于部署环境。签名私钥只在发布环境；设备公钥只信任独立配置的根，不能用包里的公钥替换。TLS CA 与固件签名密钥是不同用途。设备私钥、CA 私钥、发布者私钥均未复制进本项目。

发布进程需能写 WEB_ROOT 与 PUBLISH_AUDIT_DIR；Nginx 进程需能读取成品文件。跨用户运行时设置 `PUBLISH_OWNER=publisher:www-data` 并配置相应权限；不需要时留空。所有服务器参数见 `config/host.conf.example`。修改模板后由实际部署环境做 `nginx -t` 和 Broker ACL 验证；本次未执行在线服务变更。
