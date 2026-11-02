#!/bin/sh
# One-time (or refresh) OTA control-plane install on lab board via NFS. Run on 216.
# Root partition is often full (32MB); store on /mnt/local/ota + bind mounts.
set -eu

SCRIPT=$(readlink -f "$0" 2>/dev/null || echo "$0")
SCRIPT_DIR=$(CDPATH= cd "$(dirname "$SCRIPT")" && pwd)
# shellcheck source=devices.conf
. "$SCRIPT_DIR/../../lib/host-env.sh"
[ ! -r "$OTA_HOST_CONFIG" ] || . "$OTA_HOST_CONFIG"

TELNET_PY=$SCRIPT_DIR/ota-lab-telnet.py
BOARD_IP=${1:-$LAB_BOARD_IP}
OTA_SRC=${OTA_CONTROL_SOURCE:-$PROJECT_ROOT/.build/lab-control}

LOCAL=/mnt/local/ota
LIB=$LOCAL/lib
ETC=$LOCAL/etc
MOUNT=/mnt/lab216
SBIN_BINS="ota-bootctl ota-app-supervisor ota-healthd app-recovery ota-installerd ota-activation-gate ota-migrate-legacy"

log() { echo "[ota-board-setup] $*"; }
die() { echo "[ota-board-setup] ERROR: $*" >&2; exit 1; }
board() { "$TELNET_PY" "$BOARD_IP" "$1" || die "$2"; }

[ -x "$TELNET_PY" ] || die "missing ota-lab-telnet.py"
[ -f "$OTA_SRC/ota-bootctl" ] || die "run make lab-control first"
[ -n "$BOARD_IP" ] && [ -n "$NFS_SERVER" ] && [ -n "$NFS_EXPORT_ROOT" ] || die "configure lab board and NFS explicitly"
ping -c 1 -W 2 "$BOARD_IP" >/dev/null 2>&1 || die "board unreachable: $BOARD_IP"

NFS_OTA="$NFS_SERVER:$NFS_EXPORT_ROOT${OTA_SRC#$NFS_EXPORT_ROOT}"

log "installing control plane on $BOARD_IP (store=$LOCAL, bind mounts)"

board "mkdir -p $LOCAL $LIB $ETC $MOUNT /usr/lib/ota /usr/sbin /etc/ota /run/ota /mnt/sd/ota_download" "mkdir failed"
board "umount $MOUNT 2>/dev/null || true; mount -t nfs -o nolock,tcp $NFS_OTA $MOUNT" "nfs mount failed"

board "cp -f $MOUNT/lib/ota-state-lib.sh $LIB/ota-state-lib.sh && cp -f $MOUNT/lib/health-functions.sh $LIB/health-functions.sh && chmod 0644 $LIB/*.sh" "copy libraries failed"
for n in $SBIN_BINS; do
    board "cp -f $MOUNT/$n $LOCAL/$n && chmod 0755 $LOCAL/$n" "copy $n failed"
done
board "cp -f $MOUNT/etc/activation-policy.conf $ETC/activation-policy.conf && cp -f $MOUNT/etc/agent.conf $ETC/agent.conf && cp -f $MOUNT/etc/product.conf $ETC/product.conf" "copy etc templates failed"
board "printf 'GK1221-ADAS\n' > $ETC/product_id; printf 'xm1221v105-r1\n' > $ETC/rootfs_abi; : > $ETC/dev_mode" "write etc templates failed"
board "printf '%s\n' '#!/bin/sh' 'exit 0' > $LOCAL/ota-agentd-wrapper.sh && chmod 0755 $LOCAL/ota-agentd-wrapper.sh" "agent wrapper failed"
board "cp -f $MOUNT/ota-bind-mounts.sh $LOCAL/ota-bind-mounts.sh && chmod 0755 $LOCAL/ota-bind-mounts.sh" "copy bind hook failed"

for n in $SBIN_BINS; do
    board "touch /usr/sbin/$n; umount /usr/sbin/$n 2>/dev/null || true; mount --bind $LOCAL/$n /usr/sbin/$n" "bind $n failed"
done
board "touch /usr/sbin/ota-agentd; umount /usr/sbin/ota-agentd 2>/dev/null || true; mount --bind $LOCAL/ota-agentd-wrapper.sh /usr/sbin/ota-agentd" "bind ota-agentd failed"
for name in ota-state-lib.sh health-functions.sh; do
    board "touch /usr/lib/ota/$name; umount /usr/lib/ota/$name 2>/dev/null || true; mount --bind $LIB/$name /usr/lib/ota/$name" "bind library failed"
done

for f in activation-policy.conf agent.conf product_id rootfs_abi dev_mode product.conf; do
    board "touch /etc/ota/$f; umount /etc/ota/$f 2>/dev/null || true; mount --bind $ETC/$f /etc/ota/$f" "bind /etc/ota/$f failed"
done

board "killall ota-agentd 2>/dev/null || true" "kill agent failed"
board "grep -q ota-bind-mounts.sh /etc/init.d/rcS 2>/dev/null || sed -i '/^# a shared static IP/i . /mnt/local/ota/ota-bind-mounts.sh' /etc/init.d/rcS 2>/dev/null || true" "rcS hook optional"
board "umount $MOUNT 2>/dev/null || true" "umount failed"
board "wc -c $LOCAL/ota-installerd /usr/sbin/ota-installerd; test -f /etc/ota/dev_mode && echo dev_mode_on" "verify failed"
board "/usr/sbin/ota-bootctl status" "status failed"

log "SUCCESS: control plane on $BOARD_IP (dev_mode enabled, agent stopped)"
