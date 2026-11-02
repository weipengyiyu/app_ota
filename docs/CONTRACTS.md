# 稳定接口与格式

## Bundle v2

Bundle 是预展开目录，包含 `manifest.env`、`manifest.sha256`、`signed-data.bin`、`manifest.sig` 和 `payload/`。`signed-data.bin` 必须逐字节等于 manifest.env 与 manifest.sha256 的顺序拼接；ECDSA P-256 对其 SHA-256 签名，签名文件为 DER。消费端用独立受信公钥，不能从 Bundle 自带公钥建立信任。

manifest.env 必需字段：FORMAT_VERSION、PRODUCT_ID、HARDWARE_REVISION、ROOTFS_ABI、ARTIFACT_ID、APP_VERSION、RELEASE_COUNTER、UNPACKED_KIB、FILE_COUNT、TRANSACTION_ID、CAMPAIGN_ID；BUILD_COMMIT 作为构建追踪字段由打包工具输出。FORMAT_VERSION 为2，元数据字段不能重复。文件列表用 sha256sum 标准 `hash  ./relative/path`；逐文件集合必须与 payload 完全相等。目录、普通文件以外的类型不允许进入包。

Bundle 路径从基线的 VERSION 改为 VERSION-rCOUNTER，内容格式保持不变。使用旧 Bundle 时可通过安装器直接消费原目录；旧代理对新的 BUNDLE_URL 目录名也没有固定 VERSION 约束。

## Campaign v1

设备订阅 `ota/PRODUCT_ID/DEVICE_ID/campaign`，QoS 1 retained，内容为逐行 key=value：

```ini
FORMAT_VERSION=1
PRODUCT_ID=GK1221-ADAS
CAMPAIGN_ID=campaign-001
APP_VERSION=V1.0.0
RELEASE_COUNTER=101
BUNDLE_URL=https://ota-download.example.com/gk1221/V1.0.0-r101
```

每字段只出现一次。Campaign 是下载提示；代理仍须独立验证 Bundle 签名、身份、计数和文件内容。MQTT 通知本身没有额外固件签名，网络认证依赖设备 TLS 身份；通知不能授予切槽权限。

## State v1 与命令

`ota-bootctl status` 返回 key=value 状态。读取期间遇到事务锁占用返回非零，调用方可退避重试。持久记录固定17字段，双记录校验和与 GENERATION 用于选取有效新状态。状态字段示例由出厂生成器产生，不通过 source 执行外部状态内容。

- `init`：已有状态有效则保留；无有效状态时从有效本地槽初始化。
- `select`：选择当前启动槽，处理试运行计数与必要回退，输出选中根目录。
- `stage A|B ARTIFACT VERSION COUNTER TX CAMPAIGN`：记录已安装候选，不切槽。
- `activate-internal`：消费一次性事务授权，进入 TRIAL_ARMED；正常外部调用应使用 activation-gate authorize。
- `mark-good`：仅 TRIAL 中确认当前候选；更新已确认槽和已确认发行计数。
- `mark-bad REASON`：回到确认槽，记录失败原因。
- `repair-state`：刷新冗余记录。

## 设备安全状态

设备控制系统将以下内容写入临时文件并原子 rename 到 `/run/ota/safe_to_update`。文件更新间隔须小于策略规定的30秒。

```ini
SAFE=1
STATE=IDLE
SPEED_MPS=0
ACTUATORS_DISABLED=1
POWER_OK=1
```

字段必须唯一；缺失、重复、陈旧、未来时间戳或不满足条件均拒绝激活。更新禁止标志为 `/run/ota/update_inhibit`。此文件接口的可信性来自控制系统写入权限，不能让网络代理或应用下载内容覆盖它。

## GK1221 启动与健康适配

应用启动入口接收选中 APP 根目录为首个参数，完成硬件初始化后写 `/run/ota/demo_ai.pid`。健康日志文件为 `/mnt/sd/demo_ai_<time>_<pid>.log`，ALIVE 行至少包含 `ALIVE pid=PID svp_ok=N vpss_fail=N svp_fail=N`。健康判断匹配当前 PID，要求日志新鲜、svp_ok 持续增长、vpss_fail/svp_fail 不增长。

`ota-healthd` 默认每10秒采样，连续6个有效增长窗口后确认，默认180秒超时。首次样本只建立基线。没有当前日志、缺字段、陈旧日志、进程退出都不能形成成功窗口。激活前健康检查由 `health-functions.sh` 实现，替换产品时通过 OTA_HEALTH_ADAPTER 指定新的受信适配器。

## 平台运行条件

`/mnt/local` 必须是已挂载的可写持久分区；`/run` 必须是每次启动清空的 tmpfs。POSIX shell、awk、sed、grep、find、sort、xargs、sha256sum、cmp、cp、mv、du、df、stat、sync、readlink、进程管理与平台 reboot 是现有依赖。实际 BusyBox applet 选项兼容性须在目标 rootfs 上验证。
