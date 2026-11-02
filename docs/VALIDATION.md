# 验证结果

日期：2026-11-02（北京时间）。构建与测试位置：`logic@192.168.1.216:/home/logic/1221/app-ota-standalone`。所有试验使用临时应用目录、临时签名密钥、临时设备状态和回环地址服务。

## 已通过

- Linux x86_64：独立 `make native -j4` 成功。构建真实 mbedTLS 验签、Paho MQTT/TLS 客户端、curl HTTPS 客户端，使用项目内固定版本源码归档，未依赖 SDK 编译产物。
- GK1221 ARM：使用原项目提供的外部 `arm-gcc12.2.0-linux-` 工具链，独立 BUILD_DIR 交叉构建三项原生工具成功。产物是 ELF32 ARM EABI5/ARMv7；MQTT 与 HTTP 使用 `/lib/ld-linux-armhf.so.3`，验签程序静态链接。动态加载器与原 demo_ai 一致；这仅确认构建 ABI，尚未替代实板执行验证。
- 最终 `make check`：**25/25 通过，无跳过，29.559秒**。
- 全套 shell 入口语法检查由测试集运行。
- 生成 rootfs 覆盖层的校验：必须提供外部信任公钥；代理配置默认 ENABLED=0、权限0600；拒绝安装到执行主机根目录。

## 测试覆盖

1. 真实 P-256/SHA-256 签名和验签，元数据或签名损坏拒绝。
2. 正常 `ACTIVE → STAGED → TRIAL_ARMED → TRIAL → ACTIVE`。
3. 试运行同一次启动重复 select 的幂等性，跨启动超限自动回退。
4. 产品、硬件、ABI、格式不匹配，重复元数据字段、散列错误、额外文件、符号链接拒绝。
5. 缺失、陈旧、重复安全状态和更新禁止标志阻止激活；无授权或事务不匹配不允许 arm。
6. 试运行候选不能被另一次安装覆盖，竞争事务被锁拒绝；相同/更低发行计数不能重装。
7. 新状态记录损坏或语义不完整时读取另一份；双记录损坏后从本地槽恢复版本和发行计数；恢复排除已 quarantine 的槽。
8. GK1221 业务健康日志持续帧进展后确认；进程退出导致 mark-bad，测试显式禁止 reboot。
9. 发行计数跨版本持续递增，发布失败计数保留，空生产目标列表拒绝执行。
10. 打包拒绝覆盖已有输出和包含符号链接的应用源。
11. 编译后的 ota-http 对回环 HTTPS 服务进行真实 mTLS 下载；缺少客户端证书和服务端主机名不匹配时拒绝。
12. 编译后的 ota-mqtt-sub 连接临时回环 Mosquitto TLS/mTLS Broker，接收 QoS 1 Campaign 并在消息后保持订阅连接。

MQTT/TLS 测试仅在已安装 Mosquitto 与 mosquitto_pub 的主机运行；本次环境两者存在，实际执行通过。其他主机缺少这两个测试依赖时会显示跳过，不能将跳过当成网络测试通过。

## 结果文件与复现

服务器日志与构建输出仅在本地目录 `.build/`（已 gitignore，不进仓库）：`native-build.log`、`gk1221-build.log`、`tests.log`，以及 `native/stage/usr/bin`、`gk1221/stage/usr/bin`。交付的 ARM 工具归档仅包含原生工具，设备运行时仍由 rootfs 覆盖层组装。

```sh
make native -j4
make check
make native -j4 BUILD_DIR="$PWD/.build/gk1221" \
  CROSS_COMPILE=/path/to/toolchain/bin/arm-gcc12.2.0-linux-
```

## 尚未执行

未操作目标设备、修改线上 PKI/服务或发布真实升级；未进行真实 NAND/YAFFS 断电注入、现场 MQTT/HTTPS Campaign 到设备的整链路升级、长期运行及实际 BusyBox applet 兼容验收。原生工具已交叉构建，但未在 GK1221 板端执行。生产化验收须在这些步骤后完成。
