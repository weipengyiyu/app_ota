#!/bin/sh
# Shared, fail-closed state helpers for GK1221 APP A/B.

OTA_PRODUCT_CONFIG=${OTA_PRODUCT_CONFIG:-/etc/ota/product.conf}
[ ! -r "$OTA_PRODUCT_CONFIG" ] || . "$OTA_PRODUCT_CONFIG"
OTA_PRODUCT_ID=${OTA_PRODUCT_ID:-GK1221-ADAS}
OTA_HARDWARE_REVISION=${OTA_HARDWARE_REVISION:-xm1221v105}
OTA_ROOTFS_ABI=${OTA_ROOTFS_ABI:-xm1221v105-r1}
OTA_APP_BINARY=${OTA_APP_BINARY:-demo_ai}
OTA_APP_LAUNCHER=${OTA_APP_LAUNCHER:-start_imx728_adas.sh}

OTA_LOCAL_DIR="${OTA_LOCAL_DIR:-/mnt/local}"
OTA_SLOT_DIR="${OTA_SLOT_DIR:-$OTA_LOCAL_DIR/slots}"
OTA_STATE_DIR="${OTA_STATE_DIR:-$OTA_LOCAL_DIR/ota_state}"
OTA_AUDIT_DIR="${OTA_AUDIT_DIR:-$OTA_LOCAL_DIR/ota_audit}"
OTA_RUN_DIR="${OTA_RUN_DIR:-/run/ota}"
OTA_SLOT_MAX_KIB="${OTA_SLOT_MAX_KIB:-32768}"
OTA_RESERVE_KIB="${OTA_RESERVE_KIB:-18432}"

ota_defaults() {
    STATE_FORMAT=1
    GENERATION=0
    TRANSACTION_ID=NONE
    CAMPAIGN_ID=NONE
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
    MAX_BOOT_ATTEMPTS="${OTA_MAX_BOOT_ATTEMPTS:-3}"
    LAST_ERROR=NONE
}

ota_is_uint() {
    case "${1:-}" in
        ''|*[!0-9]*) return 1 ;;
        *) return 0 ;;
    esac
}

ota_is_token() {
    case "${1:-}" in
        ''|*[!A-Za-z0-9._:-]*) return 1 ;;
        *) return 0 ;;
    esac
}

ota_slot_name_valid() {
    [ "${1:-}" = A ] || [ "${1:-}" = B ]
}

ota_slot_root() {
    case "${1:-}" in
        A) echo "$OTA_SLOT_DIR/app_A/root" ;;
        B) echo "$OTA_SLOT_DIR/app_B/root" ;;
        *) return 1 ;;
    esac
}

ota_slot_base() {
    case "${1:-}" in
        A) echo "$OTA_SLOT_DIR/app_A" ;;
        B) echo "$OTA_SLOT_DIR/app_B" ;;
        *) return 1 ;;
    esac
}

ota_audit() {
    ota_event="${1:-UNKNOWN}"
    ota_detail="${2:-NONE}"
    mkdir -p "$OTA_AUDIT_DIR"
    ota_log="$OTA_AUDIT_DIR/events.log"
    if [ -f "$ota_log" ]; then
        ota_bytes=$(wc -c < "$ota_log" 2>/dev/null || echo 0)
        if ota_is_uint "$ota_bytes" && [ "$ota_bytes" -gt 524288 ]; then
            mv -f "$ota_log" "$ota_log.1"
        fi
    fi
    printf '%s event=%s detail=%s\n' "$(date -u '+%Y-%m-%dT%H:%M:%SZ' 2>/dev/null || date)" "$ota_event" "$ota_detail" >> "$ota_log"
}

ota_state_file_valid() {
    ota_file="$1"
    [ -s "$ota_file" ] && [ -s "$ota_file.sha256" ] || return 1
    ota_expected=$(awk 'NR == 1 { print $1 }' "$ota_file.sha256" 2>/dev/null)
    ota_actual=$(sha256sum "$ota_file" 2>/dev/null | awk '{ print $1 }')
    [ -n "$ota_expected" ] && [ "$ota_expected" = "$ota_actual" ]
}

ota_state_generation() {
    awk -F= '$1 == "GENERATION" { print $2; exit }' "$1" 2>/dev/null
}

ota_parse_state() {
    ota_defaults
    ota_seen='|'
    ota_fields=0
    while IFS='=' read -r ota_key ota_value; do
        case "$ota_seen" in *"|$ota_key|"*) return 1 ;; esac
        ota_seen="$ota_seen$ota_key|"
        ota_fields=$((ota_fields + 1))
        case "$ota_key" in
            STATE_FORMAT) STATE_FORMAT="$ota_value" ;;
            GENERATION) GENERATION="$ota_value" ;;
            TRANSACTION_ID) TRANSACTION_ID="$ota_value" ;;
            CAMPAIGN_ID) CAMPAIGN_ID="$ota_value" ;;
            PHASE) PHASE="$ota_value" ;;
            ACTIVE_SLOT) ACTIVE_SLOT="$ota_value" ;;
            CONFIRMED_SLOT) CONFIRMED_SLOT="$ota_value" ;;
            PENDING_SLOT) PENDING_SLOT="$ota_value" ;;
            ACTIVE_ARTIFACT) ACTIVE_ARTIFACT="$ota_value" ;;
            PENDING_ARTIFACT) PENDING_ARTIFACT="$ota_value" ;;
            ACTIVE_VERSION) ACTIVE_VERSION="$ota_value" ;;
            PENDING_VERSION) PENDING_VERSION="$ota_value" ;;
            ACTIVE_COUNTER) ACTIVE_COUNTER="$ota_value" ;;
            PENDING_COUNTER) PENDING_COUNTER="$ota_value" ;;
            BOOT_ATTEMPTS) BOOT_ATTEMPTS="$ota_value" ;;
            MAX_BOOT_ATTEMPTS) MAX_BOOT_ATTEMPTS="$ota_value" ;;
            LAST_ERROR) LAST_ERROR="$ota_value" ;;
            *) return 1 ;;
        esac
    done < "$1"
    [ "$ota_fields" -eq 17 ] || return 1
    ota_is_uint "$STATE_FORMAT" && [ "$STATE_FORMAT" -eq 1 ] || return 1
    ota_is_uint "$GENERATION" || return 1
    ota_is_uint "$ACTIVE_COUNTER" || return 1
    ota_is_uint "$PENDING_COUNTER" || return 1
    ota_is_uint "$BOOT_ATTEMPTS" || return 1
    ota_is_uint "$MAX_BOOT_ATTEMPTS" || return 1
    ota_slot_name_valid "$ACTIVE_SLOT" || return 1
    ota_slot_name_valid "$CONFIRMED_SLOT" || return 1
    [ "$PENDING_SLOT" = NONE ] || ota_slot_name_valid "$PENDING_SLOT" || return 1
    ota_is_token "$TRANSACTION_ID" || return 1
    ota_is_token "$CAMPAIGN_ID" || return 1
    ota_is_token "$PHASE" || return 1
    case "$PHASE" in ACTIVE|STAGED|TRIAL_ARMED|TRIAL|ROLLED_BACK) ;; *) return 1 ;; esac
    ota_is_token "$ACTIVE_ARTIFACT" || return 1
    ota_is_token "$PENDING_ARTIFACT" || return 1
    ota_is_token "$ACTIVE_VERSION" || return 1
    ota_is_token "$PENDING_VERSION" || return 1
    ota_is_token "$LAST_ERROR" || return 1
}

ota_load_state() {
    ota_f0="$OTA_STATE_DIR/state.0"
    ota_f1="$OTA_STATE_DIR/state.1"
    ota_g0=-1
    ota_g1=-1
    ota_state_file_valid "$ota_f0" && ota_g0=$(ota_state_generation "$ota_f0")
    ota_state_file_valid "$ota_f1" && ota_g1=$(ota_state_generation "$ota_f1")
    ota_is_uint "$ota_g0" || ota_g0=-1
    ota_is_uint "$ota_g1" || ota_g1=-1

    if [ "$ota_g0" -lt 0 ] && [ "$ota_g1" -lt 0 ]; then
        return 1
    fi
    if [ "$ota_g1" -gt "$ota_g0" ]; then
        ota_parse_state "$ota_f1" && return 0
        [ "$ota_g0" -ge 0 ] && ota_parse_state "$ota_f0"
    else
        ota_parse_state "$ota_f0" && return 0
        [ "$ota_g1" -ge 0 ] && ota_parse_state "$ota_f1"
    fi
}

# Serialize installers and all bootctl state operations. A child bootctl may
# inherit the installer's lock; only the process that created it releases it.
# No guessed stale-PID takeover: /run is volatile and operator recovery is explicit.
ota_lock() {
    OTA_LOCK_ACQUIRED=0
    mkdir -p "$OTA_RUN_DIR" || return 1
    OTA_LOCK_PATH=$OTA_RUN_DIR/transaction.lock
    if [ -n "${OTA_LOCK_OWNER:-}" ] &&
       [ "$(cat "$OTA_LOCK_PATH/owner" 2>/dev/null)" = "$OTA_LOCK_OWNER" ] &&
       kill -0 "$OTA_LOCK_OWNER" 2>/dev/null; then
        return 0
    fi
    mkdir "$OTA_LOCK_PATH" 2>/dev/null || {
        echo "APP-OTA transaction is busy (or stale lock): $OTA_LOCK_PATH" >&2
        return 1
    }
    OTA_LOCK_ACQUIRED=1
    OTA_LOCK_OWNER=$$
    export OTA_LOCK_OWNER
    printf '%s\n' "$OTA_LOCK_OWNER" > "$OTA_LOCK_PATH/owner" || { ota_unlock; return 1; }
}

ota_unlock() {
    if [ "${OTA_LOCK_ACQUIRED:-0}" = 1 ]; then
        rm -f "$OTA_LOCK_PATH/owner"
        rmdir "$OTA_LOCK_PATH" 2>/dev/null || true
        OTA_LOCK_ACQUIRED=0
    fi
}

ota_write_state() {
    mkdir -p "$OTA_STATE_DIR"
    GENERATION=$((GENERATION + 1))
    ota_index=$((GENERATION % 2))
    ota_target="$OTA_STATE_DIR/state.$ota_index"
    ota_tmp="$OTA_STATE_DIR/.state.$ota_index.$$.tmp"
    ota_sum_tmp="$ota_tmp.sha256"
    umask 077
    {
        echo "STATE_FORMAT=1"
        echo "GENERATION=$GENERATION"
        echo "TRANSACTION_ID=$TRANSACTION_ID"
        echo "CAMPAIGN_ID=$CAMPAIGN_ID"
        echo "PHASE=$PHASE"
        echo "ACTIVE_SLOT=$ACTIVE_SLOT"
        echo "CONFIRMED_SLOT=$CONFIRMED_SLOT"
        echo "PENDING_SLOT=$PENDING_SLOT"
        echo "ACTIVE_ARTIFACT=$ACTIVE_ARTIFACT"
        echo "PENDING_ARTIFACT=$PENDING_ARTIFACT"
        echo "ACTIVE_VERSION=$ACTIVE_VERSION"
        echo "PENDING_VERSION=$PENDING_VERSION"
        echo "ACTIVE_COUNTER=$ACTIVE_COUNTER"
        echo "PENDING_COUNTER=$PENDING_COUNTER"
        echo "BOOT_ATTEMPTS=$BOOT_ATTEMPTS"
        echo "MAX_BOOT_ATTEMPTS=$MAX_BOOT_ATTEMPTS"
        echo "LAST_ERROR=$LAST_ERROR"
    } > "$ota_tmp" || return 1
    sha256sum "$ota_tmp" | awk '{ print $1 }' > "$ota_sum_tmp" || return 1
    sync
    mv -f "$ota_tmp" "$ota_target" || return 1
    mv -f "$ota_sum_tmp" "$ota_target.sha256" || return 1
    sync
}

ota_verify_slot() {
    ota_slot="$1"
    ota_base=$(ota_slot_base "$ota_slot") || return 1
    ota_root="$ota_base/root"
    [ -d "$ota_root" ] || return 1
    [ -f "$ota_base/slot.ready" ] || return 1
    [ -s "$ota_base/slot.meta" ] || return 1
    [ -s "$ota_base/manifest.sha256" ] || return 1
    [ -x "$ota_root/$OTA_APP_BINARY" ] || return 1
    (cd "$ota_root" && sha256sum -c "$ota_base/manifest.sha256" >/dev/null 2>&1)
}

ota_slot_meta() {
    awk -F= -v key="$1" '$1 == key { n++; value=$2 } END { if(n!=1) exit 1; print value }' "$2" 2>/dev/null
}

# Reconstruct identity as well as slot choice. Defaulting a recovered release
# to factory/counter=0 would silently lower the anti-replay floor.
ota_state_from_valid_slots() {
    ota_recover_best=
    ota_recover_counter=-1
    for ota_recover_slot in A B; do
        ota_verify_slot "$ota_recover_slot" || continue
        ota_recover_meta=$(ota_slot_base "$ota_recover_slot")/slot.meta
        [ "$(ota_slot_meta PRODUCT_ID "$ota_recover_meta")" = "$OTA_PRODUCT_ID" ] || continue
        [ "$(ota_slot_meta HARDWARE_REVISION "$ota_recover_meta")" = "$OTA_HARDWARE_REVISION" ] || continue
        [ "$(ota_slot_meta ROOTFS_ABI "$ota_recover_meta")" = "$OTA_ROOTFS_ABI" ] || continue
        ota_recover_artifact=$(ota_slot_meta ARTIFACT_ID "$ota_recover_meta")
        ota_recover_n=$(ota_slot_meta RELEASE_COUNTER "$ota_recover_meta")
        ota_recover_format=$(ota_slot_meta FORMAT_VERSION "$ota_recover_meta")
        ota_is_token "$ota_recover_artifact" && ota_is_uint "$ota_recover_n" || continue
        case "$ota_recover_format" in
            2) ;;
            '') [ "$ota_recover_artifact" = factory ] && [ "$ota_recover_n" -eq 0 ] || continue ;;
            *) continue ;;
        esac
        [ ! -e "$OTA_STATE_DIR/quarantine/$ota_recover_artifact.reason" ] || continue
        ota_recover_version=$(ota_slot_meta APP_VERSION "$ota_recover_meta")
        ota_recover_tx=$(ota_slot_meta TRANSACTION_ID "$ota_recover_meta")
        ota_recover_campaign=$(ota_slot_meta CAMPAIGN_ID "$ota_recover_meta")
        ota_is_token "$ota_recover_version" && ota_is_token "$ota_recover_tx" && ota_is_token "$ota_recover_campaign" || continue
        if [ "$ota_recover_n" -gt "$ota_recover_counter" ]; then
            ota_recover_best=$ota_recover_slot
            ota_recover_counter=$ota_recover_n
            ota_recover_best_artifact=$ota_recover_artifact
            ota_recover_best_version=$ota_recover_version
            ota_recover_best_tx=$ota_recover_tx
            ota_recover_best_campaign=$ota_recover_campaign
        fi
    done
    [ -n "$ota_recover_best" ] || return 1
    ota_defaults
    ACTIVE_SLOT=$ota_recover_best
    CONFIRMED_SLOT=$ota_recover_best
    ACTIVE_ARTIFACT=$ota_recover_best_artifact
    ACTIVE_VERSION=$ota_recover_best_version
    ACTIVE_COUNTER=$ota_recover_counter
    TRANSACTION_ID=$ota_recover_best_tx
    CAMPAIGN_ID=$ota_recover_best_campaign
    LAST_ERROR=STATE_REBUILT
}

ota_slot_size_kib() {
    ota_size_base=$(ota_slot_base "$1") || return 1
    du -sk "$ota_size_base" 2>/dev/null | awk '{ print $1 }'
}

ota_available_kib() {
    df -Pk "$OTA_LOCAL_DIR" 2>/dev/null | awk 'NR == 2 { print $4 }'
}

ota_prepare_runtime() {
    mkdir -p "$OTA_RUN_DIR"
    chmod 0755 "$OTA_RUN_DIR"
}
