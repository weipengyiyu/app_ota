#!/bin/sh
# Build and sign a canonical pre-extracted APP bundle on the release host.
set -eu

SCRIPT_DIR=$(CDPATH= cd "$(dirname "$0")" && pwd)
# shellcheck source=ota-release-paths.sh
. "$SCRIPT_DIR/../../lib/host-env.sh"

usage()
{
    echo "Usage: $0 <app-source> <version> <release-counter> [output-dir|campaign-id] [campaign-id]" >&2
    echo "  output-dir defaults to $OTA_BUNDLE_ROOT/<version>-r<counter>" >&2
    exit 2
}

case "${1:-}" in -h|--help) usage ;; esac

if [ "$#" -lt 3 ] || [ "$#" -gt 5 ]; then
    usage
fi

SRC=$1
VERSION=$2
COUNTER=$3
shift 3

if [ "$#" -eq 0 ]; then
    OUT=$OTA_BUNDLE_ROOT/$VERSION-r$COUNTER
    CAMPAIGN=manual-service-$VERSION
elif [ "$#" -eq 1 ]; then
    case "$1" in
        "$OTA_BUNDLE_ROOT"/*|/*|./*|../*)
            OUT=$1
            CAMPAIGN=manual-service-$VERSION
            ;;
        *)
            OUT=$OTA_BUNDLE_ROOT/$VERSION-r$COUNTER
            CAMPAIGN=$1
            ;;
    esac
else
    OUT=$1
    CAMPAIGN=$2
fi
SIGN_KEY=${SIGN_KEY:?set SIGN_KEY to an external signing key}
PRODUCT_ID=${PRODUCT_ID:-$OTA_PRODUCT_ID}
HARDWARE_REVISION=${HARDWARE_REVISION:-$OTA_HARDWARE_REVISION}
ROOTFS_ABI=${ROOTFS_ABI:-$OTA_ROOTFS_ABI}
SLOT_MAX_KIB=${OTA_SLOT_MAX_KIB:-32768}

case "$VERSION:$CAMPAIGN" in *[!A-Za-z0-9._:-]*) echo "Invalid version/campaign token" >&2; exit 1 ;; esac
case "$COUNTER" in ''|*[!0-9]*) echo "Invalid release counter" >&2; exit 1 ;; esac
[ "$COUNTER" -gt 0 ] || { echo "Release counter must be positive" >&2; exit 1; }
[ -d "$SRC" ] || { echo "Missing APP source: $SRC" >&2; exit 1; }
[ -s "$SIGN_KEY" ] || { echo "Missing signing key: $SIGN_KEY" >&2; exit 1; }
if find "$SRC" -type l -print | grep -q .; then
    echo "Symbolic links are forbidden in APP source" >&2
    exit 1
fi
if find "$SRC" ! -type f ! -type d -print | grep -q .; then
    echo "Special files are forbidden in APP source" >&2
    exit 1
fi
if find "$SRC" -print | grep -q '[[:space:]]'; then
    echo "Whitespace in APP paths is forbidden" >&2
    exit 1
fi
if (cd "$SRC" && find . -print | LC_ALL=C grep -q '[^A-Za-z0-9._/-]'); then
    echo "Unsupported character in APP relative path" >&2
    exit 1
fi

[ ! -e "$OUT" ] || { echo "Refusing to overwrite existing bundle: $OUT" >&2; exit 1; }
mkdir -p "$(dirname "$OUT")"
OUT_PARENT=$(CDPATH= cd "$(dirname "$OUT")" && pwd -P)
SRC_REAL=$(CDPATH= cd "$SRC" && pwd -P)
case "$OUT_PARENT/$(basename "$OUT")/" in "$SRC_REAL/"*) echo "Bundle output must be outside APP source" >&2; exit 1 ;; esac
mkdir "$OUT"
mkdir "$OUT/payload"
for item in $OTA_APP_ITEMS; do
    [ -e "$SRC/$item" ] || { echo "Required item missing: $item" >&2; exit 1; }
    cp -a "$SRC/$item" "$OUT/payload/"
done

find "$OUT/payload" -type d -exec chmod 0755 {} \;
find "$OUT/payload" -type f -exec chmod 0644 {} \;
find "$OUT/payload" -type f -name '*.sh' -exec chmod 0755 {} \;
chmod 0755 "$OUT/payload/$OTA_APP_BINARY"

(cd "$OUT/payload" && find . -type f -print | LC_ALL=C sort | xargs sha256sum) > "$OUT/manifest.sha256"
UNPACKED_KIB=$(du -sk "$OUT/payload" | awk '{ print $1 }')
FILE_COUNT=$(wc -l < "$OUT/manifest.sha256")
[ "$UNPACKED_KIB" -le "$SLOT_MAX_KIB" ] || { echo "APP exceeds ${SLOT_MAX_KIB} KiB" >&2; exit 1; }

COMMIT=$(git -C "$SRC" rev-parse --short=12 HEAD 2>/dev/null || echo unknown)
TRANSACTION_ID="tx-$(date -u +%Y%m%d%H%M%S)-$$"
ARTIFACT_ID="${VERSION}-r${COUNTER}-${COMMIT}"

cat > "$OUT/manifest.env" <<EOF
FORMAT_VERSION=2
PRODUCT_ID=$PRODUCT_ID
HARDWARE_REVISION=$HARDWARE_REVISION
ROOTFS_ABI=$ROOTFS_ABI
ARTIFACT_ID=$ARTIFACT_ID
APP_VERSION=$VERSION
RELEASE_COUNTER=$COUNTER
UNPACKED_KIB=$UNPACKED_KIB
FILE_COUNT=$FILE_COUNT
TRANSACTION_ID=$TRANSACTION_ID
CAMPAIGN_ID=$CAMPAIGN
BUILD_COMMIT=$COMMIT
EOF

cat "$OUT/manifest.env" "$OUT/manifest.sha256" > "$OUT/signed-data.bin"
openssl dgst -sha256 -sign "$SIGN_KEY" -out "$OUT/manifest.sig" "$OUT/signed-data.bin"
openssl pkey -in "$SIGN_KEY" -pubout -out "$OUT/release-public-key.pem"

echo "Bundle: $OUT"
echo "Artifact: $ARTIFACT_ID"
echo "Payload: ${UNPACKED_KIB} KiB, ${FILE_COUNT} files"
echo "Counter: $COUNTER"
