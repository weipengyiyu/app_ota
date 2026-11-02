# APP-OTA 独立实现

从 `logic@192.168.1.216:/home/logic/1221/0901/gk1221` 提取的应用层 OTA 项目。基线提交 `aa6d7886de0ddd56f744d3fab1075e0fbe0c564f`，文件来源和 SHA-256 记录在 `provenance.json`。

项目独立构建、打包和生成 rootfs 覆盖层，不需要编译整套 SDK。支持签名 Bundle、MQTT 通知、HTTPS/mTLS 下载、APP A/B 逻辑槽、独立激活门控、试运行确认、自动回退和恢复。升级范围是应用、模型和应用资源；Kernel、U-Boot、rootfs 镜像及 ISP 芯片固件升级不在本协议内。

架构与代码职责见 [ARCHITECTURE.md](docs/ARCHITECTURE.md)，迁移与差异见 [MIGRATION.md](docs/MIGRATION.md)，接口见 [CONTRACTS.md](docs/CONTRACTS.md)，验证结果见 [VALIDATION.md](docs/VALIDATION.md)。

## 目录

```text
runtime/           状态核心、安装、网络代理、激活、启动监管、恢复
native/            ECDSA P-256 验签、Paho MQTT/TLS 适配源码
host/release/      签名打包、出厂槽准备、发布、发布计数管理
profiles/gk1221/   产品 ID、硬件版本、ABI、APP 文件清单与入口
adapters/gk1221/   GK1221 健康日志适配、应用启动脚本及健康接口参考源码
config/            设备代理模板、激活策略、发布主机配置示例
integration/       rootfs 覆盖层和开机集成入口
server/            Nginx/Mosquitto/mTLS/ACL 配置模板
compat/lab/        从原项目保留并适配的实验室 NFS/Telnet 工作流
mk/ third_party/     离线原生构建规则与固定版本源码归档
tools/ tests/      覆盖层生成器和隔离集成测试
references/        原 SDK 集成片段、原设备清单和历史公钥，仅供比对
```

## 独立构建

构建环境为 Linux，需 POSIX shell、GNU make、C/C++ 工具链、CMake、Python 3、patch、tar/xz/bzip2、OpenSSL 和常用 coreutils。设备运行时不需要 Python、CMake 或 OpenSSL CLI。

```sh
make native -j4           # 构建当前主机架构工具
make check               # 主机验签与隔离升级集成测试
```

使用 GK1221 对应 glibc/浮点 ABI 的外部编译器，单独使用构建目录：

```sh
make native -j4 \
  CROSS_COMPILE=/path/to/toolchain/bin/arm-gcc12.2.0-linux- \
  BUILD_DIR="$PWD/.build/gk1221"
```

工具链路径是输入，不属于 SDK 源码依赖。切换编译器时必须使用不同 BUILD_DIR，避免复用另一架构的中间文件。`ota-http` 是精简 curl；`ota_verify_sig` 使用 mbedTLS 的真实 ECDSA P-256/SHA-256 验签。

## 构建签名升级包

APP_SOURCE 由业务项目提供，需包含产品 profile 中的全部文件。应用需使用 `adapters/gk1221/start_imx728_adas.sh` 约定的启动与健康日志接口。健康参考源码只作为原业务接入参考，OTA 不编译 NPU/ADAS 主程序。

```sh
make bundle APP_SOURCE=/path/to/deploy VERSION=V1.0.0 COUNTER=101 \
  SIGN_KEY=/secure/app-ota-signing.pem
```

默认输出 `.work/bundles/V1.0.0-r101`。签名私钥必须由调用者提供；输出目录已存在时拒绝覆盖。COUNTER 是设备发行线内全局单调递增的整数，跨版本也必须递增。文件名只使用字母、数字、点、下划线、横杠和目录分隔符，不支持空格、符号链接和特殊文件。

## 生成出厂 APP 与 rootfs 覆盖层

```sh
make factory APP_SOURCE=/path/to/deploy FACTORY_STAGE=/path/to/out/factory
make install BUILD_DIR="$PWD/.build/gk1221" \
  DESTDIR=/path/to/out/rootfs-overlay \
  TRUSTED_RELEASE_PUBKEY=/secure/app-ota-public.pem
```

出厂内容包含已确认的 A 槽、空 B 槽与双份状态记录。覆盖层包含控制程序、原生工具、产品配置和默认关闭的网络代理。安装器强制要求独立指定信任公钥，不自动使用 `references` 下的历史公钥。DESTDIR 必须是专用暂存目录；没有执行设备安装。

在 SDK rootfs 打包阶段合入覆盖层，或 include `integration/sdk/app-ota.mk`。在板级初始化完成持久存储挂载、`/run` tmpfs、SD 日志、网络、时钟和 watchdog 后调用 `/usr/sbin/ota-start`。OTA 不接管 IP 地址或 SD 设备探测，不替换整个 rcS。

## 设备命令

```sh
ota-bootctl status
ota-installerd install /path/to/verified-bundle
ota-activation-gate check
ota-activation-gate authorize
# 设备控制系统决定合适时间执行重启。
```

代理只下载和 STAGED，不生成设备安全状态、不授权激活、不重启。设备控制系统原子更新 `/run/ota/safe_to_update`；激活后试运行，健康适配器确认成功才更新确认槽。健康不达标或试运行次数超限会回退。默认策略强制当前应用健康检查，禁止工厂健康旁路。

## 网络发布

将 `config/host.conf.example` 复制为 `config/host.local.conf` 并填入环境参数，或通过 `OTA_HOST_CONFIG` 指向外部受信配置。提供独立设备证书、Broker/HTTPS 证书、签名密钥和信任公钥。按 `server/README.md` 集成现有服务。

```sh
host/release/ota-release.sh V1.0.0 --device device-001
```

该命令明确执行真实发布。没有默认生产设备列表；必须指定设备或配置 PROD_DEVICES。它预留全局计数后构建、发布、发送通知。失败后计数可以留空洞，不能回退复用。初次接管已有设备时，需把 `.work/release-index.env` 的 LAST_RELEASE_COUNTER 设置到所有目标设备已使用计数的最大值以上，再开始发布。多发布主机必须共享唯一计数分配存储；独立的本地工作目录不能提供跨主机全局计数。

实验室工作流见 `compat/lab/README.md`。测试和构建未触达设备、Broker 或线上 HTTP 服务。设备端断电恢复和完整网络端到端验收仍需在实际目标板上执行。
