#!/bin/sh
# GK1221 网络 APP-OTA 发布脚本（仅运行在 192.168.1.216）
#
# 职责：
#   校验已签名 Bundle
#   -> 原子发布到 Nginx HTTPS 目录
#   -> 生成 Campaign notice
#   -> 使用发布者证书发送 MQTT QoS 1 retained 通知
#
# 本脚本不持有 OTA 签名私钥，不连接板端，不授权切槽，不重启设备。

set -eu

SCRIPT_DIR=$(CDPATH= cd "$(dirname "$0")" && pwd)
# shellcheck source=ota-release-paths.sh
. "$SCRIPT_DIR/../../lib/host-env.sh"

# DEVICE_ID 必须显式传入。历史 --sdk-root 参数仅兼容解析，不读取 SDK。
SDK_ROOT=
DEVICE_ID=

WEB_ROOT=${WEB_ROOT:-/srv/ota/gk1221}
PRODUCT_ID=${PRODUCT_ID:-$OTA_PRODUCT_ID}
PUBLISH_OWNER=${PUBLISH_OWNER:-}
PUBLISH_AUDIT_DIR=${PUBLISH_AUDIT_DIR:-$OTA_WORK_ROOT/audit}
HTTPS_BASE_URL=${HTTPS_BASE_URL:?set HTTPS_BASE_URL}

MQTT_HOST=${MQTT_HOST:?set MQTT_HOST}
MQTT_PORT=${MQTT_PORT:-8883}
MQTT_TOPIC=
PUBLISHER_DIR=${PUBLISHER_DIR:-/etc/app-ota/publisher}
MQTT_CA=${MQTT_CA:-$PUBLISHER_DIR/ca.pem}
MQTT_CERT=${MQTT_CERT:-$PUBLISHER_DIR/publisher.crt}
MQTT_KEY=${MQTT_KEY:-$PUBLISHER_DIR/publisher.key}
# Use the stable, release-controlled trust anchor. The generated rootfs output
# may disappear after `make clean`, so it must not be the publisher's default.
TRUSTED_RELEASE_PUBKEY=${TRUSTED_RELEASE_PUBKEY:?set TRUSTED_RELEASE_PUBKEY}

VERSION=
COUNTER=
CAMPAIGN_ID=
TMP_PUBLISH=
TMP_HTTPS=
PUBLISH_LOCK=

log()
{
    echo "[publish-network-ota] $*"
}

die()
{
    echo "[publish-network-ota] ERROR: $*" >&2
    exit 1
}

usage()
{
    echo "Usage: $0 --device-id DEVICE VERSION COUNTER [CAMPAIGN_ID]" >&2
    echo "Example:" >&2
    echo "  $0 --device-id device-001 \\" >&2
    echo "      V0.4.0 16 campaign-20260901-V0.4.0" >&2
    exit 2
}

cleanup()
{
    [ -z "$TMP_PUBLISH" ] || rm -rf "$TMP_PUBLISH"
    [ -z "$TMP_HTTPS" ] || rm -f "$TMP_HTTPS"
    [ -z "$PUBLISH_LOCK" ] || rmdir "$PUBLISH_LOCK" 2>/dev/null || true
}

field()
{
    key=$1
    file=$2
    count=$(grep -c "^${key}=" "$file" 2>/dev/null || true)
    [ "$count" = 1 ] || return 1
    sed -n "s/^${key}=//p" "$file"
}

trap cleanup EXIT
trap 'exit 1' HUP INT TERM
umask 027

# 所有选项必须放在 VERSION 前面。
while [ "$#" -gt 0 ]; do
    case "$1" in
        --sdk-root)
            shift
            [ "$#" -gt 0 ] || usage
            SDK_ROOT=$1
            shift
            ;;
        --sdk-root=*)
            SDK_ROOT=${1#--sdk-root=}
            shift
            ;;
        --device-id)
            shift
            [ "$#" -gt 0 ] || usage
            DEVICE_ID=$1
            shift
            ;;
        --device-id=*)
            DEVICE_ID=${1#--device-id=}
            shift
            ;;
        --)
            shift
            break
            ;;
        -h|--help)
            usage
            ;;
        -*)
            die "unknown option: $1"
            ;;
        *)
            break
            ;;
    esac
done

[ "$#" -ge 2 ] && [ "$#" -le 3 ] || usage
[ -n "$DEVICE_ID" ] || die "device id is required; pass --device-id DEVICE"

VERSION=$1
COUNTER=$2
CAMPAIGN_ID=${3:-}

BUNDLE_ROOT=${BUNDLE_ROOT:-$OTA_BUNDLE_ROOT}

[ -n "$VERSION" ] && [ -n "$COUNTER" ] || usage
printf '%s\n' "$VERSION" | grep -Eq '^V[0-9]+\.[0-9]+\.[0-9]+$' || \
    die "invalid VERSION: $VERSION"
case "$COUNTER" in
    ''|*[!0-9]*) die "COUNTER must be a decimal integer" ;;
esac
[ "$COUNTER" -gt 0 ] || die "COUNTER must be greater than zero"

[ -n "$CAMPAIGN_ID" ] || CAMPAIGN_ID="campaign-$(date -u +%Y%m%dT%H%M%SZ)-$VERSION"
case "$CAMPAIGN_ID" in
    ''|*[!A-Za-z0-9._:-]*) die "invalid CAMPAIGN_ID: $CAMPAIGN_ID" ;;
esac
case "$DEVICE_ID" in
    ''|*[!A-Za-z0-9._:-]*) die "invalid DEVICE_ID: $DEVICE_ID" ;;
esac

# MQTT主题和HTTPS notice必须由同一个 DEVICE_ID 派生，禁止分别配置。
MQTT_TOPIC="ota/$PRODUCT_ID/$DEVICE_ID/campaign"
HTTPS_BASE_URL=${HTTPS_BASE_URL%/}
case "$HTTPS_BASE_URL" in
    https://*) ;;
    *) die "HTTPS_BASE_URL must use https: $HTTPS_BASE_URL" ;;
esac

for command in openssl sha256sum cmp mosquitto_pub curl; do
    command -v "$command" >/dev/null 2>&1 || die "missing command: $command"
done
# Run as the account that owns WEB_ROOT; optional PUBLISH_OWNER requires chown rights.

BUNDLE_NAME=$VERSION-r$COUNTER
BUNDLE_DIR=$BUNDLE_ROOT/$BUNDLE_NAME
[ -d "$BUNDLE_DIR" ] || die "bundle does not exist: $BUNDLE_DIR"
for required in manifest.env manifest.sha256 signed-data.bin manifest.sig payload; do
    [ -e "$BUNDLE_DIR/$required" ] || die "bundle is missing: $required"
done
[ -s "$TRUSTED_RELEASE_PUBKEY" ] || die "trusted OTA public key is missing"
[ -s "$MQTT_CA" ] && [ -s "$MQTT_CERT" ] && [ -s "$MQTT_KEY" ] || \
    die "publisher mTLS credentials are incomplete"

MANIFEST_VERSION=$(field APP_VERSION "$BUNDLE_DIR/manifest.env") || die "invalid APP_VERSION field"
MANIFEST_COUNTER=$(field RELEASE_COUNTER "$BUNDLE_DIR/manifest.env") || die "invalid RELEASE_COUNTER field"
MANIFEST_PRODUCT=$(field PRODUCT_ID "$BUNDLE_DIR/manifest.env") || die "invalid PRODUCT_ID field"
[ "$MANIFEST_VERSION" = "$VERSION" ] || die "version mismatch: manifest=$MANIFEST_VERSION requested=$VERSION"
[ "$MANIFEST_COUNTER" = "$COUNTER" ] || die "counter mismatch: manifest=$MANIFEST_COUNTER requested=$COUNTER"
[ "$MANIFEST_PRODUCT" = "$PRODUCT_ID" ] || die "product mismatch: $MANIFEST_PRODUCT"

log "Reconstructing and verifying signed metadata"
RECONSTRUCTED=$BUNDLE_DIR/.reconstructed.publish.$$
trap 'rm -f "$RECONSTRUCTED"; cleanup' EXIT
cat "$BUNDLE_DIR/manifest.env" "$BUNDLE_DIR/manifest.sha256" > "$RECONSTRUCTED"
cmp -s "$RECONSTRUCTED" "$BUNDLE_DIR/signed-data.bin" || die "signed-data.bin does not match manifests"
openssl dgst -sha256 \
    -verify "$TRUSTED_RELEASE_PUBKEY" \
    -signature "$BUNDLE_DIR/manifest.sig" \
    "$BUNDLE_DIR/signed-data.bin" >/dev/null || die "OTA signature verification failed"
(cd "$BUNDLE_DIR/payload" && sha256sum -c "$BUNDLE_DIR/manifest.sha256") || \
    die "payload SHA-256 verification failed"
rm -f "$RECONSTRUCTED"

[ -d "$WEB_ROOT" ] || die "HTTPS root is missing: $WEB_ROOT"
[ -w "$WEB_ROOT" ] || die "HTTPS root is not writable"
mkdir "$WEB_ROOT/.publish.lock" 2>/dev/null || die "publication busy (or stale .publish.lock)"
PUBLISH_LOCK=$WEB_ROOT/.publish.lock

DEST=$WEB_ROOT/$BUNDLE_NAME
if [ -e "$DEST" ]; then
    [ -f "$DEST/signed-data.bin" ] && [ -f "$DEST/manifest.sig" ] || \
        die "existing destination is incomplete: $DEST"
    cmp -s "$BUNDLE_DIR/signed-data.bin" "$DEST/signed-data.bin" && \
        cmp -s "$BUNDLE_DIR/manifest.sig" "$DEST/manifest.sig" || \
        die "refusing to overwrite a different published version: $DEST"
    log "Identical HTTPS bundle already exists; reusing $DEST"
else
    TMP_PUBLISH=$WEB_ROOT/.publish-$BUNDLE_NAME-$$
    mkdir "$TMP_PUBLISH" || die "cannot create temporary publish directory"
    cp -a "$BUNDLE_DIR/." "$TMP_PUBLISH/"
    [ -z "$PUBLISH_OWNER" ] || chown -R "$PUBLISH_OWNER" "$TMP_PUBLISH"
    find "$TMP_PUBLISH" -type d -exec chmod 2750 {} \;
    find "$TMP_PUBLISH" -type f -exec chmod 0640 {} \;
    mv "$TMP_PUBLISH" "$DEST"
    TMP_PUBLISH=
    log "HTTPS bundle atomically published to $DEST"
fi

mkdir -p "$WEB_ROOT/campaigns"
NOTICE=$WEB_ROOT/campaigns/$DEVICE_ID.notice
NOTICE_TMP=$WEB_ROOT/campaigns/.$DEVICE_ID.notice.$$
printf '%s\n' \
    'FORMAT_VERSION=1' \
    "PRODUCT_ID=$PRODUCT_ID" \
    "CAMPAIGN_ID=$CAMPAIGN_ID" \
    "APP_VERSION=$VERSION" \
    "RELEASE_COUNTER=$COUNTER" \
    "BUNDLE_URL=$HTTPS_BASE_URL/$BUNDLE_NAME" \
    > "$NOTICE_TMP"
chmod 0640 "$NOTICE_TMP"
[ -z "$PUBLISH_OWNER" ] || chown "$PUBLISH_OWNER" "$NOTICE_TMP"
mv "$NOTICE_TMP" "$NOTICE"

TMP_HTTPS=$(mktemp)
curl --silent --show-error --fail --proto '=https' --tlsv1.2 \
    --cacert "$MQTT_CA" --cert "$MQTT_CERT" --key "$MQTT_KEY" \
    --output "$TMP_HTTPS" \
    "$HTTPS_BASE_URL/$BUNDLE_NAME/manifest.env" || \
    die "HTTPS/mTLS publication probe failed"
cmp -s "$TMP_HTTPS" "$BUNDLE_DIR/manifest.env" || die "HTTPS manifest differs from source"

# 同时探测设备专属notice，发布阶段即可发现404或设备ID错位。
rm -f "$TMP_HTTPS"
TMP_HTTPS=$(mktemp)
curl --silent --show-error --fail --proto '=https' --tlsv1.2 \
    --cacert "$MQTT_CA" --cert "$MQTT_CERT" --key "$MQTT_KEY" \
    --output "$TMP_HTTPS" \
    "$HTTPS_BASE_URL/campaigns/$DEVICE_ID.notice" || \
    die "HTTPS/mTLS device notice probe failed: $DEVICE_ID"
cmp -s "$TMP_HTTPS" "$NOTICE" || die "HTTPS device notice differs from source"
rm -f "$TMP_HTTPS"
TMP_HTTPS=

log "Publishing MQTT QoS 1 retained Campaign"
mosquitto_pub \
    -h "$MQTT_HOST" -p "$MQTT_PORT" \
    --cafile "$MQTT_CA" --cert "$MQTT_CERT" --key "$MQTT_KEY" \
    -t "$MQTT_TOPIC" -q 1 -r -f "$NOTICE" || die "MQTT publish failed"

mkdir -p "$PUBLISH_AUDIT_DIR"
printf '%s event=CAMPAIGN_PUBLISHED device=%s campaign=%s version=%s counter=%s\n' \
    "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "$DEVICE_ID" "$CAMPAIGN_ID" "$VERSION" "$COUNTER" \
    >> "$PUBLISH_AUDIT_DIR/publish.log"
[ -z "$PUBLISH_OWNER" ] || chown -R "$PUBLISH_OWNER" "$PUBLISH_AUDIT_DIR"

log "SUCCESS: Campaign published"
log "Device=$DEVICE_ID"
log "Topic=$MQTT_TOPIC"
log "Notice=$HTTPS_BASE_URL/campaigns/$DEVICE_ID.notice"
log "Version=$VERSION counter=$COUNTER campaign=$CAMPAIGN_ID"
log "The device Agent may download and stage; this script never authorizes or reboots it."
