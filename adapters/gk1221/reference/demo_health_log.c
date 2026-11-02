/*
 * demo_ai health: one dated log file per process start.
 * Runtime state, heartbeat, crash and network events share the same file.
 */

#include "demo_health_log.h"
#include "demo_thermal_monitor.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define DEMO_HEALTH_LOG_DIR            "/mnt/sd"
#define DEMO_HEALTH_LOG_DIR_FALLBACK   "/mnt/log"
#define DEMO_HEALTH_DEFAULT_PATH       DEMO_HEALTH_LOG_DIR "/demo_ai_health.log"
#define DEMO_HEALTH_MAX_BYTES          0 /* 0 = no rotate; board has large /mnt/sd */
#define DEMO_HEALTH_DISK_WARN_BYTES    (200ULL * 1024ULL * 1024ULL)
#define DEMO_HEALTH_HEARTBEAT_SEC      10
#define DEMO_HEALTH_MONITOR_PERIOD_MS  500
#define DEMO_HEALTH_STALL_SEC          15
#define DEMO_HEALTH_NET_STALL_POLLS    5
#define DEMO_HEALTH_NET_EVENT_COOLDOWN 10
#define DEMO_HEALTH_MIRROR_STDOUT      0
#define DEMO_HEALTH_MIN_LEVEL          DEMO_HEALTH_LEVEL_INFO
#define DEMO_HEALTH_VPSS_LOG_EVERY     30
#define DEMO_HEALTH_SVP_OK_EVERY       100
#define DEMO_HEALTH_PATH_MAX           256
#define DEMO_HEALTH_TS_LEN             32
#define DEMO_HEALTH_IFNAME_MAX         16
#define DEMO_HEALTH_BW_CHN_MAX         4
#define DEMO_HEALTH_LAT_WARN_MS        300
#define DEMO_HEALTH_LAT_HIGH_MS        500
#define DEMO_HEALTH_NPU_WARN_MS        150
#define DEMO_HEALTH_NPU_HIGH_MS        250
#define DEMO_HEALTH_VI_FPS_WARN        25
#define DEMO_HEALTH_VPSS_FPS_WARN      20
#define DEMO_HEALTH_NPU_FPS_WARN       5

typedef enum {
    DEMO_HEALTH_STAGE_YOLO_NPU = 0,
    DEMO_HEALTH_STAGE_YOLO_POST = 1,
    DEMO_HEALTH_STAGE_LANE_PRE = 2,
    DEMO_HEALTH_STAGE_LANE_NPU = 3,
    DEMO_HEALTH_STAGE_LANE_POST = 4,
    DEMO_HEALTH_STAGE_CNT
} demo_health_stage_id;

static const char *g_stage_names[DEMO_HEALTH_STAGE_CNT] = {
    "yolov5_npu_process",
    "yolov5_post_process",
    "lane_pre_crop",
    "lane_npu_process",
    "lane_post_process",
};

typedef struct {
    unsigned long long vpss_bytes[DEMO_HEALTH_BW_CHN_MAX];
    unsigned long long vpss_frames[DEMO_HEALTH_BW_CHN_MAX];
    unsigned long long npu_bytes;
    unsigned long long npu_frames;
    unsigned long long rtsp_bytes[DEMO_HEALTH_BW_CHN_MAX];
    unsigned long long rtsp_frames[DEMO_HEALTH_BW_CHN_MAX];
} demo_health_bandwidth_stats;

typedef struct {
    unsigned long long sum_us;
    unsigned long long max_us;
    unsigned long long samples;
    unsigned long long high_cnt;
} demo_health_lat_bucket;

typedef struct {
    demo_health_lat_bucket rtsp[DEMO_HEALTH_BW_CHN_MAX];
    demo_health_lat_bucket npu_proc;
    demo_health_lat_bucket npu_hold;
    demo_health_lat_bucket npu_age;
    demo_health_lat_bucket pipe[DEMO_HEALTH_PIPE_MAX];
    demo_health_lat_bucket e2e;
    unsigned long long npu_samples;
} demo_health_latency_stats;

typedef struct {
    unsigned long long frames;
    unsigned long long zero_frames;
    unsigned long long targets_sum;
    unsigned long long targets_max;
    double conf_sum;
    unsigned long long lane_cnt_sum;
    unsigned long long lane_pts_sum;
} demo_health_detect_stats;

typedef struct {
    demo_health_lat_bucket stage[DEMO_HEALTH_STAGE_CNT];
    demo_health_lat_bucket other;
} demo_health_stage_stats;

typedef struct {
    unsigned long long ok[DEMO_HEALTH_BW_CHN_MAX];
    unsigned long long fail[DEMO_HEALTH_BW_CHN_MAX];
    demo_health_lat_bucket send_us[DEMO_HEALTH_BW_CHN_MAX];
} demo_health_soft_send_stats;

typedef struct {
    int vi_fps;
    unsigned long long vi_lost;
    unsigned long long vi_vb_fail;
    unsigned long long vi_recv;
    unsigned long long vi_task_us;
    unsigned long long viproc_hw_us;
    int vpss_comm_free;
    int vpss_comm_wait;
    int vpss_task_wait;
    unsigned long long vpss_reset;
    unsigned long long vpss_cost_us;
    int vpss_pipe_free;
    int vpss_fps[3];
    unsigned long long vpss_send[3];
    int npu_usage;
    unsigned long long npu_finish;
    unsigned long long npu_cycle;
    int venc_fps[3];
    unsigned long long venc_full[3];
    unsigned long long venc_overload[3];
    unsigned long long venc_ring_skip[3];
    unsigned long long venc_rc_skip[3];
    int vb_free[4];
    int vb_min_free[4];
    int ddr_mhz;
} demo_health_hw_stats;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static FILE *g_fp = NULL;
static int g_enabled = 0;
static demo_health_log_modules g_mods = {
    .core = 1,
    .net = 1,
    .hw = 0,
    .bandwidth = 0,
    .latency = 0,
    .detect = 0,
    .stage = 0,
    .soft_send = 0,
    .pipe = 1,
    .disk = 0,
};

static int mod_enabled(int on)
{
    return (g_enabled && on) ? 1 : 0;
}
static int g_monitor_run = 0;
static int g_rtsp_ready = 0;
static pthread_t g_monitor_tid;
static time_t g_last_heartbeat = 0;
static time_t g_last_net_event = 0;
static time_t g_thr_alive[DEMO_HEALTH_THR_MAX];
static char g_path[DEMO_HEALTH_PATH_MAX] = DEMO_HEALTH_DEFAULT_PATH;
static demo_health_stats g_stats;
static demo_health_bandwidth_stats g_bw_stats;
static demo_health_latency_stats g_lat_stats;
static demo_health_detect_stats g_detect_stats;
static demo_health_stage_stats g_stage_stats;
static demo_health_soft_send_stats g_soft_send_stats;
static demo_health_hw_stats g_last_hw;
static int g_have_last_hw = 0;
static char g_log_dir[DEMO_HEALTH_PATH_MAX] = DEMO_HEALTH_LOG_DIR;
static stack_t g_altstack;
static int g_crash_installed = 0;

static void flush_log_unlocked(void)
{
    if (g_fp != NULL) {
        fflush(g_fp);
    }
}

static void lat_bucket_add(demo_health_lat_bucket *b, unsigned long long us,
                           unsigned long long high_us)
{
    b->sum_us += us;
    if (us > b->max_us) {
        b->max_us = us;
    }
    b->samples++;
    if (us >= high_us) {
        b->high_cnt++;
    }
}

static int lat_bucket_avg_ms(const demo_health_lat_bucket *b)
{
    if (b->samples == 0) {
        return -1;
    }
    return (int)((b->sum_us / b->samples) / 1000ULL);
}

static int lat_bucket_max_ms(const demo_health_lat_bucket *b)
{
    if (b->samples == 0) {
        return -1;
    }
    return (int)(b->max_us / 1000ULL);
}

static const char *level_name(demo_health_level level)
{
    switch (level) {
        case DEMO_HEALTH_LEVEL_WARN:
            return "WARN";
        case DEMO_HEALTH_LEVEL_ERROR:
            return "ERROR";
        case DEMO_HEALTH_LEVEL_INFO:
        default:
            return "INFO";
    }
}

static void ensure_parent_dir(const char *path)
{
    char dir[DEMO_HEALTH_PATH_MAX];
    char *slash;
    size_t n;

    n = strlen(path);
    if (n == 0 || n >= sizeof(dir)) {
        return;
    }
    memcpy(dir, path, n + 1);
    slash = strrchr(dir, '/');
    if (slash == NULL || slash == dir) {
        return;
    }
    *slash = '\0';
    mkdir(dir, 0755);
}

static void rotate_if_needed_unlocked(void)
{
    long pos;
    char bak1[DEMO_HEALTH_PATH_MAX + 4];
    char bak2[DEMO_HEALTH_PATH_MAX + 4];

    if (g_fp == NULL || DEMO_HEALTH_MAX_BYTES <= 0) {
        return;
    }
    pos = ftell(g_fp);
    if (pos < 0 || pos < DEMO_HEALTH_MAX_BYTES) {
        return;
    }

    fclose(g_fp);
    g_fp = NULL;
    snprintf(bak1, sizeof(bak1), "%s.1", g_path);
    snprintf(bak2, sizeof(bak2), "%s.2", g_path);
    (void)rename(bak1, bak2);
    (void)rename(g_path, bak1);
    g_fp = fopen(g_path, "a");
    if (g_fp != NULL) {
        setvbuf(g_fp, NULL, _IOLBF, 0);
        fprintf(g_fp, "=== demo_ai health log rotated pid=%d from_bytes=%ld max=%lld ===\n",
                (int)getpid(), pos, (long long)DEMO_HEALTH_MAX_BYTES);
        fflush(g_fp);
    }
}

static int open_log_unlocked(const char *path)
{
    ensure_parent_dir(path);
    g_fp = fopen(path, "w");
    if (g_fp == NULL) {
        return -1;
    }
    fclose(g_fp);
    g_fp = fopen(path, "a");
    if (g_fp == NULL) {
        return -1;
    }
    setvbuf(g_fp, NULL, _IOLBF, 0);
    strncpy(g_path, path, sizeof(g_path) - 1);
    g_path[sizeof(g_path) - 1] = '\0';
    {
        char *slash = strrchr(g_path, '/');
        if (slash != NULL && slash != g_path) {
            size_t n = (size_t)(slash - g_path);
            if (n >= sizeof(g_log_dir)) {
                n = sizeof(g_log_dir) - 1;
            }
            memcpy(g_log_dir, g_path, n);
            g_log_dir[n] = '\0';
        }
    }
    return 0;
}

static void put_u_digits(char *dst, unsigned v, int width)
{
    int i;
    for (i = width - 1; i >= 0; --i) {
        dst[i] = (char)('0' + (v % 10u));
        v /= 10u;
    }
}

/* "YYYY-MM-DD HH:MM:SS.mmm" = 23 chars, no snprintf (avoids -Wformat-truncation). */
static void format_ts(char *buf, size_t buflen)
{
    struct timeval tv;
    struct tm tm_now;
    time_t sec;
    unsigned y, mo, d, h, mi, s, ms;

    if (buflen < 24) {
        if (buflen > 0) {
            buf[0] = '\0';
        }
        return;
    }

    gettimeofday(&tv, NULL);
    sec = tv.tv_sec;
    localtime_r(&sec, &tm_now);

    y = (unsigned)(tm_now.tm_year + 1900) % 10000u;
    mo = ((unsigned)tm_now.tm_mon + 1u) % 100u;
    d = (unsigned)tm_now.tm_mday % 100u;
    h = (unsigned)tm_now.tm_hour % 100u;
    mi = (unsigned)tm_now.tm_min % 100u;
    s = (unsigned)tm_now.tm_sec % 100u;
    ms = ((unsigned)tv.tv_usec / 1000u) % 1000u;

    put_u_digits(buf + 0, y, 4);
    buf[4] = '-';
    put_u_digits(buf + 5, mo, 2);
    buf[7] = '-';
    put_u_digits(buf + 8, d, 2);
    buf[10] = ' ';
    put_u_digits(buf + 11, h, 2);
    buf[13] = ':';
    put_u_digits(buf + 14, mi, 2);
    buf[16] = ':';
    put_u_digits(buf + 17, s, 2);
    buf[19] = '.';
    put_u_digits(buf + 20, ms, 3);
    buf[23] = '\0';
}

static void build_run_log_path(char *path, size_t path_len, const char *dir)
{
    struct tm tm_now;
    time_t now = time(NULL);
    char date_time[32];

    if (dir == NULL || dir[0] == '\0') {
        dir = DEMO_HEALTH_LOG_DIR;
    }
    if (localtime_r(&now, &tm_now) == NULL ||
        strftime(date_time, sizeof(date_time), "%Y%m%d_%H%M%S", &tm_now) == 0) {
        snprintf(path, path_len, "%s/demo_ai_%ld_%d.log",
                 dir, (long)now, (int)getpid());
        return;
    }
    snprintf(path, path_len, "%s/demo_ai_%s_%d.log",
             dir, date_time, (int)getpid());
}

/* async-signal-safe helpers for crash path */
static void crash_write(int fd, const char *s)
{
    size_t n;

    if (fd < 0 || s == NULL) {
        return;
    }
    n = strlen(s);
    (void)write(fd, s, n);
}

static void crash_write_int(int fd, int v)
{
    char buf[16];
    int i = 0;
    int neg = 0;
    unsigned int x;

    if (v < 0) {
        neg = 1;
        x = (unsigned int)(-v);
    } else {
        x = (unsigned int)v;
    }
    if (x == 0) {
        buf[i++] = '0';
    } else {
        char tmp[16];
        int j = 0;
        while (x > 0 && j < (int)sizeof(tmp)) {
            tmp[j++] = (char)('0' + (x % 10));
            x /= 10;
        }
        while (j > 0) {
            buf[i++] = tmp[--j];
        }
    }
    if (neg) {
        char out[17];
        out[0] = '-';
        memcpy(out + 1, buf, (size_t)i);
        (void)write(fd, out, (size_t)i + 1);
    } else {
        (void)write(fd, buf, (size_t)i);
    }
}

static void crash_handler(int signo)
{
    int fd;
    const char *name = "UNKNOWN";

    switch (signo) {
        case SIGSEGV: name = "SIGSEGV"; break;
        case SIGABRT: name = "SIGABRT"; break;
        case SIGBUS:  name = "SIGBUS"; break;
        case SIGFPE:  name = "SIGFPE"; break;
        case SIGILL:  name = "SIGILL"; break;
        case SIGSYS:  name = "SIGSYS"; break;
        default: break;
    }

    fd = open(g_path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd >= 0) {
        crash_write(fd, "[CRASH] signo=");
        crash_write_int(fd, signo);
        crash_write(fd, " name=");
        crash_write(fd, name);
        crash_write(fd, " pid=");
        crash_write_int(fd, (int)getpid());
        crash_write(fd, "\n");
        close(fd);
    }

    signal(signo, SIG_DFL);
    raise(signo);
}

static void install_crash_handlers(void)
{
    struct sigaction sa;

    if (g_crash_installed) {
        return;
    }

    g_altstack.ss_sp = malloc(SIGSTKSZ);
    g_altstack.ss_size = SIGSTKSZ;
    g_altstack.ss_flags = 0;
    if (g_altstack.ss_sp != NULL) {
        sigaltstack(&g_altstack, NULL);
    }

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = crash_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_ONSTACK | SA_RESETHAND;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGABRT, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGFPE, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);
#ifdef SIGSYS
    sigaction(SIGSYS, &sa, NULL);
#endif
    g_crash_installed = 1;
}

static void write_state_file(const char *state)
{
    DEMO_LOG_I("STATE pid=%d state=%s", (int)getpid(), state);
}

static void write_alive_file(void)
{
    static time_t last_alive = 0;
    time_t now = time(NULL);
    demo_health_stats snap;

    if (last_alive != 0 && (now - last_alive) < DEMO_HEALTH_HEARTBEAT_SEC) {
        return;
    }
    last_alive = now;
    pthread_mutex_lock(&g_lock);
    snap = g_stats;
    pthread_mutex_unlock(&g_lock);

    DEMO_LOG_I("ALIVE pid=%d svp_ok=%llu vpss_fail=%llu svp_fail=%llu "
               "venc_to=%llu rtsp_fail=%llu net_if=%s carrier=%d "
               "rx=%llu tx=%llu rx_err=%llu tx_err=%llu",
               (int)getpid(), snap.svp_ok_frames, snap.vpss_fail_cnt,
               snap.svp_fail_cnt, snap.venc_timeout_cnt, snap.rtsp_fail_cnt,
               snap.net_ifname[0] ? snap.net_ifname : "none",
               snap.net_carrier, snap.net_rx_packets, snap.net_tx_packets,
               snap.net_rx_errors, snap.net_tx_errors);
}

static int read_sysfs_int(const char *path, int *out)
{
    FILE *fp;
    char buf[64];

    fp = fopen(path, "r");
    if (fp == NULL) {
        return -1;
    }
    if (fgets(buf, sizeof(buf), fp) == NULL) {
        fclose(fp);
        return -1;
    }
    fclose(fp);
    *out = atoi(buf);
    return 0;
}

static int read_operstate_up(const char *ifname, int *up)
{
    char path[128];
    FILE *fp;
    char buf[64];

    snprintf(path, sizeof(path), "/sys/class/net/%s/operstate", ifname);
    fp = fopen(path, "r");
    if (fp == NULL) {
        return -1;
    }
    if (fgets(buf, sizeof(buf), fp) == NULL) {
        fclose(fp);
        return -1;
    }
    fclose(fp);
    *up = (strncmp(buf, "up", 2) == 0) ? 1 : 0;
    return 0;
}

static int pick_net_iface(char *ifname, size_t len)
{
    static const char *candidates[] = {
        "eth0", "eth1", "end0", "enp0s3", "wlan0", "usb0", NULL
    };
    int i;
    int carrier = 0;
    char path[128];

    for (i = 0; candidates[i] != NULL; ++i) {
        snprintf(path, sizeof(path), "/sys/class/net/%s", candidates[i]);
        if (access(path, F_OK) != 0) {
            continue;
        }
        strncpy(ifname, candidates[i], len - 1);
        ifname[len - 1] = '\0';
        snprintf(path, sizeof(path), "/sys/class/net/%s/carrier", candidates[i]);
        if (read_sysfs_int(path, &carrier) == 0 && carrier == 1) {
            return 0;
        }
    }
    /* fall back to first existing iface even if down */
    for (i = 0; candidates[i] != NULL; ++i) {
        snprintf(path, sizeof(path), "/sys/class/net/%s", candidates[i]);
        if (access(path, F_OK) == 0) {
            strncpy(ifname, candidates[i], len - 1);
            ifname[len - 1] = '\0';
            return 0;
        }
    }
    return -1;
}

static int read_proc_net_dev(const char *ifname,
                            unsigned long long *rx_bytes, unsigned long long *tx_bytes,
                            unsigned long long *rx_pkt, unsigned long long *tx_pkt,
                            unsigned long long *rx_err, unsigned long long *tx_err,
                            unsigned long long *rx_drop, unsigned long long *tx_drop)
{
    FILE *fp;
    char line[512];
    char name[64];
    unsigned long long vals[16];
    int n;

    fp = fopen("/proc/net/dev", "r");
    if (fp == NULL) {
        return -1;
    }
    /* skip headers */
    (void)fgets(line, sizeof(line), fp);
    (void)fgets(line, sizeof(line), fp);

    while (fgets(line, sizeof(line), fp) != NULL) {
        char *p = line;
        while (*p == ' ') {
            p++;
        }
        n = sscanf(p,
                   "%63[^:]: %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu",
                   name,
                   &vals[0], &vals[1], &vals[2], &vals[3],
                   &vals[4], &vals[5], &vals[6], &vals[7],
                   &vals[8], &vals[9], &vals[10], &vals[11],
                   &vals[12], &vals[13], &vals[14], &vals[15]);
        if (n < 10) {
            continue;
        }
        /* strip trailing spaces in name */
        {
            size_t L = strlen(name);
            while (L > 0 && (name[L - 1] == ' ' || name[L - 1] == '\t')) {
                name[--L] = '\0';
            }
        }
        if (strcmp(name, ifname) != 0) {
            continue;
        }
        /* fields: rx_bytes rx_packets rx_errs rx_drop ... tx_bytes tx_packets tx_errs tx_drop */
        *rx_bytes = vals[0];
        *tx_bytes = vals[8];
        *rx_pkt = vals[1];
        *rx_err = vals[2];
        *rx_drop = vals[3];
        *tx_pkt = vals[9];
        *tx_err = vals[10];
        *tx_drop = vals[11];
        fclose(fp);
        return 0;
    }
    fclose(fp);
    return -1;
}

static void read_vpss_hw_stats(demo_health_hw_stats *hw)
{
    FILE *fp = fopen("/proc/umap/vpss", "r");
    char line[512];
    int section = 0;

    if (fp == NULL) {
        return;
    }
    while (fgets(line, sizeof(line), fp) != NULL) {
        if (strstr(line, "COMM WORK QUEUE STATUS") != NULL) {
            section = 1;
            continue;
        }
        if (strstr(line, "DEV WORK STATUS") != NULL) {
            section = 2;
            continue;
        }
        if (strstr(line, "PIPE PIC QUEUE") != NULL) {
            section = 3;
            continue;
        }
        if (strstr(line, "OCHN WORK STATUS") != NULL) {
            section = 4;
            continue;
        }
        if (line[0] == '-') {
            continue;
        }
        if (section == 1) {
            int busy;
            if (sscanf(line, "%d %d %d", &hw->vpss_comm_free,
                       &hw->vpss_comm_wait, &busy) == 3) {
                section = 0;
            }
        } else if (section == 2) {
            int task_free, task_busy, task_proc;
            unsigned long long max_cost;
            if (sscanf(line, "%d %d %d %d %llu %llu %llu",
                       &task_free, &hw->vpss_task_wait, &task_busy, &task_proc,
                       &hw->vpss_reset, &hw->vpss_cost_us, &max_cost) == 7) {
                section = 0;
            }
        } else if (section == 3) {
            int pipe, busy;
            if (sscanf(line, "%d %d %d", &pipe, &hw->vpss_pipe_free, &busy) == 3) {
                section = 0;
            }
        } else if (section == 4) {
            int pipe, chn, fps;
            unsigned int width, height;
            unsigned long long send;
            char pixel[32], video[32], compress[32];
            if (sscanf(line, "%d %d %llu %u %u %31s %31s %31s %d",
                       &pipe, &chn, &send, &width, &height,
                       pixel, video, compress, &fps) == 9 &&
                chn >= 0 && chn < 3) {
                hw->vpss_send[chn] = send;
                hw->vpss_fps[chn] = fps;
            }
        }
    }
    fclose(fp);
}

static void read_vi_hw_stats(demo_health_hw_stats *hw)
{
    FILE *fp = fopen("/proc/umap/vi", "r");
    char line[512];
    int section = 0;

    if (fp == NULL) {
        return;
    }
    while (fgets(line, sizeof(line), fp) != NULL) {
        if (strstr(line, "VI PIPE STATUS") != NULL) {
            section = 1;
            continue;
        }
        if (strstr(line, "VI PIPE STATISTIC") != NULL) {
            section = 2;
            continue;
        }
        if (strstr(line, "VIPROC TASK HW COST TIME") != NULL) {
            section = 3;
            continue;
        }
        if (line[0] == '-') {
            continue;
        }
        if (section == 1) {
            int pipe, fps;
            unsigned int width, height;
            unsigned long long int_cnt;
            char enabled[8];
            if (sscanf(line, "%d %7s %llu %d %llu %llu %u %u",
                       &pipe, enabled, &int_cnt, &fps, &hw->vi_lost,
                       &hw->vi_vb_fail, &width, &height) == 8) {
                hw->vi_fps = fps;
                section = 0;
            }
        } else if (section == 2) {
            int pipe;
            unsigned long long lost, cur_soft, max_soft, max_task;
            if (sscanf(line, "%d %llu %llu %llu %llu %llu %llu",
                       &pipe, &hw->vi_recv, &lost, &cur_soft, &max_soft,
                       &hw->vi_task_us, &max_task) == 7) {
                section = 0;
            }
        } else if (section == 3) {
            int pipe, viproc_id;
            unsigned long long max_hw;
            if (sscanf(line, "%d %d %llu %llu", &pipe, &viproc_id,
                       &hw->viproc_hw_us, &max_hw) == 4) {
                section = 0;
            }
        }
    }
    fclose(fp);
}

static void read_npu_hw_stats(demo_health_hw_stats *hw)
{
    FILE *fp = fopen("/proc/umap/npu", "r");
    char line[256];
    char *p;

    if (fp == NULL) {
        return;
    }
    while (fgets(line, sizeof(line), fp) != NULL) {
        p = strstr(line, "------");
        if (p == NULL) {
            continue;
        }
        p += 6;
        if (strstr(line, "usage rate") != NULL) {
            hw->npu_usage = atoi(p);
        } else if (strstr(line, "total finish num") != NULL) {
            hw->npu_finish = strtoull(p, NULL, 10);
        } else if (strstr(line, "run cyle") != NULL) {
            hw->npu_cycle = strtoull(p, NULL, 10);
        }
    }
    fclose(fp);
}

static void read_venc_hw_stats(demo_health_hw_stats *hw)
{
    FILE *fp = fopen("/proc/umap/venc", "r");
    char line[512];
    int section = 0;

    if (fp == NULL) {
        return;
    }
    while (fgets(line, sizeof(line), fp) != NULL) {
        if (strstr(line, "venc send 1") != NULL) {
            section = 1;
            continue;
        }
        if (strstr(line, "venc chnl info") != NULL) {
            section = 2;
            continue;
        }
        if (strstr(line, "venc stream state") != NULL) {
            section = 3;
            continue;
        }
        if (line[0] == '-') {
            continue;
        }
        if (section == 1) {
            int id;
            unsigned long long value[10];
            if (sscanf(line, "%d %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu",
                       &id, &value[0], &value[1], &value[2], &value[3], &value[4],
                       &value[5], &value[6], &value[7], &value[8], &value[9]) == 11 &&
                id >= 0 && id < 3) {
                hw->venc_full[id] = value[6];
            }
        } else if (section == 2) {
            int id;
            unsigned long long value[10];
            if (sscanf(line, "%d %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu",
                       &id, &value[0], &value[1], &value[2], &value[3], &value[4],
                       &value[5], &value[6], &value[7], &value[8], &value[9]) == 11 &&
                id >= 0 && id < 3) {
                hw->venc_overload[id] = value[7];
                hw->venc_ring_skip[id] = value[8];
                hw->venc_rc_skip[id] = value[9];
            }
        } else if (section == 3) {
            int id, free_cnt, busy_cnt, user_cnt, fps;
            unsigned long long user_get, user_rls, get_times, interval;
            if (sscanf(line, "%d %d %d %d %llu %llu %llu %llu %d",
                       &id, &free_cnt, &busy_cnt, &user_cnt, &user_get,
                       &user_rls, &get_times, &interval, &fps) == 9 &&
                id >= 0 && id < 3) {
                hw->venc_fps[id] = fps;
            }
        }
    }
    fclose(fp);
}

static void read_vb_hw_stats(demo_health_hw_stats *hw)
{
    FILE *fp = fopen("/proc/umap/vb", "r");
    char line[512];
    int expect_pool = 0;

    if (fp == NULL) {
        return;
    }
    while (fgets(line, sizeof(line), fp) != NULL) {
        if (strstr(line, "pool_id") != NULL && strstr(line, "blk_size") != NULL) {
            expect_pool = 1;
            continue;
        }
        if (expect_pool) {
            int pool_id, is_comm, owner, blk_cnt, free_cnt, min_free;
            unsigned long long blk_size;
            char phy[32], vir[32];
            if (sscanf(line, "%d %31s %31s %d %d %llu %d %d %d",
                       &pool_id, phy, vir, &is_comm, &owner, &blk_size,
                       &blk_cnt, &free_cnt, &min_free) == 9) {
                if (pool_id >= 0 && pool_id < 4) {
                    hw->vb_free[pool_id] = free_cnt;
                    hw->vb_min_free[pool_id] = min_free;
                }
                expect_pool = 0;
            }
        }
    }
    fclose(fp);
}

static void read_ddr_hw_stats(demo_health_hw_stats *hw)
{
    FILE *fp = fopen("/proc/umap/ddr_ost_qos", "r");
    char line[256];
    int expect_info = 0;

    if (fp == NULL) {
        return;
    }
    while (fgets(line, sizeof(line), fp) != NULL) {
        if (strstr(line, "ddr_type") != NULL && strstr(line, "ddr_freq") != NULL) {
            expect_info = 1;
            continue;
        }
        if (expect_info) {
            char type[32];
            if (sscanf(line, "%31s %d", type, &hw->ddr_mhz) == 2) {
                break;
            }
        }
    }
    fclose(fp);
}

static void check_media_hw_health(void)
{
    demo_health_hw_stats hw;

    memset(&hw, 0, sizeof(hw));
    hw.vi_fps = -1;
    hw.npu_usage = -1;
    hw.ddr_mhz = -1;
    for (int i = 0; i < 3; ++i) {
        hw.vpss_fps[i] = -1;
        hw.venc_fps[i] = -1;
    }
    for (int i = 0; i < 4; ++i) {
        hw.vb_free[i] = -1;
        hw.vb_min_free[i] = -1;
    }

    read_vi_hw_stats(&hw);
    read_vpss_hw_stats(&hw);
    read_npu_hw_stats(&hw);
    read_venc_hw_stats(&hw);
    read_vb_hw_stats(&hw);
    read_ddr_hw_stats(&hw);

    if (!mod_enabled(g_mods.hw)) {
        pthread_mutex_lock(&g_lock);
        g_last_hw = hw;
        g_have_last_hw = 1;
        pthread_mutex_unlock(&g_lock);
        return;
    }

    DEMO_LOG_I("HW_CHAIN_CORE vi_fps=%d vi_lost=%llu vi_vb_fail=%llu "
               "vi_recv=%llu vi_task_us=%llu viproc_us=%llu "
               "vpss_fps=%d/%d/%d vpss_send=%llu/%llu/%llu "
               "vpss_free=%d vpss_wait=%d reset=%llu cost_us=%llu pipe_free=%d "
               "npu_pct=%d npu_finish=%llu npu_cycle=%llu",
               hw.vi_fps, hw.vi_lost, hw.vi_vb_fail,
               hw.vi_recv, hw.vi_task_us, hw.viproc_hw_us,
               hw.vpss_fps[0], hw.vpss_fps[1], hw.vpss_fps[2],
               hw.vpss_send[0], hw.vpss_send[1], hw.vpss_send[2],
               hw.vpss_comm_free,
               hw.vpss_comm_wait + hw.vpss_task_wait, hw.vpss_reset,
               hw.vpss_cost_us, hw.vpss_pipe_free,
               hw.npu_usage, hw.npu_finish, hw.npu_cycle);
    DEMO_LOG_I("HW_CHAIN_VENC fps=%d/%d/%d full=%llu/%llu/%llu "
               "overload=%llu/%llu/%llu skip=%llu/%llu/%llu",
               hw.venc_fps[0], hw.venc_fps[1], hw.venc_fps[2],
               hw.venc_full[0], hw.venc_full[1], hw.venc_full[2],
               hw.venc_overload[0], hw.venc_overload[1], hw.venc_overload[2],
               hw.venc_ring_skip[0] + hw.venc_rc_skip[0],
               hw.venc_ring_skip[1] + hw.venc_rc_skip[1],
               hw.venc_ring_skip[2] + hw.venc_rc_skip[2]);
    DEMO_LOG_I("HW_CHAIN_MEM vb_free=%d/%d/%d/%d vb_min=%d/%d/%d/%d ddr_mhz=%d",
               hw.vb_free[0], hw.vb_free[1], hw.vb_free[2], hw.vb_free[3],
               hw.vb_min_free[0], hw.vb_min_free[1],
               hw.vb_min_free[2], hw.vb_min_free[3], hw.ddr_mhz);

    pthread_mutex_lock(&g_lock);
    g_last_hw = hw;
    g_have_last_hw = 1;
    pthread_mutex_unlock(&g_lock);
}

static void dump_net_event(const char *ifname, const char *reason,
                           int carrier, int oper_up,
                           unsigned long long rx, unsigned long long tx,
                           unsigned long long rx_err, unsigned long long tx_err,
                           unsigned long long rx_drop, unsigned long long tx_drop,
                           unsigned long long drx, unsigned long long dtx)
{
    char if_safe[DEMO_HEALTH_IFNAME_MAX];
    time_t now = time(NULL);

    pthread_mutex_lock(&g_lock);
    if (g_last_net_event != 0 && (now - g_last_net_event) < DEMO_HEALTH_NET_EVENT_COOLDOWN) {
        pthread_mutex_unlock(&g_lock);
        return;
    }
    g_last_net_event = now;
    g_stats.net_event_cnt++;
    pthread_mutex_unlock(&g_lock);

    strncpy(if_safe, ifname ? ifname : "eth0", sizeof(if_safe) - 1);
    if_safe[sizeof(if_safe) - 1] = '\0';

    DEMO_LOG_W("NET EVENT reason=%s if=%s carrier=%d oper_up=%d "
               "rx=%llu tx=%llu rx_err=%llu tx_err=%llu "
               "rx_drop=%llu tx_drop=%llu d_rx=%llu d_tx=%llu rtsp_ready=%d",
               reason, if_safe, carrier, oper_up, rx, tx, rx_err, tx_err,
               rx_drop, tx_drop, drx, dtx, g_rtsp_ready);
}

static void check_network_health(void)
{
    char ifname[32];
    int carrier = -1;
    int oper_up = -1;
    char path[128];
    unsigned long long rx = 0, tx = 0, rx_err = 0, tx_err = 0, rx_drop = 0, tx_drop = 0;
    unsigned long long rx_bytes = 0, tx_bytes = 0;
    static unsigned long long prev_rx = 0, prev_tx = 0, prev_rx_err = 0, prev_tx_err = 0;
    static int prev_carrier = -1;
    static int have_prev = 0;
    static int zero_traffic_polls = 0;
    unsigned long long drx = 0, dtx = 0, derr = 0;
    int rtsp_ready;

    if (pick_net_iface(ifname, sizeof(ifname)) != 0) {
        DEMO_LOG_W("NET no iface found under /sys/class/net");
        return;
    }

    snprintf(path, sizeof(path), "/sys/class/net/%s/carrier", ifname);
    if (read_sysfs_int(path, &carrier) != 0) {
        carrier = -1;
    }
    (void)read_operstate_up(ifname, &oper_up);
    (void)read_proc_net_dev(ifname, &rx_bytes, &tx_bytes,
                            &rx, &tx, &rx_err, &tx_err, &rx_drop, &tx_drop);

    pthread_mutex_lock(&g_lock);
    strncpy(g_stats.net_ifname, ifname, sizeof(g_stats.net_ifname) - 1);
    g_stats.net_carrier = carrier;
    g_stats.net_rx_bytes = rx_bytes;
    g_stats.net_tx_bytes = tx_bytes;
    g_stats.net_rx_packets = rx;
    g_stats.net_tx_packets = tx;
    g_stats.net_rx_errors = rx_err;
    g_stats.net_tx_errors = tx_err;
    g_stats.net_rx_dropped = rx_drop;
    g_stats.net_tx_dropped = tx_drop;
    rtsp_ready = g_rtsp_ready;
    pthread_mutex_unlock(&g_lock);

    if (have_prev) {
        drx = (rx >= prev_rx) ? (rx - prev_rx) : 0;
        dtx = (tx >= prev_tx) ? (tx - prev_tx) : 0;
        if (rx_err >= prev_rx_err) {
            derr += rx_err - prev_rx_err;
        }
        if (tx_err >= prev_tx_err) {
            derr += tx_err - prev_tx_err;
        }

        if (prev_carrier == 1 && carrier == 0) {
            pthread_mutex_lock(&g_lock);
            g_stats.net_link_down_cnt++;
            pthread_mutex_unlock(&g_lock);
            DEMO_LOG_E("NET LINK_DOWN if=%s operstate_up=%d", ifname, oper_up);
            dump_net_event(ifname, "link_down", carrier, oper_up,
                           rx, tx, rx_err, tx_err, rx_drop, tx_drop, drx, dtx);
        } else if (prev_carrier == 0 && carrier == 1) {
            DEMO_LOG_W("NET LINK_UP if=%s (recovered)", ifname);
            dump_net_event(ifname, "link_recover", carrier, oper_up,
                           rx, tx, rx_err, tx_err, rx_drop, tx_drop, drx, dtx);
        }

        if (derr > 0) {
            DEMO_LOG_W("NET ERRORS if=%s d_err=%llu rx_drop=%llu tx_drop=%llu",
                       ifname, derr, rx_drop, tx_drop);
            dump_net_event(ifname, "net_errors", carrier, oper_up,
                           rx, tx, rx_err, tx_err, rx_drop, tx_drop, drx, dtx);
        }

        if (rtsp_ready && carrier == 1) {
            if (drx == 0 && dtx == 0) {
                zero_traffic_polls++;
                if (zero_traffic_polls >= DEMO_HEALTH_NET_STALL_POLLS) {
                    pthread_mutex_lock(&g_lock);
                    g_stats.net_stall_cnt++;
                    pthread_mutex_unlock(&g_lock);
                    DEMO_LOG_W("NET STALL if=%s no rx/tx for %d polls (possible disconnect/flap)",
                               ifname, zero_traffic_polls);
                    dump_net_event(ifname, "traffic_stall", carrier, oper_up,
                                   rx, tx, rx_err, tx_err, rx_drop, tx_drop, drx, dtx);
                    zero_traffic_polls = 0;
                }
            } else {
                zero_traffic_polls = 0;
            }
        }
    }

    prev_rx = rx;
    prev_tx = tx;
    prev_rx_err = rx_err;
    prev_tx_err = tx_err;
    prev_carrier = carrier;
    have_prev = 1;
}

static void check_thread_stalls(void)
{
    time_t now = time(NULL);
    time_t svp_t, venc_t;
    int rtsp_ready;
    unsigned long long svp_frames;

    pthread_mutex_lock(&g_lock);
    svp_t = g_thr_alive[DEMO_HEALTH_THR_SVP];
    venc_t = g_thr_alive[DEMO_HEALTH_THR_VENC];
    rtsp_ready = g_rtsp_ready;
    svp_frames = g_stats.svp_ok_frames;
    pthread_mutex_unlock(&g_lock);

    if (svp_frames > 0 && svp_t > 0 && (now - svp_t) > DEMO_HEALTH_STALL_SEC) {
        DEMO_LOG_E("RUNTIME STALL svp silent for %ld sec", (long)(now - svp_t));
    }
    if (rtsp_ready && venc_t > 0 && (now - venc_t) > DEMO_HEALTH_STALL_SEC) {
        DEMO_LOG_E("RUNTIME STALL venc silent for %ld sec", (long)(now - venc_t));
    }
}

static unsigned long long bandwidth_kbps(unsigned long long current,
                                         unsigned long long previous,
                                         unsigned long long elapsed_ms)
{
    unsigned long long delta = current >= previous ? current - previous : 0;

    return elapsed_ms > 0 ? delta * 8ULL / elapsed_ms : 0;
}

static unsigned long long bandwidth_fps_x100(unsigned long long current,
                                             unsigned long long previous,
                                             unsigned long long elapsed_ms)
{
    unsigned long long delta = current >= previous ? current - previous : 0;

    return elapsed_ms > 0 ? delta * 100000ULL / elapsed_ms : 0;
}

static void log_bandwidth(void)
{
    static demo_health_bandwidth_stats previous;
    static demo_health_stats previous_net;
    static demo_health_hw_stats previous_hw;
    static struct timespec previous_ts;
    static int have_previous = 0;
    static int have_previous_hw = 0;
    demo_health_bandwidth_stats current;
    demo_health_stats current_net;
    demo_health_hw_stats current_hw;
    struct timespec now_ts;
    long long elapsed_ns;
    unsigned long long elapsed_ms;
    unsigned long long fps0, fps1, fps2;
    unsigned long long vi_recv_delta = 0;
    unsigned long long vi_lost_delta = 0;
    unsigned long long vi_vb_fail_delta = 0;
    int have_hw = 0;
    const char *vi_health = "NA";

    clock_gettime(CLOCK_MONOTONIC, &now_ts);
    pthread_mutex_lock(&g_lock);
    current = g_bw_stats;
    current_net = g_stats;
    have_hw = g_have_last_hw;
    if (have_hw) {
        current_hw = g_last_hw;
    }
    pthread_mutex_unlock(&g_lock);

    if (!have_previous) {
        previous = current;
        previous_net = current_net;
        previous_ts = now_ts;
        if (have_hw) {
            previous_hw = current_hw;
            have_previous_hw = 1;
        }
        have_previous = 1;
        return;
    }

    elapsed_ns = (long long)(now_ts.tv_sec - previous_ts.tv_sec) * 1000000000LL +
                 (long long)(now_ts.tv_nsec - previous_ts.tv_nsec);
    if (elapsed_ns <= 0) {
        return;
    }
    elapsed_ms = (unsigned long long)elapsed_ns / 1000000ULL;
    if (elapsed_ms == 0) {
        return;
    }

    if (have_hw && have_previous_hw) {
        if (current_hw.vi_recv >= previous_hw.vi_recv) {
            vi_recv_delta = current_hw.vi_recv - previous_hw.vi_recv;
        }
        if (current_hw.vi_lost >= previous_hw.vi_lost) {
            vi_lost_delta = current_hw.vi_lost - previous_hw.vi_lost;
        }
        if (current_hw.vi_vb_fail >= previous_hw.vi_vb_fail) {
            vi_vb_fail_delta = current_hw.vi_vb_fail - previous_hw.vi_vb_fail;
        }
        if (vi_lost_delta > 0 || vi_vb_fail_delta > 0) {
            vi_health = "WARN";
        } else if (current_hw.vi_fps >= 0 && current_hw.vi_fps < DEMO_HEALTH_VI_FPS_WARN) {
            vi_health = "WARN";
        } else if (current_hw.vi_fps >= DEMO_HEALTH_VI_FPS_WARN || vi_recv_delta > 0) {
            vi_health = "OK";
        }
        DEMO_LOG_I("BANDWIDTH VI period_ms=%llu fps=%d task_us=%llu "
                   "recv_delta=%llu lost_delta=%llu vb_fail_delta=%llu "
                   "viproc_us=%llu health=%s",
                   elapsed_ms, current_hw.vi_fps, current_hw.vi_task_us,
                   vi_recv_delta, vi_lost_delta, vi_vb_fail_delta,
                   current_hw.viproc_hw_us, vi_health);
        /* och0 is typically full-sensor; use it as VI→VPSS payload estimate. */
        DEMO_LOG_I("BANDWIDTH VI_EST period_ms=%llu och0_kbps=%llu och0_fps=%llu.%02llu",
                   elapsed_ms,
                   bandwidth_kbps(current.vpss_bytes[0], previous.vpss_bytes[0], elapsed_ms),
                   bandwidth_fps_x100(current.vpss_frames[0], previous.vpss_frames[0],
                                      elapsed_ms) / 100ULL,
                   bandwidth_fps_x100(current.vpss_frames[0], previous.vpss_frames[0],
                                      elapsed_ms) % 100ULL);
    } else {
        DEMO_LOG_I("BANDWIDTH VI period_ms=%llu health=NA (hw stats not ready)",
                   elapsed_ms);
    }

    fps0 = bandwidth_fps_x100(current.vpss_frames[0], previous.vpss_frames[0], elapsed_ms);
    fps1 = bandwidth_fps_x100(current.vpss_frames[1], previous.vpss_frames[1], elapsed_ms);
    fps2 = bandwidth_fps_x100(current.vpss_frames[2], previous.vpss_frames[2], elapsed_ms);
    DEMO_LOG_I("BANDWIDTH VPSS period_ms=%llu "
               "och0_kbps=%llu och0_fps=%llu.%02llu "
               "och1_kbps=%llu och1_fps=%llu.%02llu "
               "och2_kbps=%llu och2_fps=%llu.%02llu",
               elapsed_ms,
               bandwidth_kbps(current.vpss_bytes[0], previous.vpss_bytes[0], elapsed_ms),
               fps0 / 100ULL, fps0 % 100ULL,
               bandwidth_kbps(current.vpss_bytes[1], previous.vpss_bytes[1], elapsed_ms),
               fps1 / 100ULL, fps1 % 100ULL,
               bandwidth_kbps(current.vpss_bytes[2], previous.vpss_bytes[2], elapsed_ms),
               fps2 / 100ULL, fps2 % 100ULL);

    fps0 = bandwidth_fps_x100(current.npu_frames, previous.npu_frames, elapsed_ms);
    DEMO_LOG_I("BANDWIDTH NPU period_ms=%llu input_kbps=%llu fps=%llu.%02llu",
               elapsed_ms,
               bandwidth_kbps(current.npu_bytes, previous.npu_bytes, elapsed_ms),
               fps0 / 100ULL, fps0 % 100ULL);

    fps0 = bandwidth_fps_x100(current.rtsp_frames[0], previous.rtsp_frames[0], elapsed_ms);
    fps1 = bandwidth_fps_x100(current.rtsp_frames[1], previous.rtsp_frames[1], elapsed_ms);
    fps2 = bandwidth_fps_x100(current.rtsp_frames[2], previous.rtsp_frames[2], elapsed_ms);
    DEMO_LOG_I("BANDWIDTH RTSP period_ms=%llu "
               "chn0_kbps=%llu chn0_fps=%llu.%02llu "
               "chn1_kbps=%llu chn1_fps=%llu.%02llu "
               "chn2_kbps=%llu chn2_fps=%llu.%02llu",
               elapsed_ms,
               bandwidth_kbps(current.rtsp_bytes[0], previous.rtsp_bytes[0], elapsed_ms),
               fps0 / 100ULL, fps0 % 100ULL,
               bandwidth_kbps(current.rtsp_bytes[1], previous.rtsp_bytes[1], elapsed_ms),
               fps1 / 100ULL, fps1 % 100ULL,
               bandwidth_kbps(current.rtsp_bytes[2], previous.rtsp_bytes[2], elapsed_ms),
               fps2 / 100ULL, fps2 % 100ULL);

    DEMO_LOG_I("BANDWIDTH NET period_ms=%llu if=%s carrier=%d "
               "rx_kbps=%llu tx_kbps=%llu rx_err=%llu tx_err=%llu "
               "rx_drop=%llu tx_drop=%llu",
               elapsed_ms,
               current_net.net_ifname[0] ? current_net.net_ifname : "none",
               current_net.net_carrier,
               bandwidth_kbps(current_net.net_rx_bytes, previous_net.net_rx_bytes,
                              elapsed_ms),
               bandwidth_kbps(current_net.net_tx_bytes, previous_net.net_tx_bytes,
                              elapsed_ms),
               current_net.net_rx_errors, current_net.net_tx_errors,
               current_net.net_rx_dropped, current_net.net_tx_dropped);

    previous = current;
    previous_net = current_net;
    previous_ts = now_ts;
    if (have_hw) {
        previous_hw = current_hw;
        have_previous_hw = 1;
    }
}

static void log_latency_stats(void)
{
    demo_health_latency_stats snap;
    int rtsp_avg[3], rtsp_max[3];
    unsigned long long rtsp_high[3];
    int npu_proc_avg_ms = -1;
    int npu_proc_max_ms = -1;
    int npu_hold_avg_ms = -1;
    int npu_frame_age_avg_ms = -1;
    int pipe_avg[DEMO_HEALTH_PIPE_MAX];
    int pipe_max[DEMO_HEALTH_PIPE_MAX];
    int i;
    const char *status = "OK";

    pthread_mutex_lock(&g_lock);
    snap = g_lat_stats;
    memset(&g_lat_stats, 0, sizeof(g_lat_stats));
    pthread_mutex_unlock(&g_lock);

    for (i = 0; i < 3; ++i) {
        rtsp_avg[i] = lat_bucket_avg_ms(&snap.rtsp[i]);
        rtsp_max[i] = lat_bucket_max_ms(&snap.rtsp[i]);
        rtsp_high[i] = snap.rtsp[i].high_cnt;
        if (rtsp_max[i] >= DEMO_HEALTH_LAT_HIGH_MS ||
            (rtsp_avg[i] >= 0 && rtsp_avg[i] >= DEMO_HEALTH_LAT_HIGH_MS)) {
            status = "HIGH";
        } else if (strcmp(status, "OK") == 0 &&
                   ((rtsp_avg[i] >= 0 && rtsp_avg[i] >= DEMO_HEALTH_LAT_WARN_MS) ||
                    rtsp_max[i] >= DEMO_HEALTH_LAT_WARN_MS)) {
            status = "WARN";
        }
    }

    if (snap.npu_samples > 0) {
        npu_proc_avg_ms = lat_bucket_avg_ms(&snap.npu_proc);
        npu_proc_max_ms = lat_bucket_max_ms(&snap.npu_proc);
        npu_hold_avg_ms = lat_bucket_avg_ms(&snap.npu_hold);
        npu_frame_age_avg_ms = lat_bucket_avg_ms(&snap.npu_age);
        if (npu_proc_max_ms >= DEMO_HEALTH_NPU_HIGH_MS ||
            npu_proc_avg_ms >= DEMO_HEALTH_NPU_HIGH_MS) {
            status = "HIGH";
        } else if (strcmp(status, "OK") == 0 &&
                   (npu_proc_avg_ms >= DEMO_HEALTH_NPU_WARN_MS ||
                    npu_proc_max_ms >= DEMO_HEALTH_NPU_WARN_MS)) {
            status = "WARN";
        }
    }

    for (i = 0; i < DEMO_HEALTH_PIPE_MAX; ++i) {
        pipe_avg[i] = lat_bucket_avg_ms(&snap.pipe[i]);
        pipe_max[i] = lat_bucket_max_ms(&snap.pipe[i]);
    }

    DEMO_LOG_I("LATENCY status=%s warn_ms=%d high_ms=%d "
               "rtsp_avg_ms=%d/%d/%d rtsp_max_ms=%d/%d/%d "
               "rtsp_high_cnt=%llu/%llu/%llu "
               "npu_proc_avg_ms=%d npu_proc_max_ms=%d "
               "npu_hold_avg_ms=%d npu_frame_age_avg_ms=%d",
               status, DEMO_HEALTH_LAT_WARN_MS, DEMO_HEALTH_LAT_HIGH_MS,
               rtsp_avg[0], rtsp_avg[1], rtsp_avg[2],
               rtsp_max[0], rtsp_max[1], rtsp_max[2],
               rtsp_high[0], rtsp_high[1], rtsp_high[2],
               npu_proc_avg_ms, npu_proc_max_ms,
               npu_hold_avg_ms, npu_frame_age_avg_ms);

    DEMO_LOG_I("LATENCY_PIPE acquire_avg/max_ms=%d/%d npu_avg/max_ms=%d/%d "
               "venc_send_avg/max_ms=%d/%d hold_avg/max_ms=%d/%d samples=%llu",
               pipe_avg[DEMO_HEALTH_PIPE_VPSS_ACQUIRE],
               pipe_max[DEMO_HEALTH_PIPE_VPSS_ACQUIRE],
               pipe_avg[DEMO_HEALTH_PIPE_NPU],
               pipe_max[DEMO_HEALTH_PIPE_NPU],
               pipe_avg[DEMO_HEALTH_PIPE_VENC_SEND],
               pipe_max[DEMO_HEALTH_PIPE_VENC_SEND],
               pipe_avg[DEMO_HEALTH_PIPE_FRAME_HOLD],
               pipe_max[DEMO_HEALTH_PIPE_FRAME_HOLD],
               snap.npu_samples);

    DEMO_LOG_I("LATENCY_E2E avg/max_ms=%d/%d samples=%llu high_cnt=%llu "
               "(sensor_pts_to_soft_send)",
               lat_bucket_avg_ms(&snap.e2e), lat_bucket_max_ms(&snap.e2e),
               snap.e2e.samples, snap.e2e.high_cnt);
}

static void log_detect_stats(void)
{
    demo_health_detect_stats snap;
    int avg_targets = -1;
    int avg_conf_x100 = -1;
    int avg_lane = -1;
    int avg_lane_pts = -1;
    const char *status = "OK";

    pthread_mutex_lock(&g_lock);
    snap = g_detect_stats;
    memset(&g_detect_stats, 0, sizeof(g_detect_stats));
    pthread_mutex_unlock(&g_lock);

    if (snap.frames == 0) {
        DEMO_LOG_I("DETECT status=NA frames=0 (no ADAS samples this period)");
        return;
    }
    avg_targets = (int)(snap.targets_sum / snap.frames);
    if (snap.targets_sum > 0) {
        avg_conf_x100 = (int)((snap.conf_sum * 100.0) / (double)snap.targets_sum);
    }
    avg_lane = (int)(snap.lane_cnt_sum / snap.frames);
    avg_lane_pts = (int)(snap.lane_pts_sum / snap.frames);
    if (snap.zero_frames * 2 > snap.frames) {
        status = "WARN"; /* >50% empty detections */
    }

    DEMO_LOG_I("DETECT status=%s frames=%llu zero_frames=%llu "
               "targets_avg=%d targets_max=%llu conf_avg_x100=%d "
               "lane_avg=%d lane_pts_avg=%d",
               status, snap.frames, snap.zero_frames,
               avg_targets, snap.targets_max, avg_conf_x100,
               avg_lane, avg_lane_pts);
}

static void log_stage_stats(void)
{
    demo_health_stage_stats snap;
    int i;
    int avg_ms[DEMO_HEALTH_STAGE_CNT];
    int max_ms[DEMO_HEALTH_STAGE_CNT];
    unsigned long long samples[DEMO_HEALTH_STAGE_CNT];

    pthread_mutex_lock(&g_lock);
    snap = g_stage_stats;
    memset(&g_stage_stats, 0, sizeof(g_stage_stats));
    pthread_mutex_unlock(&g_lock);

    for (i = 0; i < DEMO_HEALTH_STAGE_CNT; ++i) {
        avg_ms[i] = lat_bucket_avg_ms(&snap.stage[i]);
        max_ms[i] = lat_bucket_max_ms(&snap.stage[i]);
        samples[i] = snap.stage[i].samples;
    }

    DEMO_LOG_I("STAGE_STATS yolo_npu_avg/max_ms=%d/%d n=%llu "
               "yolo_post_avg/max_ms=%d/%d n=%llu "
               "lane_pre_avg/max_ms=%d/%d n=%llu "
               "lane_npu_avg/max_ms=%d/%d n=%llu "
               "lane_post_avg/max_ms=%d/%d n=%llu "
               "other_avg/max_ms=%d/%d n=%llu",
               avg_ms[DEMO_HEALTH_STAGE_YOLO_NPU], max_ms[DEMO_HEALTH_STAGE_YOLO_NPU],
               samples[DEMO_HEALTH_STAGE_YOLO_NPU],
               avg_ms[DEMO_HEALTH_STAGE_YOLO_POST], max_ms[DEMO_HEALTH_STAGE_YOLO_POST],
               samples[DEMO_HEALTH_STAGE_YOLO_POST],
               avg_ms[DEMO_HEALTH_STAGE_LANE_PRE], max_ms[DEMO_HEALTH_STAGE_LANE_PRE],
               samples[DEMO_HEALTH_STAGE_LANE_PRE],
               avg_ms[DEMO_HEALTH_STAGE_LANE_NPU], max_ms[DEMO_HEALTH_STAGE_LANE_NPU],
               samples[DEMO_HEALTH_STAGE_LANE_NPU],
               avg_ms[DEMO_HEALTH_STAGE_LANE_POST], max_ms[DEMO_HEALTH_STAGE_LANE_POST],
               samples[DEMO_HEALTH_STAGE_LANE_POST],
               lat_bucket_avg_ms(&snap.other), lat_bucket_max_ms(&snap.other),
               snap.other.samples);
}

static void log_soft_send_stats(void)
{
    demo_health_soft_send_stats snap;
    unsigned long long ok1, fail1, ok2, fail2;

    pthread_mutex_lock(&g_lock);
    snap = g_soft_send_stats;
    memset(&g_soft_send_stats, 0, sizeof(g_soft_send_stats));
    pthread_mutex_unlock(&g_lock);

    ok1 = snap.ok[1];
    fail1 = snap.fail[1];
    ok2 = snap.ok[2];
    fail2 = snap.fail[2];

    DEMO_LOG_I("SOFT_SEND och1_ok/fail=%llu/%llu send_avg/max_ms=%d/%d "
               "och2_ok/fail=%llu/%llu send_avg/max_ms=%d/%d",
               ok1, fail1,
               lat_bucket_avg_ms(&snap.send_us[1]), lat_bucket_max_ms(&snap.send_us[1]),
               ok2, fail2,
               lat_bucket_avg_ms(&snap.send_us[2]), lat_bucket_max_ms(&snap.send_us[2]));
}

static void log_disk_health(void)
{
    struct statvfs fs;
    unsigned long long free_bytes = 0;
    unsigned long long total_bytes = 0;
    long log_bytes = -1;
    const char *status = "OK";

    if (statvfs(g_log_dir, &fs) == 0) {
        free_bytes = (unsigned long long)fs.f_bavail * (unsigned long long)fs.f_frsize;
        total_bytes = (unsigned long long)fs.f_blocks * (unsigned long long)fs.f_frsize;
    }
    pthread_mutex_lock(&g_lock);
    if (g_fp != NULL) {
        log_bytes = ftell(g_fp);
    }
    pthread_mutex_unlock(&g_lock);

    if (free_bytes > 0 && free_bytes < DEMO_HEALTH_DISK_WARN_BYTES) {
        status = "WARN";
    }
    if (free_bytes == 0 && total_bytes == 0) {
        status = "NA";
    }

    DEMO_LOG_I("DISK status=%s dir=%s free_mb=%llu total_mb=%llu "
               "log_bytes=%ld rotate=off",
               status, g_log_dir,
               free_bytes / (1024ULL * 1024ULL),
               total_bytes / (1024ULL * 1024ULL),
               log_bytes);
    if (strcmp(status, "WARN") == 0) {
        DEMO_LOG_W("DISK low free space dir=%s free_mb=%llu",
                   g_log_dir, free_bytes / (1024ULL * 1024ULL));
    }

    pthread_mutex_lock(&g_lock);
    flush_log_unlocked();
    pthread_mutex_unlock(&g_lock);
}

static void log_pipe_health(void)
{
    demo_health_hw_stats hw;
    demo_health_bandwidth_stats bw;
    demo_health_stats st;
    static demo_health_bandwidth_stats prev_bw;
    static int have_prev_bw = 0;
    unsigned long long npu_fps_x100 = 0;
    unsigned long long vpss1_fps_x100 = 0;
    const char *vi_h = "NA";
    const char *vpss_h = "NA";
    const char *npu_h = "NA";
    const char *rtsp_h = "NA";
    const char *net_h = "NA";
    const char *status = "OK";
    int have_hw = 0;

    pthread_mutex_lock(&g_lock);
    have_hw = g_have_last_hw;
    if (have_hw) {
        hw = g_last_hw;
    }
    bw = g_bw_stats;
    st = g_stats;
    pthread_mutex_unlock(&g_lock);

    if (have_prev_bw) {
        /* Rough 2s period fps from cumulative counters (monitor period). */
        npu_fps_x100 = (bw.npu_frames >= prev_bw.npu_frames) ?
            (bw.npu_frames - prev_bw.npu_frames) * 100ULL / 2ULL : 0;
        vpss1_fps_x100 = (bw.vpss_frames[1] >= prev_bw.vpss_frames[1]) ?
            (bw.vpss_frames[1] - prev_bw.vpss_frames[1]) * 100ULL / 2ULL : 0;
    }
    prev_bw = bw;
    have_prev_bw = 1;

    if (have_hw) {
        if (hw.vi_lost > 0 || hw.vi_vb_fail > 0 ||
            (hw.vi_fps >= 0 && hw.vi_fps < DEMO_HEALTH_VI_FPS_WARN)) {
            vi_h = "WARN";
            status = "WARN";
        } else if (hw.vi_fps >= DEMO_HEALTH_VI_FPS_WARN || hw.vi_recv > 0) {
            vi_h = "OK";
        }

        if (hw.vpss_comm_wait + hw.vpss_task_wait > 0 || hw.vpss_reset > 0) {
            vpss_h = "WARN";
            status = "WARN";
        } else if (hw.vpss_fps[1] >= 0 && hw.vpss_fps[1] < DEMO_HEALTH_VPSS_FPS_WARN) {
            vpss_h = "WARN";
            status = "WARN";
        } else if (hw.vpss_fps[0] > 0 || hw.vpss_fps[1] > 0 || hw.vpss_fps[2] > 0) {
            vpss_h = "OK";
        }
    }

    if (st.svp_fail_cnt > 0 && st.svp_ok_frames == 0) {
        npu_h = "HIGH";
        status = "HIGH";
    } else if (have_prev_bw && npu_fps_x100 < (unsigned long long)DEMO_HEALTH_NPU_FPS_WARN * 100ULL &&
               st.svp_ok_frames > 0) {
        npu_h = "WARN";
        if (strcmp(status, "OK") == 0) {
            status = "WARN";
        }
    } else if (st.svp_ok_frames > 0 || npu_fps_x100 > 0) {
        npu_h = "OK";
    }

    if (st.rtsp_fail_cnt > 0 || st.venc_timeout_cnt > 0 || st.venc_send_fail_cnt > 0) {
        rtsp_h = "WARN";
        if (strcmp(status, "OK") == 0) {
            status = "WARN";
        }
    } else if (g_rtsp_ready) {
        rtsp_h = "OK";
    }

    if (st.net_carrier == 0 || st.net_link_down_cnt > 0 || st.net_stall_cnt > 0) {
        net_h = "HIGH";
        status = "HIGH";
    } else if (st.net_carrier == 1) {
        net_h = "OK";
    }

    DEMO_LOG_I("PIPE_HEALTH status=%s vi=%s vpss=%s npu=%s rtsp=%s net=%s "
               "vpss1_fps~=%llu.%02llu npu_fps~=%llu.%02llu "
               "svp_ok=%llu vpss_fail=%llu svp_fail=%llu venc_send_fail=%llu "
               "rtsp_fail=%llu",
               status, vi_h, vpss_h, npu_h, rtsp_h, net_h,
               vpss1_fps_x100 / 100ULL, vpss1_fps_x100 % 100ULL,
               npu_fps_x100 / 100ULL, npu_fps_x100 % 100ULL,
               st.svp_ok_frames, st.vpss_fail_cnt, st.svp_fail_cnt,
               st.venc_send_fail_cnt, st.rtsp_fail_cnt);

    pthread_mutex_lock(&g_lock);
    flush_log_unlocked();
    pthread_mutex_unlock(&g_lock);
}

static void check_thermal_health(void)
{
    static int thermal_inited = 0;
    demo_thermal_state state;

    if (!thermal_inited) {
        demo_thermal_monitor_init();
        thermal_inited = 1;
    }
    (void)demo_thermal_monitor_sample(&state);
    demo_thermal_monitor_log(&state);
    /* 持续超温保护：超温达时长停模型算法，降温自动恢复 */
    demo_thermal_npu_protect_update(&state);
}

static void *monitor_thread(void *arg)
{
    (void)arg;
    if (mod_enabled(g_mods.core)) {
        DEMO_LOG_I("monitor thread started period=%dms", DEMO_HEALTH_MONITOR_PERIOD_MS);
    }

    while (g_monitor_run) {
        demo_health_note_thread_alive(DEMO_HEALTH_THR_MAIN);
        if (mod_enabled(g_mods.net)) {
            check_network_health();
        }
        if (mod_enabled(g_mods.thermal)) {
            check_thermal_health();
        }
        if (mod_enabled(g_mods.hw) || mod_enabled(g_mods.pipe)) {
            check_media_hw_health();
        }
        if (mod_enabled(g_mods.core)) {
            check_thread_stalls();
        }
        if (mod_enabled(g_mods.bandwidth)) {
            log_bandwidth();
        }
        if (mod_enabled(g_mods.latency)) {
            log_latency_stats();
        }
        if (mod_enabled(g_mods.detect)) {
            log_detect_stats();
        }
        if (mod_enabled(g_mods.stage)) {
            log_stage_stats();
        }
        if (mod_enabled(g_mods.soft_send)) {
            log_soft_send_stats();
        }
        if (mod_enabled(g_mods.pipe)) {
            log_pipe_health();
        }
        if (mod_enabled(g_mods.disk)) {
            log_disk_health();
        }
        if (mod_enabled(g_mods.core)) {
            write_alive_file();
            demo_health_tick();
        }
        usleep(DEMO_HEALTH_MONITOR_PERIOD_MS * 1000);
    }

    if (mod_enabled(g_mods.core)) {
        DEMO_LOG_I("monitor thread exit");
    }
    return NULL;
}

int demo_health_log_init(int enable_disk, const char *log_dir,
                         const demo_health_log_modules *mods)
{
    int i;
    char run_path[DEMO_HEALTH_PATH_MAX];
    const char *dir = log_dir;

    pthread_mutex_lock(&g_lock);
    memset(&g_stats, 0, sizeof(g_stats));
    memset(&g_bw_stats, 0, sizeof(g_bw_stats));
    memset(&g_lat_stats, 0, sizeof(g_lat_stats));
    memset(&g_detect_stats, 0, sizeof(g_detect_stats));
    memset(&g_stage_stats, 0, sizeof(g_stage_stats));
    memset(&g_soft_send_stats, 0, sizeof(g_soft_send_stats));
    g_have_last_hw = 0;
    g_stats.net_carrier = -1;
    g_rtsp_ready = 0;
    for (i = 0; i < DEMO_HEALTH_THR_MAX; ++i) {
        g_thr_alive[i] = 0;
    }

    if (mods != NULL) {
        g_mods = *mods;
    } else {
        g_mods.core = 1;
        g_mods.net = 1;
        g_mods.hw = 0;
        g_mods.bandwidth = 0;
        g_mods.latency = 0;
        g_mods.detect = 0;
        g_mods.stage = 0;
        g_mods.soft_send = 0;
        g_mods.pipe = 1;
        g_mods.disk = 0;
        g_mods.thermal = 0;
    }

    if (!enable_disk) {
        g_enabled = 0;
        if (g_fp != NULL) {
            fclose(g_fp);
            g_fp = NULL;
        }
        unsetenv("DEMO_HEALTH_LOG_PATH");
        pthread_mutex_unlock(&g_lock);
        fprintf(stderr, "[demo_health_log] disk log disabled (health_log_enable=0)\n");
        return 0;
    }

    if (dir == NULL || dir[0] == '\0') {
        dir = DEMO_HEALTH_LOG_DIR;
    }
    strncpy(g_log_dir, dir, sizeof(g_log_dir) - 1);
    g_log_dir[sizeof(g_log_dir) - 1] = '\0';

    mkdir("/mnt", 0755);
    mkdir(g_log_dir, 0755);
    mkdir(DEMO_HEALTH_LOG_DIR_FALLBACK, 0755);
    build_run_log_path(run_path, sizeof(run_path), g_log_dir);

    g_enabled = 1;
    if (open_log_unlocked(run_path) != 0) {
        build_run_log_path(run_path, sizeof(run_path), DEMO_HEALTH_LOG_DIR_FALLBACK);
        if (open_log_unlocked(run_path) != 0) {
            pthread_mutex_unlock(&g_lock);
            fprintf(stderr, "[demo_health_log] open failed: dir=%s fallback=%s (%s)\n",
                    g_log_dir, DEMO_HEALTH_LOG_DIR_FALLBACK, strerror(errno));
            g_enabled = 0;
            unsetenv("DEMO_HEALTH_LOG_PATH");
            return -1;
        }
        strncpy(g_log_dir, DEMO_HEALTH_LOG_DIR_FALLBACK, sizeof(g_log_dir) - 1);
        g_log_dir[sizeof(g_log_dir) - 1] = '\0';
    }
    /*
     * TIME_PROFILE 热路径依赖 DEMO_HEALTH_LOG_PATH；仅 stage 模块开启时设置，
     * 避免每帧 fprintf+fflush 拖垮 RTSP。
     */
    if (g_mods.stage) {
        (void)setenv("DEMO_HEALTH_LOG_PATH", g_path, 1);
    } else {
        unsetenv("DEMO_HEALTH_LOG_PATH");
    }
    g_last_heartbeat = time(NULL);
    pthread_mutex_unlock(&g_lock);

    install_crash_handlers();
    if (mod_enabled(g_mods.core)) {
        write_state_file("RUNNING");
    }

    DEMO_LOG_I("health log ready dir=%s path=%s mods=core%d/net%d/hw%d/bw%d/lat%d/det%d/stage%d/soft%d/pipe%d/disk%d/thermal%d",
               g_log_dir, g_path,
               g_mods.core, g_mods.net, g_mods.hw, g_mods.bandwidth, g_mods.latency,
               g_mods.detect, g_mods.stage, g_mods.soft_send, g_mods.pipe, g_mods.disk,
               g_mods.thermal);
    return 0;
}

int demo_health_monitor_start(void)
{
    if (!g_enabled) {
        return 0;
    }
    if (g_monitor_run) {
        return 0;
    }
    g_monitor_run = 1;
    if (pthread_create(&g_monitor_tid, NULL, monitor_thread, NULL) != 0) {
        g_monitor_run = 0;
        DEMO_LOG_E("monitor thread create failed");
        return -1;
    }
    pthread_setname_np(g_monitor_tid, "health_mon");
    demo_health_stage("health_monitor", 1, "runtime+net monitor on");
    return 0;
}

void demo_health_monitor_stop(void)
{
    if (!g_monitor_run) {
        return;
    }
    g_monitor_run = 0;
    pthread_join(g_monitor_tid, NULL);
}

void demo_health_log_deinit(void)
{
    demo_health_monitor_stop();
    if (g_enabled) {
        write_state_file("CLEAN_EXIT");
        DEMO_LOG_I("health log deinit clean exit");
    }

    pthread_mutex_lock(&g_lock);
    if (g_fp != NULL) {
        fflush(g_fp);
        fclose(g_fp);
        g_fp = NULL;
    }
    unsetenv("DEMO_HEALTH_LOG_PATH");
    g_enabled = 0;
    pthread_mutex_unlock(&g_lock);
}

void demo_health_log(demo_health_level level, const char *func, int line, const char *fmt, ...)
{
    char ts[DEMO_HEALTH_TS_LEN];
    char body[512];
    va_list ap;

    if (!g_enabled || level < DEMO_HEALTH_MIN_LEVEL) {
        return;
    }

    va_start(ap, fmt);
    vsnprintf(body, sizeof(body), fmt, ap);
    va_end(ap);

    format_ts(ts, sizeof(ts));

    pthread_mutex_lock(&g_lock);
    if (g_fp != NULL) {
        rotate_if_needed_unlocked();
        if (g_fp != NULL) {
            fprintf(g_fp, "%s [%s] [%s:%d] %s\n", ts, level_name(level),
                    func ? func : "?", line, body);
            if (level >= DEMO_HEALTH_LEVEL_ERROR) {
                fflush(g_fp);
            }
        }
    }
    pthread_mutex_unlock(&g_lock);

#if DEMO_HEALTH_MIRROR_STDOUT
    printf("%s [%s] [%s:%d] %s\n", ts, level_name(level),
           func ? func : "?", line, body);
#endif
}

void demo_health_stage(const char *stage_name, int ok, const char *detail)
{
    if (!mod_enabled(g_mods.core)) {
        return;
    }
    if (ok) {
        DEMO_LOG_I("STAGE OK  %s%s%s", stage_name,
                   (detail && detail[0]) ? " | " : "",
                   (detail && detail[0]) ? detail : "");
    } else {
        DEMO_LOG_E("STAGE FAIL %s%s%s", stage_name,
                   (detail && detail[0]) ? " | " : "",
                   (detail && detail[0]) ? detail : "");
    }
}

void demo_health_note_thread_alive(demo_health_thread_id thr)
{
    if (!mod_enabled(g_mods.core)) {
        return;
    }
    if (thr < 0 || thr >= DEMO_HEALTH_THR_MAX) {
        return;
    }
    pthread_mutex_lock(&g_lock);
    g_thr_alive[thr] = time(NULL);
    pthread_mutex_unlock(&g_lock);
}

void demo_health_note_rtsp_ready(int ready)
{
    if (!g_enabled) {
        return;
    }
    pthread_mutex_lock(&g_lock);
    g_rtsp_ready = ready ? 1 : 0;
    pthread_mutex_unlock(&g_lock);
    if (mod_enabled(g_mods.core) || mod_enabled(g_mods.pipe)) {
        DEMO_LOG_I("rtsp_ready=%d", ready ? 1 : 0);
    }
}

void demo_health_note_vpss_frame(int chn, unsigned int width, unsigned int height)
{
    unsigned long long bytes;

    if (!mod_enabled(g_mods.bandwidth) && !mod_enabled(g_mods.pipe)) {
        return;
    }
    if (chn < 0 || chn >= DEMO_HEALTH_BW_CHN_MAX || width == 0 || height == 0) {
        return;
    }
    bytes = (unsigned long long)width * (unsigned long long)height * 3ULL / 2ULL;
    pthread_mutex_lock(&g_lock);
    g_bw_stats.vpss_bytes[chn] += bytes;
    g_bw_stats.vpss_frames[chn]++;
    pthread_mutex_unlock(&g_lock);
}

void demo_health_note_npu_frame(unsigned long long input_bytes)
{
    if (!mod_enabled(g_mods.bandwidth) && !mod_enabled(g_mods.pipe)) {
        return;
    }
    pthread_mutex_lock(&g_lock);
    g_bw_stats.npu_bytes += input_bytes;
    g_bw_stats.npu_frames++;
    pthread_mutex_unlock(&g_lock);
}

void demo_health_note_rtsp_stream(int chn, unsigned long long stream_bytes)
{
    if (!mod_enabled(g_mods.bandwidth) && !mod_enabled(g_mods.pipe)) {
        return;
    }
    if (chn < 0 || chn >= DEMO_HEALTH_BW_CHN_MAX) {
        return;
    }
    pthread_mutex_lock(&g_lock);
    g_bw_stats.rtsp_bytes[chn] += stream_bytes;
    g_bw_stats.rtsp_frames[chn]++;
    pthread_mutex_unlock(&g_lock);
}

void demo_health_note_npu_timing(unsigned long long frame_pts_us,
                                 unsigned long long hold_us,
                                 unsigned long long npu_us,
                                 unsigned long long frame_age_us)
{
    (void)frame_pts_us;
    if (!mod_enabled(g_mods.latency) && !mod_enabled(g_mods.pipe)) {
        return;
    }
    pthread_mutex_lock(&g_lock);
    lat_bucket_add(&g_lat_stats.npu_proc, npu_us,
                   (unsigned long long)DEMO_HEALTH_NPU_HIGH_MS * 1000ULL);
    lat_bucket_add(&g_lat_stats.npu_hold, hold_us,
                   (unsigned long long)DEMO_HEALTH_LAT_HIGH_MS * 1000ULL);
    lat_bucket_add(&g_lat_stats.npu_age, frame_age_us,
                   (unsigned long long)DEMO_HEALTH_LAT_HIGH_MS * 1000ULL);
    g_lat_stats.npu_samples++;
    pthread_mutex_unlock(&g_lock);
}

void demo_health_note_rtsp_latency(int chn, unsigned long long latency_us)
{
    if (!mod_enabled(g_mods.latency) && !mod_enabled(g_mods.pipe)) {
        return;
    }
    if (chn < 0 || chn >= DEMO_HEALTH_BW_CHN_MAX) {
        return;
    }
    pthread_mutex_lock(&g_lock);
    lat_bucket_add(&g_lat_stats.rtsp[chn], latency_us,
                   (unsigned long long)DEMO_HEALTH_LAT_HIGH_MS * 1000ULL);
    pthread_mutex_unlock(&g_lock);
}

void demo_health_note_pipe_stage(demo_health_pipe_stage stage,
                                 unsigned long long elapsed_us)
{
    if (!mod_enabled(g_mods.latency) && !mod_enabled(g_mods.pipe)) {
        return;
    }
    if (stage < 0 || stage >= DEMO_HEALTH_PIPE_MAX) {
        return;
    }
    pthread_mutex_lock(&g_lock);
    lat_bucket_add(&g_lat_stats.pipe[stage], elapsed_us,
                   (unsigned long long)DEMO_HEALTH_LAT_HIGH_MS * 1000ULL);
    pthread_mutex_unlock(&g_lock);
}

void demo_health_note_detect(int targets, float conf_sum, int lane_cnt, int lane_pts)
{
    if (!mod_enabled(g_mods.detect)) {
        return;
    }
    if (targets < 0) {
        targets = 0;
    }
    if (lane_cnt < 0) {
        lane_cnt = 0;
    }
    if (lane_pts < 0) {
        lane_pts = 0;
    }
    pthread_mutex_lock(&g_lock);
    g_detect_stats.frames++;
    if (targets == 0) {
        g_detect_stats.zero_frames++;
    }
    g_detect_stats.targets_sum += (unsigned long long)targets;
    if ((unsigned long long)targets > g_detect_stats.targets_max) {
        g_detect_stats.targets_max = (unsigned long long)targets;
    }
    if (conf_sum > 0.0f) {
        g_detect_stats.conf_sum += (double)conf_sum;
    }
    g_detect_stats.lane_cnt_sum += (unsigned long long)lane_cnt;
    g_detect_stats.lane_pts_sum += (unsigned long long)lane_pts;
    pthread_mutex_unlock(&g_lock);
}

void demo_health_note_stage_ns(const char *stage, long long elapsed_ns)
{
    unsigned long long us;
    int i;
    int matched = 0;

    if (!mod_enabled(g_mods.stage)) {
        return;
    }
    if (stage == NULL || elapsed_ns < 0) {
        return;
    }
    us = (unsigned long long)(elapsed_ns / 1000LL);
    pthread_mutex_lock(&g_lock);
    for (i = 0; i < DEMO_HEALTH_STAGE_CNT; ++i) {
        if (strcmp(stage, g_stage_names[i]) == 0) {
            lat_bucket_add(&g_stage_stats.stage[i], us,
                           (unsigned long long)DEMO_HEALTH_NPU_HIGH_MS * 1000ULL);
            matched = 1;
            break;
        }
    }
    if (!matched) {
        lat_bucket_add(&g_stage_stats.other, us,
                       (unsigned long long)DEMO_HEALTH_NPU_HIGH_MS * 1000ULL);
    }
    pthread_mutex_unlock(&g_lock);
}

void demo_health_note_soft_send(int chn, int ok, unsigned long long elapsed_us)
{
    if (!mod_enabled(g_mods.soft_send) && !mod_enabled(g_mods.pipe)) {
        return;
    }
    if (chn < 0 || chn >= DEMO_HEALTH_BW_CHN_MAX) {
        return;
    }
    pthread_mutex_lock(&g_lock);
    if (ok) {
        g_soft_send_stats.ok[chn]++;
    } else {
        g_soft_send_stats.fail[chn]++;
    }
    lat_bucket_add(&g_soft_send_stats.send_us[chn], elapsed_us,
                   (unsigned long long)DEMO_HEALTH_LAT_HIGH_MS * 1000ULL);
    pthread_mutex_unlock(&g_lock);
}

void demo_health_note_e2e(unsigned long long e2e_us)
{
    if (!mod_enabled(g_mods.latency) && !mod_enabled(g_mods.pipe)) {
        return;
    }
    pthread_mutex_lock(&g_lock);
    lat_bucket_add(&g_lat_stats.e2e, e2e_us,
                   (unsigned long long)DEMO_HEALTH_LAT_HIGH_MS * 1000ULL);
    pthread_mutex_unlock(&g_lock);
}

void demo_health_note_vpss_fail(int ret)
{
    unsigned long long cnt;

    if (!g_enabled) {
        return;
    }
    pthread_mutex_lock(&g_lock);
    g_stats.vpss_fail_cnt++;
    cnt = g_stats.vpss_fail_cnt;
    pthread_mutex_unlock(&g_lock);

    if (mod_enabled(g_mods.core) &&
        (cnt == 1 || (cnt % DEMO_HEALTH_VPSS_LOG_EVERY) == 0)) {
        DEMO_LOG_W("vpss acquire fail ret=%#x count=%llu", ret, cnt);
    }
}

void demo_health_note_svp_ok(int target_num)
{
    unsigned long long frames;

    if (!g_enabled) {
        return;
    }
    pthread_mutex_lock(&g_lock);
    g_stats.svp_ok_frames++;
    g_stats.last_target_num = target_num;
    g_stats.last_svp_ret = 0;
    frames = g_stats.svp_ok_frames;
    pthread_mutex_unlock(&g_lock);
    demo_health_note_thread_alive(DEMO_HEALTH_THR_SVP);

    if (mod_enabled(g_mods.core) && (frames % DEMO_HEALTH_SVP_OK_EVERY) == 0) {
        DEMO_LOG_I("svp ok frames=%llu targets=%d", frames, target_num);
    }
}

void demo_health_note_svp_fail(int ret)
{
    unsigned long long cnt;

    if (!g_enabled) {
        return;
    }
    pthread_mutex_lock(&g_lock);
    g_stats.svp_fail_cnt++;
    g_stats.last_svp_ret = ret;
    cnt = g_stats.svp_fail_cnt;
    pthread_mutex_unlock(&g_lock);

    DEMO_LOG_E("svp process fail ret=%#x fail_cnt=%llu", ret, cnt);
}

void demo_health_note_venc_timeout(unsigned int total_timeout_cnt)
{
    if (!g_enabled) {
        return;
    }
    pthread_mutex_lock(&g_lock);
    g_stats.venc_timeout_cnt = total_timeout_cnt;
    pthread_mutex_unlock(&g_lock);

    if (total_timeout_cnt == 1 || (total_timeout_cnt % 10) == 0) {
        DEMO_LOG_W("venc stream timeout total=%u", total_timeout_cnt);
    }
}

void demo_health_note_venc_send_fail(int ret)
{
    unsigned long long cnt;

    if (!g_enabled) {
        return;
    }
    pthread_mutex_lock(&g_lock);
    g_stats.venc_send_fail_cnt++;
    cnt = g_stats.venc_send_fail_cnt;
    pthread_mutex_unlock(&g_lock);

    if (cnt == 1 || (cnt % 20) == 0) {
        DEMO_LOG_W("venc send_frame fail ret=%#x count=%llu", ret, cnt);
    }
}

void demo_health_note_rtsp_fail(int ret)
{
    unsigned long long cnt;

    if (!g_enabled) {
        return;
    }
    pthread_mutex_lock(&g_lock);
    g_stats.rtsp_fail_cnt++;
    cnt = g_stats.rtsp_fail_cnt;
    pthread_mutex_unlock(&g_lock);

    if (cnt == 1 || (cnt % 20) == 0) {
        DEMO_LOG_W("rtsp send fail ret=%#x count=%llu", ret, cnt);
    }
}

void demo_health_tick(void)
{
    time_t now;
    demo_health_stats snap;

    if (!g_enabled) {
        return;
    }

    now = time(NULL);
    pthread_mutex_lock(&g_lock);
    if (g_last_heartbeat != 0 && (now - g_last_heartbeat) < DEMO_HEALTH_HEARTBEAT_SEC) {
        pthread_mutex_unlock(&g_lock);
        return;
    }
    g_last_heartbeat = now;
    snap = g_stats;
    pthread_mutex_unlock(&g_lock);

    DEMO_LOG_I("HEARTBEAT svp_ok=%llu vpss_fail=%llu svp_fail=%llu venc_to=%llu "
               "rtsp_fail=%llu net=%s carrier=%d rx=%llu tx=%llu rx_err=%llu link_down=%llu stall=%llu net_evt=%llu",
               snap.svp_ok_frames, snap.vpss_fail_cnt, snap.svp_fail_cnt,
               snap.venc_timeout_cnt, snap.rtsp_fail_cnt,
               snap.net_ifname[0] ? snap.net_ifname : "-",
               snap.net_carrier, snap.net_rx_packets, snap.net_tx_packets,
               snap.net_rx_errors, snap.net_link_down_cnt, snap.net_stall_cnt,
               snap.net_event_cnt);
}

void demo_health_get_stats(demo_health_stats *out)
{
    if (out == NULL) {
        return;
    }
    pthread_mutex_lock(&g_lock);
    *out = g_stats;
    pthread_mutex_unlock(&g_lock);
}
