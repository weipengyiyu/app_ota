/*
 * demo_ai health log — startup / runtime / crash / network health.
 */

#ifndef __DEMO_HEALTH_LOG_H__
#define __DEMO_HEALTH_LOG_H__

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    DEMO_HEALTH_LEVEL_INFO = 0,
    DEMO_HEALTH_LEVEL_WARN = 1,
    DEMO_HEALTH_LEVEL_ERROR = 2,
} demo_health_level;

typedef enum {
    DEMO_HEALTH_THR_SVP = 0,
    DEMO_HEALTH_THR_VENC = 1,
    DEMO_HEALTH_THR_MAIN = 2,
    DEMO_HEALTH_THR_MAX
} demo_health_thread_id;

/* Software pipeline stages timed in sample_svp_proc (linear path). */
typedef enum {
    DEMO_HEALTH_PIPE_VPSS_ACQUIRE = 0,
    DEMO_HEALTH_PIPE_NPU = 1,
    DEMO_HEALTH_PIPE_VENC_SEND = 2,
    DEMO_HEALTH_PIPE_FRAME_HOLD = 3,
    DEMO_HEALTH_PIPE_MAX
} demo_health_pipe_stage;

typedef struct {
    unsigned long long svp_ok_frames;
    unsigned long long vpss_fail_cnt;
    unsigned long long svp_fail_cnt;
    unsigned long long venc_timeout_cnt;
    unsigned long long venc_send_fail_cnt;
    unsigned long long rtsp_fail_cnt;
    unsigned long long net_link_down_cnt;
    unsigned long long net_stall_cnt;
    unsigned long long net_event_cnt;
    int last_svp_ret;
    int last_target_num;
    int net_carrier;          /* 1=up 0=down -1=unknown */
    char net_ifname[32];
    unsigned long long net_rx_bytes;
    unsigned long long net_tx_bytes;
    unsigned long long net_rx_packets;
    unsigned long long net_tx_packets;
    unsigned long long net_rx_errors;
    unsigned long long net_tx_errors;
    unsigned long long net_rx_dropped;
    unsigned long long net_tx_dropped;
} demo_health_stats;

/* Per-module disk log switches (used when enable_disk=1). */
typedef struct {
    int core;       /* boot/alive/stall/state */
    int net;        /* NET* */
    int hw;         /* HW_CHAIN* */
    int bandwidth;  /* BANDWIDTH* */
    int latency;    /* LATENCY* */
    int detect;     /* DETECT* */
    int stage;      /* STAGE_STATS + TIME_PROFILE */
    int soft_send;  /* SOFT_SEND* */
    int pipe;       /* PIPE_HEALTH* */
    int disk;       /* DISK* */
    int thermal;    /* THERMAL* */
} demo_health_log_modules;

/*
 * Init health log.
 * enable_disk: 1=on, 0=off.
 * log_dir: log dir; NULL/empty uses default /mnt/sd.
 * mods: per-module switches; NULL defaults to core/net/pipe only.
 */
int demo_health_log_init(int enable_disk, const char *log_dir,
                         const demo_health_log_modules *mods);
/* Start runtime/network monitor thread (after pipeline begins is fine). */
int demo_health_monitor_start(void);
void demo_health_monitor_stop(void);
/* Mark clean exit; stop monitor; close log. */
void demo_health_log_deinit(void);

void demo_health_log(demo_health_level level, const char *func, int line, const char *fmt, ...);
void demo_health_stage(const char *stage_name, int ok, const char *detail);

void demo_health_note_vpss_fail(int ret);
void demo_health_note_svp_ok(int target_num);
void demo_health_note_svp_fail(int ret);
void demo_health_note_venc_timeout(unsigned int total_timeout_cnt);
void demo_health_note_venc_send_fail(int ret);
void demo_health_note_rtsp_fail(int ret);
void demo_health_note_rtsp_ready(int ready);
void demo_health_note_vpss_frame(int chn, unsigned int width, unsigned int height);
void demo_health_note_npu_frame(unsigned long long input_bytes);
void demo_health_note_rtsp_stream(int chn, unsigned long long stream_bytes);

/*
 * NPU/SVP timing sample (us). Call once per SENSOR frame after release.
 */
void demo_health_note_npu_timing(unsigned long long frame_pts_us,
                                 unsigned long long hold_us,
                                 unsigned long long npu_us,
                                 unsigned long long frame_age_us);

/* RTSP path latency sample (us): now - venc pack pts. */
void demo_health_note_rtsp_latency(int chn, unsigned long long latency_us);

/* Aggregate one software pipe-stage sample (us). */
void demo_health_note_pipe_stage(demo_health_pipe_stage stage,
                                 unsigned long long elapsed_us);

/*
 * DETECT summary for one ADAS frame.
 */
void demo_health_note_detect(int targets, float conf_sum, int lane_cnt, int lane_pts);

/* TIME_PROFILE stage sample (ns). Linked from SVP adas_task via demo_main.h. */
void demo_health_note_stage_ns(const char *stage, long long elapsed_ns);

/* Soft venc_send result for och1/och2. */
void demo_health_note_soft_send(int chn, int ok, unsigned long long elapsed_us);

/* End-to-end: sensor frame pts -> soft-send moment (us). */
void demo_health_note_e2e(unsigned long long e2e_us);

/* Touch activity timestamp for stall detection. */
void demo_health_note_thread_alive(demo_health_thread_id thr);

void demo_health_tick(void);

void demo_health_get_stats(demo_health_stats *out);

#define DEMO_LOG_I(fmt, ...) \
    demo_health_log(DEMO_HEALTH_LEVEL_INFO, __FUNCTION__, __LINE__, fmt, ##__VA_ARGS__)
#define DEMO_LOG_W(fmt, ...) \
    demo_health_log(DEMO_HEALTH_LEVEL_WARN, __FUNCTION__, __LINE__, fmt, ##__VA_ARGS__)
#define DEMO_LOG_E(fmt, ...) \
    demo_health_log(DEMO_HEALTH_LEVEL_ERROR, __FUNCTION__, __LINE__, fmt, ##__VA_ARGS__)

#ifdef __cplusplus
}
#endif

#endif /* __DEMO_HEALTH_LOG_H__ */