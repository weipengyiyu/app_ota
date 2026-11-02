#!/bin/sh
# Start the GK1221 application from the slot selected by ota-bootctl.
# Storage mounting and OTA decisions intentionally do not belong here.
set -eu

APP_ROOT="${1:-${APP_ROOT:-/mnt/local}}"

case "$APP_ROOT" in
    /mnt/local/slots/app_A/root|/mnt/local/slots/app_B/root)
        ;;
    /mnt/local)
        # Temporary field migration fallback. Production rcS selects A/B.
        [ "${OTA_ALLOW_LEGACY_APP:-0}" = "1" ] || {
            echo "start_app: legacy /mnt/local layout is disabled" >&2
            exit 1
        }
        ;;
    *)
        echo "start_app: rejected APP_ROOT: $APP_ROOT" >&2
        exit 1
        ;;
esac

[ -x "$APP_ROOT/demo_ai" ] || {
    echo "start_app: missing executable $APP_ROOT/demo_ai" >&2
    exit 1
}

mkdir -p /run/ota
APP_LOG_DIR="${APP_LOG_DIR:-/mnt/sd}"
APP_CONSOLE="${APP_CONSOLE:-/dev/console}"
mkdir -p "$APP_LOG_DIR"
[ -d "$APP_LOG_DIR" ] && [ -w "$APP_LOG_DIR" ] || {
    echo "start_app: log directory is not writable: $APP_LOG_DIR" >&2
    exit 1
}
[ -c "$APP_CONSOLE" ] && [ -w "$APP_CONSOLE" ] || {
    echo "start_app: serial console is unavailable: $APP_CONSOLE" >&2
    exit 1
}

# ota-app-supervisor redirects launcher output to an SD-card log. Override that
# redirection here so the complete hardware/application initialization sequence
# remains visible on the serial console. demo_ai's structured health log is
# still written independently under APP_LOG_DIR (/mnt/sd by default).
exec >"$APP_CONSOLE" 2>&1

HW_INIT_STATUS_FILE=/run/ota/hw_init.status
HW_INIT_ERRORS=""
rm -f "$HW_INIT_STATUS_FILE"

record_hw_init_error()
{
    if [ -n "$HW_INIT_ERRORS" ]; then
        HW_INIT_ERRORS="${HW_INIT_ERRORS},$1"
    else
        HW_INIT_ERRORS=$1
    fi
}

publish_hw_init_status()
{
    status_tmp="${HW_INIT_STATUS_FILE}.$$"
    if [ -n "$HW_INIT_ERRORS" ]; then
        printf '%s\n' \
            'STATUS=DEGRADED' \
            "ERRORS=$HW_INIT_ERRORS" \
            > "$status_tmp"
    else
        printf '%s\n' \
            'STATUS=OK' \
            'ERRORS=NONE' \
            > "$status_tmp"
    fi
    mv "$status_tmp" "$HW_INIT_STATUS_FILE"
}

echo "------rootfs----------load ko"
cd /lib/ko
./load xm1221v105 -i
sleep 3

echo "------$APP_ROOT----------load serdes"
if ! sh "$APP_ROOT/X3C_imx728_96717_96712_30fps_8m.sh"; then
    record_hw_init_error SERDES_INIT_FAILED
    echo "start_app: WARNING: SerDes initialization failed; continuing in degraded mode" >&2
fi

echo "------$APP_ROOT----------load ark7118"
if ! sh "$APP_ROOT/ark7118.sh"; then
    record_hw_init_error ARK7118_INIT_FAILED
    echo "start_app: WARNING: ark7118 initialization failed; continuing in degraded mode" >&2
fi

publish_hw_init_status

echo "------$APP_ROOT----------load demo_ai"
sleep 3
cd "$APP_ROOT"
# demo_ai must be started only here: after KO loading and both SerDes/ark7118
# initialization attempts have completed. It runs in the background and
# inherits /dev/console; degraded hardware status remains observable above.
./demo_ai 32 1 &
DEMO_PID=$!
echo "$DEMO_PID" > /run/ota/demo_ai.pid
rm -f /run/ota/demo_ai.stdout.log.path

sleep 3
kill -0 "$DEMO_PID" 2>/dev/null || {
    echo "start_app: demo_ai exited during startup" >&2
    exit 1
}

echo "----------------load isp.bin"
cd /lib/jm_config
./pq_bin 1 1 0 0 0 0 0 isp.bin || {
    kill "$DEMO_PID" 2>/dev/null || true
    exit 1
}

sleep 1
echo "----------------load StartControl"
cd /lib/pq_board_2/arm-gcc12.2.0-linux
./StartControl.sh || {
    kill "$DEMO_PID" 2>/dev/null || true
    exit 1
}

HEALTH_LOG=$(ls -1t "$APP_LOG_DIR"/demo_ai_*_"$DEMO_PID".log 2>/dev/null | head -1)
[ -z "$HEALTH_LOG" ] || echo "$HEALTH_LOG" > /run/ota/demo_ai.health.log.path

echo "start_app: demo_ai pid=$DEMO_PID root=$APP_ROOT stdout=/dev/console health_log=${HEALTH_LOG:-PENDING}"
exit 0
