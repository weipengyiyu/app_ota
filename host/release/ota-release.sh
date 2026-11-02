#!/bin/sh
# One-shot release: deploy -> signed bundle -> HTTPS + MQTT (production boards).
set -eu

SCRIPT=$(readlink -f "$0" 2>/dev/null || echo "$0")
SCRIPT_DIR=$(CDPATH= cd "$(dirname "$SCRIPT")" && pwd)
# shellcheck source=ota-release-paths.sh
. "$SCRIPT_DIR/../../lib/host-env.sh"
# shellcheck source=devices.conf
[ ! -r "$OTA_HOST_CONFIG" ] || . "$OTA_HOST_CONFIG"

BUILD=$SCRIPT_DIR/build_app_bundle.sh
PUBLISH=${OTA_PUBLISH_COMMAND:-$SCRIPT_DIR/publish_network_ota.sh}
INDEX=$OTA_WORK_ROOT/release-index.env

GROUP=prod
DEVICES=
VERSION=
CAMPAIGN=

log() { echo "[ota-release] $*"; }
die() { echo "[ota-release] ERROR: $*" >&2; exit 1; }

usage() {
    echo "Usage: $0 VERSION [--group prod|lab] [--device device-112 ...] [--campaign ID]" >&2
    echo "  source is always: $OTA_APP_SOURCE" >&2
    exit 2
}

next_counter() {
    if [ -f "$INDEX" ]; then
        # shellcheck disable=SC1090
        . "$INDEX"
    fi
    last=${LAST_RELEASE_COUNTER:-0}
    if [ "$last" -gt 0 ] 2>/dev/null; then
        echo $((last + 1))
        return
    fi
    echo 1
}

save_counter() {
    mkdir -p "$OTA_WORK_ROOT"
    umask 077
    printf 'LAST_RELEASE_VERSION=%s\nLAST_RELEASE_COUNTER=%s\n' "$VERSION" "$1" > "$INDEX.tmp"
    mv -f "$INDEX.tmp" "$INDEX"
}

while [ "$#" -gt 0 ]; do
    case "$1" in
        --group)
            [ "$#" -ge 2 ] || usage
            GROUP=$2
            shift 2
            ;;
        --device)
            shift
            while [ "$#" -gt 0 ]; do
                case "$1" in
                    --*) break ;;
                    *)
                        DEVICES="$DEVICES $1"
                        shift
                        ;;
                esac
            done
            ;;
        --campaign)
            [ "$#" -ge 2 ] || usage
            CAMPAIGN=$2
            shift 2
            ;;
        -h|--help) usage ;;
        -*)
            die "unknown option: $1"
            ;;
        *)
            [ -z "$VERSION" ] || die "unexpected argument: $1"
            VERSION=$1
            shift
            ;;
    esac
done

[ -n "$VERSION" ] || usage
[ -n "${OTA_APP_SOURCE:-}" ] || die "set OTA_APP_SOURCE"
[ -n "${SIGN_KEY:-}" ] || die "set SIGN_KEY"
printf '%s\n' "$VERSION" | grep -Eq '^V[0-9]+\.[0-9]+\.[0-9]+$' || die "invalid VERSION: $VERSION"
[ -d "$OTA_APP_SOURCE" ] || die "deploy missing: $OTA_APP_SOURCE"
[ -f "$SIGN_KEY" ] || die "missing SIGN_KEY: $SIGN_KEY"

if [ -z "$DEVICES" ]; then
    case "$GROUP" in
        prod) DEVICES=$PROD_DEVICES ;;
        lab) DEVICES=$LAB_DEVICE_ID ;;
        *) die "unknown group: $GROUP" ;;
    esac
fi

mkdir -p "$OTA_WORK_ROOT"
mkdir "$OTA_WORK_ROOT/release.lock" 2>/dev/null || die "release already running (or stale release.lock)"
trap 'rmdir "$OTA_WORK_ROOT/release.lock"' EXIT
trap 'exit 1' HUP INT TERM
COUNTER=$(next_counter)
[ -n "$CAMPAIGN" ] || CAMPAIGN="campaign-$(date -u +%Y%m%dT%H%M%SZ)-$VERSION"

[ -n "$DEVICES" ] || die "no target devices configured"
# Reserve the global counter before publication; retries never reuse it.
save_counter "$COUNTER"
log "building from deploy version=$VERSION counter=$COUNTER"
SIGN_KEY=$SIGN_KEY sh "$BUILD" "$OTA_APP_SOURCE" "$VERSION" "$COUNTER" "$CAMPAIGN" || die "build failed"
for dev in $DEVICES; do
    log "publishing to $dev"
    "$PUBLISH" --device-id "$dev" "$VERSION" "$COUNTER" "$CAMPAIGN" || die "publish failed for $dev"
done

save_counter "$COUNTER"
log "SUCCESS: $VERSION counter=$COUNTER campaign=$CAMPAIGN devices:$DEVICES"
