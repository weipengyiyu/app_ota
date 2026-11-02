#!/bin/sh
# Re-apply OTA control-plane bind mounts after boot (stored on /mnt/local).
LOCAL=/mnt/local/ota
LIB=$LOCAL/lib

for n in ota-bootctl ota-app-supervisor ota-healthd app-recovery ota-installerd ota-activation-gate ota-migrate-legacy; do
    [ -f "$LOCAL/$n" ] || continue
    umount "/usr/sbin/$n" 2>/dev/null || true
    mount --bind "$LOCAL/$n" "/usr/sbin/$n" 2>/dev/null || true
done

if [ -f "$LOCAL/ota-agentd-wrapper.sh" ]; then
    umount /usr/sbin/ota-agentd 2>/dev/null || true
    mount --bind "$LOCAL/ota-agentd-wrapper.sh" /usr/sbin/ota-agentd 2>/dev/null || true
fi

for name in ota-state-lib.sh health-functions.sh; do
    [ -f "$LIB/$name" ] || continue
    touch "/usr/lib/ota/$name" 2>/dev/null || true
    umount "/usr/lib/ota/$name" 2>/dev/null || true
    mount --bind "$LIB/$name" "/usr/lib/ota/$name" 2>/dev/null || true
done

ETC=$LOCAL/etc
for f in activation-policy.conf agent.conf product_id rootfs_abi dev_mode product.conf; do
    [ -f "$ETC/$f" ] || continue
    touch "/etc/ota/$f" 2>/dev/null || true
    umount "/etc/ota/$f" 2>/dev/null || true
    mount --bind "$ETC/$f" "/etc/ota/$f" 2>/dev/null || true
done
