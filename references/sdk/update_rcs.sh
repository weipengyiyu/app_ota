#!/bin/sh
# Generate the deterministic production startup sequence.
set -eu

[ "$#" -eq 1 ] || { echo "Usage: $0 <rcS-path>" >&2; exit 2; }
RCS_FILE=$1

cat > "$RCS_FILE" <<'EOF'
#!/bin/sh


sleep 4
echo "----------------set eth0 111"
ifconfig eth0 192.168.1.111 up
sleep 1
route add default gw 192.168.1.12

sleep 1
telnetd &

/bin/mount -a

for initscript in /etc/init.d/S[0-9][0-9]*; do
    if [ -x "$initscript" ]; then
        echo "[RCS]: $initscript"
        "$initscript"
    fi
done

mkdir -p /mnt/local /mnt/sd /run/ota
grep -q ' /mnt/local ' /proc/mounts || mount /dev/mtdblock5 /mnt/local
grep -q ' /run/ota ' /proc/mounts || mount -t tmpfs -o size=2m,mode=0755 tmpfs /run/ota

# Keep the hardware watchdog fed during slow APP initialization and normal runtime.
# APP pipeline failures do not directly trigger a system reboot.
if [ -c /dev/watchdog ] && command -v watchdog >/dev/null 2>&1; then
    watchdog -F -T 60 -t 5 /dev/watchdog >/dev/console 2>&1 &
    echo $! > /run/ota/watchdog.pid
fi

# Network identity is provisioned per device. Production images do not contain
# a shared static IP. The development board may use /etc/ota/device-network.conf.
if [ -r /etc/ota/device-network.conf ]; then
    . /etc/ota/device-network.conf
    network_device_ready=0
    for wait_round in 1 2 3 4 5 6 7 8 9 10; do
        if ifconfig eth0 >/dev/null 2>&1; then
            network_device_ready=1
            break
        fi
        sleep 1
    done
    case "${NETWORK_MODE:-}" in
        static)
            [ "$network_device_ready" = 1 ] && ifconfig eth0 "$DEVICE_IP" up
            [ -z "${GATEWAY:-}" ] || route add default gw "$GATEWAY" 2>/dev/null || true
            ;;
        dhcp) [ "$network_device_ready" = 1 ] && udhcpc -i eth0 -q -n -t 5 || true ;;
    esac
fi

# Development access is opt-in and is never included in a production image.
if [ -e /etc/ota/dev_mode ]; then
    telnetd &
fi

echo "---------------- mount SD card to /mnt/sd"
# 自动探测 SD 卡分区并以 umask=0000 (777) 挂载到 /mnt/sd
# 注意: SD 卡仅用于录像, 挂载失败不能中断开机流程(demo_ai 等仍需启动), 故此处不 exit
MOUNT_POINT=/mnt/sd
mkdir -p "$MOUNT_POINT"
chmod 777 "$MOUNT_POINT"

SD_DEV=""

# 1. 优先从 /proc/mounts 中查找已经挂载到目标目录的设备
SD_DEV=$(awk -v mp="$MOUNT_POINT" '$2 == mp {print $1; exit}' /proc/mounts)

# 2. 如果未挂载，则按优先级自动探测物理设备
if [ -z "$SD_DEV" ]; then
    # 优先寻找带分区的设备 (如 mmcblk1p1)
    for d in /dev/mmcblk*p*; do
        [ -b "$d" ] || continue
        SD_DEV="$d"
        break
    done
    
    # 如果没找到分区，再退而寻找整盘设备 (如 mmcblk1)，并排除 mmz_userdev
    if [ -z "$SD_DEV" ]; then
        for d in /dev/mmcblk[0-9]*; do
            [ -b "$d" ] || continue
            # 严格过滤掉 mmz_userdev 以及其他非 mmcblk 开头的设备
            case "$d" in
                /dev/mmz_userdev) continue ;;
            esac
            SD_DEV="$d"
            break
        done
    fi
fi

# 3. 执行挂载逻辑
if [ -z "$SD_DEV" ]; then
    echo "警告: 未找到 SD 卡设备, 跳过挂载"
else
    echo "找到 SD 卡设备: $SD_DEV"
    
    # 已挂载则先卸载, 避免重复挂载
    if grep -q " $MOUNT_POINT " /proc/mounts; then
        umount "$MOUNT_POINT" 2>/dev/null
    fi
    
    # 尝试挂载 (vfat 格式, 开放读写权限)
    if mount -t vfat -o rw,umask=0000 "$SD_DEV" "$MOUNT_POINT"; then
        echo "挂载成功: $SD_DEV -> $MOUNT_POINT (umask=0000)"
        mount | grep "$MOUNT_POINT"
    else
        echo "警告: 挂载失败 $SD_DEV, 跳过(不影响开机)"
    fi
fi

sleep 1

if [ -x /usr/sbin/ota-app-supervisor ]; then
    /usr/sbin/ota-app-supervisor
else
    echo "rcS: ota-app-supervisor missing" >/dev/console
fi

# ota-agentd is transport/orchestration only. It may download and stage an APP,
# but activation remains controlled by ota-activation-gate.
if [ -x /usr/sbin/ota-agentd ]; then
    /usr/sbin/ota-agentd >/dev/console 2>&1 &
fi

exit 0
EOF

chmod 0755 "$RCS_FILE"
echo "Generated production A/B startup: $RCS_FILE"
