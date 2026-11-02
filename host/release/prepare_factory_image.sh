#!/bin/sh
# Prepare the factory YAFFS content with a confirmed slot A.
set -eu
SCRIPT_DIR=$(CDPATH= cd "$(dirname "$0")" && pwd)
. "$SCRIPT_DIR/../../lib/host-env.sh"

[ "$#" -eq 2 ] || { echo "Usage: $0 <jmapp-source> <stage-dir>" >&2; exit 2; }
SRC=$1
STAGE=$2

if find "$SRC" -type l -print | grep -q . || \
   find "$SRC" ! -type f ! -type d -print | grep -q . || \
   find "$SRC" -print | grep -q '[[:space:]]'; then
    echo "Unsafe file type or path in factory APP source" >&2
    exit 1
fi

case "$STAGE" in */out/*|*/.build/*) ;; *) echo "Unsafe factory stage path: $STAGE" >&2; exit 1 ;; esac
[ ! -e "$STAGE" ] || { echo "Factory stage already exists: $STAGE" >&2; exit 1; }
mkdir -p "$STAGE/slots/app_A/root" "$STAGE/slots/app_B" "$STAGE/ota_state/quarantine" \
    "$STAGE/app_data" "$STAGE/app_config" "$STAGE/app_log" "$STAGE/ota_audit"

for item in $OTA_APP_ITEMS; do
    [ -e "$SRC/$item" ] || { echo "Required factory item missing: $item" >&2; exit 1; }
    cp -a "$SRC/$item" "$STAGE/slots/app_A/root/"
done

find "$STAGE/slots/app_A/root" -type d -exec chmod 0755 {} \;
find "$STAGE/slots/app_A/root" -type f -exec chmod 0644 {} \;
find "$STAGE/slots/app_A/root" -type f -name '*.sh' -exec chmod 0755 {} \;
chmod 0755 "$STAGE/slots/app_A/root/$OTA_APP_BINARY"

(cd "$STAGE/slots/app_A/root" && find . -type f -print | LC_ALL=C sort | xargs sha256sum) > "$STAGE/slots/app_A/manifest.sha256"
SIZE=$(du -sk "$STAGE/slots/app_A" | awk '{ print $1 }')
COUNT=$(wc -l < "$STAGE/slots/app_A/manifest.sha256")
[ "$SIZE" -le "$OTA_SLOT_MAX_KIB" ] || { echo "Factory APP exceeds 32 MiB" >&2; exit 1; }
cat > "$STAGE/slots/app_A/slot.meta" <<EOF
FORMAT_VERSION=2
PRODUCT_ID=$OTA_PRODUCT_ID
HARDWARE_REVISION=$OTA_HARDWARE_REVISION
ROOTFS_ABI=$OTA_ROOTFS_ABI
ARTIFACT_ID=factory
APP_VERSION=V0.0.0
RELEASE_COUNTER=0
UNPACKED_KIB=$SIZE
FILE_COUNT=$COUNT
TRANSACTION_ID=factory
CAMPAIGN_ID=factory
EOF
touch "$STAGE/slots/app_A/slot.ready"

cat > "$STAGE/ota_state/state.1" <<EOF
STATE_FORMAT=1
GENERATION=1
TRANSACTION_ID=factory
CAMPAIGN_ID=factory
PHASE=ACTIVE
ACTIVE_SLOT=A
CONFIRMED_SLOT=A
PENDING_SLOT=NONE
ACTIVE_ARTIFACT=factory
PENDING_ARTIFACT=NONE
ACTIVE_VERSION=V0.0.0
PENDING_VERSION=NONE
ACTIVE_COUNTER=0
PENDING_COUNTER=0
BOOT_ATTEMPTS=0
MAX_BOOT_ATTEMPTS=3
LAST_ERROR=NONE
EOF
sha256sum "$STAGE/ota_state/state.1" | awk '{ print $1 }' > "$STAGE/ota_state/state.1.sha256"
sed 's/^GENERATION=1$/GENERATION=2/' "$STAGE/ota_state/state.1" > "$STAGE/ota_state/state.0"
sha256sum "$STAGE/ota_state/state.0" | awk '{ print $1 }' > "$STAGE/ota_state/state.0.sha256"
chmod 0700 "$STAGE/ota_state"
echo "Factory A/B stage prepared: $STAGE (${SIZE} KiB)"
