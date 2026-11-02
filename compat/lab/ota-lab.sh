#!/bin/sh
# Push deploy/ or local jmapp to lab board.
# Release host (216): uses devices.conf defaults.
# Dev host (250/205/...): copy devices.local.conf.example -> devices.local.conf.
set -eu

SCRIPT=$(readlink -f "$0" 2>/dev/null || echo "$0")
SCRIPT_DIR=$(CDPATH= cd "$(dirname "$SCRIPT")" && pwd)
# shellcheck source=ota-release-paths.sh
. "$SCRIPT_DIR/../../lib/host-env.sh"
# shellcheck source=devices.conf
[ ! -r "$OTA_HOST_CONFIG" ] || . "$OTA_HOST_CONFIG"
if [ -f "$SCRIPT_DIR/devices.local.conf" ]; then
    # shellcheck source=devices.local.conf
    . "$SCRIPT_DIR/devices.local.conf"
fi

TELNET_PY=$SCRIPT_DIR/ota-lab-telnet.py
BUILD=$PROJECT_ROOT/host/release/build_app_bundle.sh

BOARD_IP=$LAB_BOARD_IP
DEVICE_ID=$LAB_DEVICE_ID
SRC=${OTA_APP_SOURCE:-}
USE_LOCAL=0
SOURCE_DIR=
ACTIVATE=0
REBOOT=0
FORCE=0
VERSION=
SKIP_STAGE=0

log() { echo "[ota-lab] $*"; }
die() { echo "[ota-lab] ERROR: $*" >&2; exit 1; }

lookup_device() {
    ip=$1
    for pair in $LAB_BOARD_LIST; do
        bip=${pair%%:*}
        bid=${pair#*:}
        if [ "$bip" = "$ip" ]; then
            echo "$bid"
            return 0
        fi
    done
    echo "$LAB_DEVICE_ID"
}

list_boards() {
    echo "Configured lab boards (use: ota-lab --board IP ...):"
    for pair in $LAB_BOARD_LIST; do
        echo "  ${pair%%:*}  ->  ${pair#*:}"
    done
}

usage() {
    echo "Usage: $0 [--local | --source DIR] [--board IP] [--version Vx.y.z] [--activate] [--reboot] [--force]" >&2
    echo "       $0 --list-boards" >&2
    echo "  default board  : $LAB_BOARD_IP ($LAB_DEVICE_ID)" >&2
    echo "  default source : $OTA_APP_SOURCE" >&2
    echo "  --local        : use ./board/jmapp under current gk1221 SDK tree" >&2
    echo "  --force        : rebuild even when board is already STAGED" >&2
    echo "  STAGED + --activate : activate pending slot only (no rebuild)" >&2
    exit 2
}

find_gk1221_root() {
    dir=$PWD
    while [ "$dir" != "/" ]; do
        if [ -d "$dir/board/jmapp" ] && { [ -f "$dir/board/jmapp/demo_ai" ] || [ -f "$dir/board/jmapp/config_1221.ini" ]; }; then
            echo "$dir/board/jmapp"
            return 0
        fi
        dir=$(dirname "$dir")
    done
    return 1
}

while [ "$#" -gt 0 ]; do
    case "$1" in
        --local) USE_LOCAL=1; shift ;;
        --source)
            [ "$#" -ge 2 ] || usage
            SOURCE_DIR=$2
            USE_LOCAL=1
            shift 2
            ;;
        --board)
            [ "$#" -ge 2 ] || usage
            BOARD_IP=$2
            shift 2
            ;;
        --version)
            [ "$#" -ge 2 ] || usage
            VERSION=$2
            shift 2
            ;;
        --activate) ACTIVATE=1; shift ;;
        --reboot) ACTIVATE=1; REBOOT=1; shift ;;
        --force) FORCE=1; shift ;;
        --list-boards) list_boards; exit 0 ;;
        -h|--help) usage ;;
        *) die "unknown option: $1" ;;
    esac
done

DEVICE_ID=$(lookup_device "$BOARD_IP")
[ -x "$BUILD" ] || die "missing build_app_bundle.sh"
[ -f "$SIGN_KEY" ] || die "missing SIGN_KEY: $SIGN_KEY"
[ -x "$TELNET_PY" ] || die "missing ota-lab-telnet.py"

if [ "$USE_LOCAL" = 1 ]; then
    if [ -n "$SOURCE_DIR" ]; then
        SRC=$SOURCE_DIR
    else
        SRC=$(find_gk1221_root) || die "not inside a gk1221 SDK tree; use --source DIR"
    fi
    log "debug source (jmapp): $SRC"
else
    log "release source (deploy): $SRC"
fi

ping -c 1 -W 2 "$BOARD_IP" >/dev/null 2>&1 || die "board unreachable: $BOARD_IP"
log "target board $BOARD_IP (device $DEVICE_ID)"

log "reading OTA state from $BOARD_IP"
STATUS=$("$TELNET_PY" "$BOARD_IP" "/usr/sbin/ota-bootctl status" </dev/null) || die "ota-bootctl status failed"
ACTIVE_COUNTER=$(printf '%s\n' "$STATUS" | sed -n 's/^active_counter=//p' | sed -n '1p')
PENDING_COUNTER=$(printf '%s\n' "$STATUS" | sed -n 's/^pending_counter=//p' | sed -n '1p')
ACTIVE_VERSION=$(printf '%s\n' "$STATUS" | sed -n 's/^active_version=//p' | sed -n '1p')
PENDING_VERSION=$(printf '%s\n' "$STATUS" | sed -n 's/^pending_version=//p' | sed -n '1p')
PHASE=$(printf '%s\n' "$STATUS" | sed -n 's/^phase=//p' | sed -n '1p')
case "$ACTIVE_COUNTER" in ''|*[!0-9]*) die "invalid active_counter on board" ;; esac

case "$PHASE" in
    STAGED)
        if [ "$ACTIVATE" = 1 ] && [ "$FORCE" != 1 ]; then
            log "board already STAGED ($PENDING_VERSION counter=$PENDING_COUNTER); activating pending slot"
            SKIP_STAGE=1
        elif [ "$FORCE" = 1 ]; then
            log "board STAGED; --force will stage a new build"
            COUNTER=$((${PENDING_COUNTER:-$ACTIVE_COUNTER} + 1))
        else
            die "board phase=STAGED ($PENDING_VERSION). Use --activate --reboot to switch, or --force to rebuild"
        fi
        ;;
    ACTIVE|ROLLED_BACK)
        COUNTER=$((ACTIVE_COUNTER + 1))
        ;;
    TRIAL|TRIAL_ARMED)
        die "board phase=$PHASE (trial boot in progress). Wait until phase=ACTIVE, or check /mnt/local/ota_audit/events.log on the board"
        ;;
    *) die "unsupported board phase: $PHASE" ;;
esac

if [ "$SKIP_STAGE" = 0 ]; then
    [ -d "$SRC" ] || die "source missing: $SRC"
    [ -f "$SRC/demo_ai" ] || die "demo_ai missing under $SRC"

    if [ -z "${COUNTER:-}" ]; then
        COUNTER=$((ACTIVE_COUNTER + 1))
    fi
    if [ -z "$VERSION" ]; then
        INDEX=$OTA_WORK_ROOT/release-index.env
        if [ "$USE_LOCAL" != 1 ] && [ -f "$INDEX" ]; then
            # shellcheck disable=SC1090
            . "$INDEX"
            case "${LAST_RELEASE_VERSION:-}" in
                V*.*.*)
                    VERSION=$LAST_RELEASE_VERSION
                    log "version from last ota-release: $VERSION (override with --version)"
                    ;;
            esac
        fi
        if [ -z "$VERSION" ]; then
            case "$ACTIVE_VERSION" in
                V*.*.*)
                    VERSION=$ACTIVE_VERSION
                    log "version from board active slot: $VERSION (same line; counter will increment)"
                    ;;
                *) VERSION=V0.6.2; log "using default VERSION=$VERSION" ;;
            esac
        fi
    fi
    printf '%s\n' "$VERSION" | grep -Eq '^V[0-9]+\.[0-9]+\.[0-9]+$' || die "invalid VERSION: $VERSION"

    BUNDLE=$OTA_WORK_ROOT/.lab-push/bundle
    CAMPAIGN=lab-$(date -u +%Y%m%dT%H%M%SZ)
    rm -rf "$BUNDLE"
    mkdir -p "$OTA_WORK_ROOT/.lab-push"

    log "building signed bundle version=$VERSION counter=$COUNTER"
    SIGN_KEY=$SIGN_KEY sh "$BUILD" "$SRC" "$VERSION" "$COUNTER" "$BUNDLE" "$CAMPAIGN" || die "bundle build failed"

    case "$BUNDLE" in
        "$NFS_EXPORT_ROOT"/*) ;;
        *) die "bundle $BUNDLE must be under NFS_EXPORT_ROOT=$NFS_EXPORT_ROOT (check devices.local.conf)" ;;
    esac
    NFS_BUNDLE="$NFS_EXPORT_ROOT${BUNDLE#$NFS_EXPORT_ROOT}"
    MOUNT=${OTA_BOARD_MOUNT:-/mnt/lab216}

    log "staging on board via NFS from $NFS_SERVER (no Campaign/MQTT)"
    "$TELNET_PY" "$BOARD_IP" \
        "killall ota-agentd 2>/dev/null || true; mkdir -p $MOUNT /run/ota /mnt/sd/ota_download; umount $MOUNT 2>/dev/null || true; mount -t nfs -o nolock,tcp $NFS_SERVER:$NFS_BUNDLE $MOUNT && TMPDIR=/run/ota /usr/sbin/ota-installerd install $MOUNT && umount $MOUNT && /usr/sbin/ota-bootctl status" \
        || die "install on board failed"
fi

if [ "$ACTIVATE" = 1 ]; then
    log "authorizing activation (lab safe state)"
    "$TELNET_PY" "$BOARD_IP" \
        "mkdir -p /run/ota; slot=\$(/usr/sbin/ota-bootctl status | sed -n 's/^pending_slot=//p' | head -1); meta=/mnt/local/slots/app_\${slot}/slot.meta; tx=\$(awk -F= '/^TRANSACTION_ID=/{print \$2; exit}' \$meta 2>/dev/null); [ -n \"\$tx\" ] || tx=lab-manual; printf 'SAFE=1\nSTATE=IDLE\nSPEED_MPS=0\nACTUATORS_DISABLED=1\nPOWER_OK=1\nTRANSACTION_ID=%s\n' \"\$tx\" > /run/ota/safe_to_update; sync; /usr/sbin/ota-activation-gate authorize && /usr/sbin/ota-bootctl status" \
        || die "activation failed"
fi

if [ "$REBOOT" = 1 ]; then
    log "rebooting board $BOARD_IP"
    "$TELNET_PY" "$BOARD_IP" "sync; reboot" || true
    log "SUCCESS: reboot sent to $BOARD_IP ($DEVICE_ID)"
elif [ "$SKIP_STAGE" = 1 ] && [ "$ACTIVATE" = 1 ]; then
    log "SUCCESS: activated on $BOARD_IP ($DEVICE_ID). Reboot manually or use --reboot."
else
    log "SUCCESS: staged on $BOARD_IP ($DEVICE_ID). Use --activate --reboot to switch."
fi
