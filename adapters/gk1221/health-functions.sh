#!/bin/sh
# GK1221 structured demo_ai health-log adapter, sourced by activation gate.
factory_health_bootstrap() {
    ota_bootstrap_reason="$1"
    [ "$ALLOW_FACTORY_HEALTH_BOOTSTRAP" = 1 ] || return 1
    [ "$ACTIVE_ARTIFACT" = factory ] || return 1
    [ "$ACTIVE_COUNTER" = 0 ] || return 1
    ota_audit ACTIVATION_FACTORY_HEALTH_BOOTSTRAP \
        "reason=$ota_bootstrap_reason,active_slot=$ACTIVE_SLOT"
    return 0
}

find_health_log() {
    health_pid="$1"
    # Only the structured health log for the current demo_ai PID is trusted.
    # Supervisor/stdout logs use different names and cannot satisfy the gate.
    ls -1t "$APP_HEALTH_LOG_DIR"/demo_ai_*_"$health_pid".log 2>/dev/null | head -1
}

alive_metric() {
    alive_line="$1"
    alive_key="$2"
    echo "$alive_line" | sed -n "s/.*${alive_key}=\([0-9][0-9]*\).*/\1/p"
}

check_current_app_health() {
    CURRENT_APP_HEALTH_REASON=CURRENT_APP_UNHEALTHY

    ota_pid=$(cat "$OTA_RUN_DIR/demo_ai.pid" 2>/dev/null || true)
    ota_is_uint "$ota_pid" && kill -0 "$ota_pid" 2>/dev/null || return 1

    ota_log=$(find_health_log "$ota_pid")
    if [ -z "$ota_log" ] || [ ! -f "$ota_log" ]; then
        factory_health_bootstrap LOG_MISSING && return 0
        CURRENT_APP_HEALTH_REASON=CURRENT_APP_LOG_MISSING
        return 1
    fi

    printf '%s\n' "$ota_log" > "$OTA_RUN_DIR/demo_ai.health.log.path"
    ota_log_mtime=$(stat -c %Y "$ota_log" 2>/dev/null) || {
        CURRENT_APP_HEALTH_REASON=CURRENT_APP_LOG_TIMESTAMP_UNAVAILABLE
        return 1
    }
    ota_is_uint "$ota_log_mtime" || {
        CURRENT_APP_HEALTH_REASON=CURRENT_APP_LOG_TIMESTAMP_INVALID
        return 1
    }
    ota_log_age=$((ota_now - ota_log_mtime))
    if [ "$ota_log_age" -lt 0 ] || [ "$ota_log_age" -gt "$APP_HEALTH_MAX_AGE_SEC" ]; then
        CURRENT_APP_HEALTH_REASON=CURRENT_APP_LOG_STALE
        return 1
    fi

    ota_alive_pair=$(grep "ALIVE pid=$ota_pid " "$ota_log" 2>/dev/null | tail -2)
    ota_alive_count=$(echo "$ota_alive_pair" | grep -c "ALIVE pid=$ota_pid " 2>/dev/null || true)
    if [ "$ota_alive_count" -lt 2 ]; then
        CURRENT_APP_HEALTH_REASON=CURRENT_APP_HEALTH_WINDOW_INSUFFICIENT
        return 1
    fi

    ota_alive_prev=$(echo "$ota_alive_pair" | head -1)
    ota_alive_last=$(echo "$ota_alive_pair" | tail -1)
    ota_prev_svp=$(alive_metric "$ota_alive_prev" svp_ok)
    ota_last_svp=$(alive_metric "$ota_alive_last" svp_ok)
    ota_prev_vpss_fail=$(alive_metric "$ota_alive_prev" vpss_fail)
    ota_last_vpss_fail=$(alive_metric "$ota_alive_last" vpss_fail)
    ota_prev_svp_fail=$(alive_metric "$ota_alive_prev" svp_fail)
    ota_last_svp_fail=$(alive_metric "$ota_alive_last" svp_fail)

    for ota_metric in \
        "$ota_prev_svp" "$ota_last_svp" \
        "$ota_prev_vpss_fail" "$ota_last_vpss_fail" \
        "$ota_prev_svp_fail" "$ota_last_svp_fail"; do
        ota_is_uint "$ota_metric" || {
            CURRENT_APP_HEALTH_REASON=CURRENT_APP_HEALTH_METRIC_INVALID
            return 1
        }
    done

    if [ "$ota_last_vpss_fail" -gt "$ota_prev_vpss_fail" ]; then
        CURRENT_APP_HEALTH_REASON=CURRENT_APP_VPSS_UNHEALTHY
        return 1
    fi
    if [ "$ota_last_svp_fail" -gt "$ota_prev_svp_fail" ]; then
        CURRENT_APP_HEALTH_REASON=CURRENT_APP_SVP_UNHEALTHY
        return 1
    fi
    if [ "$ota_last_svp" -le "$ota_prev_svp" ]; then
        CURRENT_APP_HEALTH_REASON=CURRENT_APP_NO_FRAME_PROGRESS
        return 1
    fi
    return 0
}

