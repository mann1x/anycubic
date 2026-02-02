/*
 * RKMPI H.264 Hardware Encoder for RV1106 with USB Camera
 *
 * Captures video from USB camera using V4L2,
 * encodes with RV1106 hardware H.264 encoder (VENC),
 * and outputs raw H.264 Annex-B stream to stdout.
 *
 * Supports two capture modes:
 * - YUYV mode: CPU conversion to NV12, limited to camera's YUYV framerate
 * - MJPEG mode: Hardware JPEG decode (VDEC) to NV12, full framerate
 *
 * This replaces gkcam for better quality/framerate control.
 *
 * Usage: rkmpi_enc [options]
 *   -d, --device <path>  Camera device (default: /dev/video10)
 *   -w, --width <n>      Width (default: 1280)
 *   -h, --height <n>     Height (default: 720)
 *   -f, --fps <n>        Framerate (default: 10 for YUYV, 30 for MJPEG)
 *   -b, --bitrate <n>    Bitrate in kbps (default: 2000)
 *   -g, --gop <n>        GOP size (default: fps value)
 *   -p, --profile <n>    H.264 profile: 66=baseline, 77=main, 100=high (default: 100)
 *   -q, --quality        Use VBR for better quality (default: CBR)
 *   -m, --mjpeg          Use MJPEG capture mode (30fps with hardware JPEG decode)
 *   -v, --verbose        Verbose output to stderr
 *
 * Output: Raw H.264 Annex-B stream on stdout
 *
 * Build: make
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <getopt.h>
#include <errno.h>
#include <time.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/videodev2.h>

#include "rk_mpi_sys.h"
#include "rk_mpi_venc.h"
#include "rk_mpi_vdec.h"
#include "rk_mpi_mb.h"
#include "rk_mpi_mmz.h"
#include "rk_comm_venc.h"
#include "rk_comm_vdec.h"
#include "rk_comm_video.h"

/* TurboJPEG for software JPEG decoding
 * Note: RV1106 does not have MJPEG hardware decoder */
#include "turbojpeg.h"

/* Networking and server modules */
#include "frame_buffer.h"
#include "http_server.h"
#include "mqtt_client.h"
#include "rpc_client.h"
#include "display_capture.h"

/* Forward declarations */
static void log_info(const char *fmt, ...);
static void log_error(const char *fmt, ...);

/* Global verbose flag (shared with other modules) */
int g_verbose = 0;

/*
 * Timing instrumentation for profiling - enable with -DENCODER_TIMING
 * Measures time spent in each stage of the encoder pipeline
 */
#ifdef ENCODER_TIMING
#define TIMING_INTERVAL_FRAMES 100  /* Log timing every N frames */

static inline uint64_t timing_get_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

typedef struct {
    uint64_t v4l2_dqbuf;      /* V4L2 dequeue time */
    uint64_t yuyv_to_nv12;    /* YUYV→NV12 conversion */
    uint64_t jpeg_decode;     /* TurboJPEG decode (MJPEG mode) */
    uint64_t venc_jpeg;       /* VENC JPEG encode (YUYV mode) */
    uint64_t venc_h264;       /* VENC H.264 encode */
    uint64_t frame_buffer;    /* Frame buffer writes */
    uint64_t total_frame;     /* Total frame processing time */
    int count;                /* Number of frames measured */
} EncoderTiming;

static EncoderTiming g_timing = {0};

#define TIMING_START(var) uint64_t _t_##var = timing_get_us()
#define TIMING_END(field) g_timing.field += timing_get_us() - _t_##field
#define TIMING_LOG() do { \
    if (g_timing.count >= TIMING_INTERVAL_FRAMES) { \
        double n = (double)g_timing.count; \
        fprintf(stderr, "[TIMING] frames=%d avg(us): dqbuf=%.1f yuyv=%.1f jpeg_dec=%.1f " \
                "venc_jpeg=%.1f venc_h264=%.1f fb=%.1f total=%.1f\n", \
                g_timing.count, \
                g_timing.v4l2_dqbuf / n, \
                g_timing.yuyv_to_nv12 / n, \
                g_timing.jpeg_decode / n, \
                g_timing.venc_jpeg / n, \
                g_timing.venc_h264 / n, \
                g_timing.frame_buffer / n, \
                g_timing.total_frame / n); \
        memset(&g_timing, 0, sizeof(g_timing)); \
    } \
} while(0)
#else
#define TIMING_START(var) (void)0
#define TIMING_END(field) (void)0
#define TIMING_LOG() (void)0
#endif /* ENCODER_TIMING */

/*
 * Stack Smashing Protection stubs for uClibc without SSP support
 * The printer's librkaiq.so requires these symbols
 */
void *__stack_chk_guard = (void *)0xdeadbeef;
void __stack_chk_fail(void) {
    fprintf(stderr, "*** stack smashing detected ***\n");
    _exit(1);
}

/* Default configuration */
/* Version info - update on significant changes */
#define VERSION            "2.0.0"
#define BUILD_DATE         __DATE__ " " __TIME__

#define DEFAULT_DEVICE     "/dev/video10"
#define DEFAULT_WIDTH      1280
#define DEFAULT_HEIGHT     720
#define DEFAULT_FPS_YUYV   10      /* YUYV max at 1280x720 */
#define DEFAULT_FPS_MJPEG  30      /* Camera capture rate (max) */
#define DEFAULT_MJPEG_TARGET_FPS 10  /* Default target MJPEG output fps */
#define DEFAULT_BITRATE    512     /* kbps */
#define DEFAULT_PROFILE    100     /* H264E_PROFILE_HIGH */
#define DEFAULT_JPEG_QUALITY 85    /* JPEG quality for HW encode (1-99) */

/* VENC channel IDs */
#define VENC_CHN_H264      0       /* H.264 encoding channel */
#define VENC_CHN_JPEG      1       /* JPEG encoding channel (YUYV mode only) */
#define VENC_CHN_ID        VENC_CHN_H264  /* Backward compatibility */

#define V4L2_BUFFER_COUNT  5       /* 5 buffers for smoother USB delivery */

/* Control file for runtime configuration */
#define CTRL_FILE          "/tmp/h264_ctrl"
#define STATS_FILE         "/tmp/rkmpi_enc.stats"
#define CTRL_CHECK_INTERVAL 30     /* Check control file every N frames */

/* MJPEG multipart boundary for HTTP streaming */
#define MJPEG_BOUNDARY     "mjpegstream"

/* Runtime control state */
typedef struct {
    int h264_enabled;       /* 0=disabled, 1=enabled */
    int skip_ratio;         /* H.264: encode every Nth MJPEG frame (1=all, 2=half, etc.) */
    int auto_skip;          /* 0=manual, 1=auto-adjust based on CPU */
    int target_cpu;         /* Target max CPU usage % (default: 60, leaves 40% free) */
    int min_skip;           /* Minimum skip ratio (1=all frames) */
    int max_skip;           /* Maximum skip ratio */
} RuntimeCtrl;

static RuntimeCtrl g_ctrl = {
    .h264_enabled = 1,
    .skip_ratio = 2,        /* Default: encode every 2nd frame (~4-5fps) */
    .auto_skip = 0,         /* Default: manual mode */
    .target_cpu = 60,       /* Default: target 60% CPU (40% free) */
    .min_skip = 1,          /* Default: min 1 (all frames) */
    .max_skip = 10          /* Default: max 10 (~1fps from 10fps MJPEG) */
};

/* Global stats for Python to read (written to control file in server mode) */
typedef struct {
    double mjpeg_fps;
    double h264_fps;
    int mjpeg_clients;
    int flv_clients;
    RK_U64 last_update;
} EncoderStats;

static EncoderStats g_stats = {0};

/*
 * MJPEG frame rate control with adaptive detection
 *
 * Detects actual camera frame rate and only sleeps when camera is faster
 * than target. If camera is slower (e.g., 10fps vs 30fps advertised),
 * we process every frame without rate limiting.
 */
typedef struct {
    int target_fps;         /* User-configured target fps (2-30) */
    RK_U64 target_interval; /* Microseconds between frames (1000000/target_fps) */
    RK_U64 last_output_time;/* Timestamp of last output frame */
    RK_U64 last_log_time;   /* Last time we logged stats */
    int frames_in;          /* Frames received since last log */
    int frames_out;         /* Frames output since last log */
    float actual_fps;       /* Measured output fps */
    /* Adaptive rate detection */
    RK_U64 last_dqbuf_time; /* Timestamp of last DQBUF (for measuring camera rate) */
    RK_U64 camera_interval; /* Measured camera inter-frame interval (microseconds) */
    int camera_fps_detected;/* 1 if we've detected the actual camera rate */
    int rate_limit_needed;  /* 1 if camera is faster than target, need to sleep */
} MjpegRateCtrl;

static MjpegRateCtrl g_mjpeg_ctrl = {
    .target_fps = 10,       /* Default: 10 fps output */
    .target_interval = 100000, /* 1000000/10 = 100ms */
    .last_output_time = 0,
    .last_log_time = 0,
    .frames_in = 0,
    .frames_out = 0,
    .actual_fps = 0,
    .last_dqbuf_time = 0,
    .camera_interval = 0,
    .camera_fps_detected = 0,
    .rate_limit_needed = 1  /* Assume rate limiting needed until detection */
};

/*
 * Client activity tracking for idle/ramp-up logic
 *
 * When no clients are connected, skip expensive TurboJPEG decode.
 * When a client connects, gradually ramp up frame processing to prevent CPU spikes:
 *   - Second 0-1: 25% of frames (process 1 in 4)
 *   - Second 1-2: 50% of frames (process 1 in 2)
 *   - Second 2-3: 75% of frames (process 3 in 4)
 *   - Second 3+:  100% of frames (process all)
 */
typedef struct {
    int prev_client_count;      /* Previous client count (to detect transitions) */
    RK_U64 client_connect_time; /* Timestamp when client(s) connected (0 = idle) */
    int ramp_phase;             /* Current ramp-up phase (0-3, 3 = full speed) */
    int frame_counter;          /* Counter for skip logic within ramp phase */
} ClientActivityState;

static ClientActivityState g_client_state = {
    .prev_client_count = 0,
    .client_connect_time = 0,
    .ramp_phase = 3,            /* Start at full speed (will reset on first client) */
    .frame_counter = 0
};

/* Get current time in microseconds (local helper for client activity) */
static inline RK_U64 client_get_time_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (RK_U64)ts.tv_sec * 1000000 + (RK_U64)ts.tv_nsec / 1000;
}

/* Check if we should process this frame based on client activity and ramp-up state
 * Returns: 1 = process frame, 0 = skip frame
 */
static int client_activity_check(int mjpeg_clients, int flv_clients, int server_mode) {
    /* In non-server mode, always process */
    if (!server_mode) {
        return 1;
    }

    int total_clients = mjpeg_clients + flv_clients;
    RK_U64 now = client_get_time_us();

    /* Detect client connection transition (0 -> N clients) */
    if (total_clients > 0 && g_client_state.prev_client_count == 0) {
        /* Client just connected - start ramp-up */
        g_client_state.client_connect_time = now;
        g_client_state.ramp_phase = 0;
        g_client_state.frame_counter = 0;
        log_info("Client connected, starting ramp-up\n");
    }
    /* Detect client disconnection transition (N -> 0 clients) */
    else if (total_clients == 0 && g_client_state.prev_client_count > 0) {
        /* All clients disconnected - go idle */
        g_client_state.client_connect_time = 0;
        g_client_state.ramp_phase = 0;
        log_info("All clients disconnected, going idle\n");
    }

    g_client_state.prev_client_count = total_clients;

    /* If no clients, skip processing (idle mode) */
    if (total_clients == 0) {
        return 0;
    }

    /* Calculate ramp-up phase based on time since connection */
    if (g_client_state.client_connect_time > 0) {
        RK_U64 elapsed_us = now - g_client_state.client_connect_time;
        int elapsed_sec = (int)(elapsed_us / 1000000);

        /* Update ramp phase: 0=25%, 1=50%, 2=75%, 3=100% */
        int new_phase = (elapsed_sec >= 3) ? 3 : elapsed_sec;
        if (new_phase != g_client_state.ramp_phase) {
            g_client_state.ramp_phase = new_phase;
            g_client_state.frame_counter = 0;
            if (g_verbose) {
                const char *pct[] = {"25%", "50%", "75%", "100%"};
                log_info("Ramp-up phase %d: %s frame rate\n", new_phase, pct[new_phase]);
            }
        }
    }

    /* Apply skip logic based on ramp phase */
    g_client_state.frame_counter++;
    int process = 0;

    switch (g_client_state.ramp_phase) {
        case 0:  /* 25% - process 1 in 4 */
            process = (g_client_state.frame_counter % 4) == 1;
            break;
        case 1:  /* 50% - process 1 in 2 */
            process = (g_client_state.frame_counter % 2) == 1;
            break;
        case 2:  /* 75% - process 3 in 4 */
            process = (g_client_state.frame_counter % 4) != 0;
            break;
        default: /* 100% - process all */
            process = 1;
            break;
    }

    return process;
}

/* Global state */
static volatile int g_running = 1;

/* V4L2 buffer info */
typedef struct {
    void *start;
    size_t length;
} V4L2Buffer;

/* Configuration */
typedef struct {
    char device[64];
    char h264_output[128];  /* H.264 output path (empty = disabled) */
    int width;
    int height;
    int fps;
    int bitrate;      /* kbps */
    int gop;
    int profile;      /* 66=baseline, 77=main, 100=high */
    int use_vbr;      /* 0=CBR, 1=VBR */
    int mjpeg_stdout; /* Output MJPEG to stdout (multipart format) */
    int yuyv_mode;    /* 0=MJPEG capture (TurboJPEG decode), 1=YUYV capture (HW JPEG encode) */
    int jpeg_quality; /* JPEG quality for HW encode (1-99, used in YUYV mode) */
    /* Server mode options */
    int server_mode;  /* 1=enable built-in HTTP/MQTT/RPC servers */
    int no_stdout;    /* 1=disable stdout output (use with server_mode) */
    /* Operating mode */
    int vanilla_klipper;  /* 1=vanilla-klipper mode (skip MQTT/RPC) */
    /* Configurable ports */
    int streaming_port;   /* MJPEG HTTP port (default 8080) */
    /* H.264 resolution (rkmpi mode only, 0=same as camera) */
    int h264_width;
    int h264_height;
    /* Display capture */
    int display_capture;  /* 1=enable display framebuffer capture */
    int display_fps;      /* Display capture FPS (default: 5) */
} EncoderConfig;

static void log_info(const char *fmt, ...) {
    if (g_verbose) {
        va_list args;
        va_start(args, fmt);
        vfprintf(stderr, fmt, args);
        va_end(args);
        fflush(stderr);
    }
}

static void log_error(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fflush(stderr);
}

static void signal_handler(int sig) {
    log_info("Received signal %d, stopping...\n", sig);
    g_running = 0;
}

/*
 * Read runtime control file
 * Format:
 *   h264=0|1       Enable/disable H.264 encoding
 *   skip=N         Skip ratio (encode every Nth frame, 1=all)
 *   auto_skip=0|1  Enable auto-adjust skip based on CPU
 *   target_cpu=N   Target max CPU % (default 60, leaves 40% free)
 *
 * Example /tmp/h264_ctrl:
 *   h264=1
 *   skip=2
 *   auto_skip=1
 *   target_cpu=60
 */
static void read_ctrl_file(void) {
    FILE *f = fopen(CTRL_FILE, "r");
    if (!f) return;

    char line[512];  /* Larger buffer for timelapse commands with paths */
    while (fgets(line, sizeof(line), f)) {
        int val;
        /* Remove trailing newline */
        line[strcspn(line, "\n")] = 0;

        if (sscanf(line, "h264=%d", &val) == 1) {
            if (val != g_ctrl.h264_enabled) {
                g_ctrl.h264_enabled = val;
                log_info("H.264 encoding %s\n", val ? "enabled" : "disabled");
            }
        } else if (sscanf(line, "skip=%d", &val) == 1) {
            if (val >= 1 && val != g_ctrl.skip_ratio && !g_ctrl.auto_skip) {
                g_ctrl.skip_ratio = val;
                log_info("Skip ratio set to %d:1\n", val);
            }
        } else if (sscanf(line, "auto_skip=%d", &val) == 1) {
            if ((val ? 1 : 0) != g_ctrl.auto_skip) {
                g_ctrl.auto_skip = val ? 1 : 0;
                log_info("Auto-skip %s\n", g_ctrl.auto_skip ? "enabled" : "disabled");
            }
        } else if (sscanf(line, "target_cpu=%d", &val) == 1) {
            if (val >= 20 && val <= 90 && val != g_ctrl.target_cpu) {
                g_ctrl.target_cpu = val;
                log_info("Target CPU set to %d%%\n", val);
            }
        } else if (sscanf(line, "display_enabled=%d", &val) == 1) {
            display_set_enabled(val);
        } else if (sscanf(line, "display_fps=%d", &val) == 1) {
            display_set_fps(val);
        }
        /* Timelapse commands */
        else if (strncmp(line, "timelapse_init:", 15) == 0) {
            /* Format: timelapse_init:<gcode_name>:<output_path> */
            char *args = line + 15;
            char *colon = strchr(args, ':');
            if (colon) {
                *colon = '\0';
                const char *gcode_name = args;
                const char *output_path = colon + 1;
                timelapse_init(gcode_name, output_path);
            }
        } else if (strcmp(line, "timelapse_capture") == 0) {
            timelapse_capture_frame();
        } else if (strcmp(line, "timelapse_finalize") == 0) {
            timelapse_finalize();
        } else if (strcmp(line, "timelapse_cancel") == 0) {
            timelapse_cancel();
        } else if (sscanf(line, "timelapse_fps:%d", &val) == 1) {
            timelapse_set_fps(val);
        } else if (sscanf(line, "timelapse_crf:%d", &val) == 1) {
            timelapse_set_crf(val);
        } else if (strncmp(line, "timelapse_variable_fps:", 23) == 0) {
            /* Format: timelapse_variable_fps:<min>:<max>:<target_length> */
            int min_fps, max_fps, target_len;
            if (sscanf(line + 23, "%d:%d:%d", &min_fps, &max_fps, &target_len) == 3) {
                timelapse_set_variable_fps(min_fps, max_fps, target_len);
            }
        } else if (sscanf(line, "timelapse_duplicate_last:%d", &val) == 1) {
            timelapse_set_duplicate_last(val);
        } else if (strncmp(line, "timelapse_flip:", 15) == 0) {
            /* Format: timelapse_flip:<x>:<y> */
            int flip_x, flip_y;
            if (sscanf(line + 15, "%d:%d", &flip_x, &flip_y) == 2) {
                timelapse_set_flip(flip_x, flip_y);
            }
        } else if (sscanf(line, "timelapse_custom_mode:%d", &val) == 1) {
            /* Enable/disable custom timelapse mode (ignores Anycubic RPC timelapse) */
            timelapse_set_custom_mode(val);
        }
    }
    fclose(f);
}

/*
 * Write control file with current settings and stats
 * In server mode, also writes FPS and client counts for Python to read
 *
 * Important: Only write skip when auto_skip is enabled (encoder controls it).
 * When auto_skip is disabled, Python controls skip via this file, so we must
 * not overwrite it.
 */
static void write_ctrl_file(void) {
    FILE *f = fopen(CTRL_FILE, "w");
    if (!f) return;
    fprintf(f, "h264=%d\n", g_ctrl.h264_enabled);
    /* Only write skip when auto_skip is enabled (encoder controls it) */
    if (g_ctrl.auto_skip) {
        fprintf(f, "skip=%d\n", g_ctrl.skip_ratio);
    }
    fprintf(f, "auto_skip=%d\n", g_ctrl.auto_skip);
    /* Display capture settings */
    fprintf(f, "display_enabled=%d\n", display_is_enabled());
    fprintf(f, "display_fps=%d\n", display_get_fps());
    /* Stats for Python (server mode) */
    fprintf(f, "mjpeg_fps=%.1f\n", g_stats.mjpeg_fps);
    fprintf(f, "h264_fps=%.1f\n", g_stats.h264_fps);
    fprintf(f, "mjpeg_clients=%d\n", g_stats.mjpeg_clients);
    fprintf(f, "flv_clients=%d\n", g_stats.flv_clients);
    fprintf(f, "display_clients=%d\n", display_get_client_count());
    fclose(f);
}

static RK_U64 get_timestamp_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (RK_U64)ts.tv_sec * 1000000 + (RK_U64)ts.tv_nsec / 1000;
}

/*
 * Convert YUYV (YUV422 packed) to NV12 (YUV420SP)
 * YUYV: Y0 U0 Y1 V0 Y2 U1 Y3 V1 ...
 * NV12: Y plane, then interleaved UV plane (half height)
 *
 * Optimized version - processes 2 rows at a time, reduces branches
 */
static void yuyv_to_nv12(const uint8_t *yuyv, uint8_t *nv12_y, uint8_t *nv12_uv,
                         int width, int height) {
    const int row_stride = width * 2;  /* YUYV is 2 bytes per pixel */

    /* Process 2 rows at a time */
    for (int y = 0; y < height; y += 2) {
        const uint8_t *src0 = yuyv + y * row_stride;
        const uint8_t *src1 = src0 + row_stride;
        uint8_t *dst_y0 = nv12_y + y * width;
        uint8_t *dst_y1 = dst_y0 + width;
        uint8_t *dst_uv = nv12_uv + (y / 2) * width;

        for (int x = 0; x < width; x += 2) {
            /* Extract Y from both rows */
            dst_y0[0] = src0[0];  /* Y0 row0 */
            dst_y0[1] = src0[2];  /* Y1 row0 */
            dst_y1[0] = src1[0];  /* Y0 row1 */
            dst_y1[1] = src1[2];  /* Y1 row1 */

            /* Average UV from both rows */
            dst_uv[0] = ((unsigned)src0[1] + (unsigned)src1[1]) >> 1;  /* U */
            dst_uv[1] = ((unsigned)src0[3] + (unsigned)src1[3]) >> 1;  /* V */

            src0 += 4;
            src1 += 4;
            dst_y0 += 2;
            dst_y1 += 2;
            dst_uv += 2;
        }
    }
}

/*
 * Initialize V4L2 camera capture
 */
static int v4l2_init(const char *device, int width, int height, int fps,
                     int use_mjpeg, int *fd_out, V4L2Buffer **buffers_out) {
    int fd = open(device, O_RDWR);
    if (fd < 0) {
        log_error("Cannot open %s: %s\n", device, strerror(errno));
        return -1;
    }

    /* Query capabilities */
    struct v4l2_capability cap;
    if (ioctl(fd, VIDIOC_QUERYCAP, &cap) < 0) {
        log_error("VIDIOC_QUERYCAP failed: %s\n", strerror(errno));
        close(fd);
        return -1;
    }

    log_info("Camera: %s (%s)\n", cap.card, cap.driver);

    if (!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE)) {
        log_error("Device does not support video capture\n");
        close(fd);
        return -1;
    }

    if (!(cap.capabilities & V4L2_CAP_STREAMING)) {
        log_error("Device does not support streaming\n");
        close(fd);
        return -1;
    }

    /* Set format */
    struct v4l2_format fmt = {0};
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width = width;
    fmt.fmt.pix.height = height;
    fmt.fmt.pix.pixelformat = use_mjpeg ? V4L2_PIX_FMT_MJPEG : V4L2_PIX_FMT_YUYV;
    fmt.fmt.pix.field = V4L2_FIELD_NONE;

    if (ioctl(fd, VIDIOC_S_FMT, &fmt) < 0) {
        log_error("VIDIOC_S_FMT failed: %s\n", strerror(errno));
        close(fd);
        return -1;
    }

    log_info("Format: %dx%d %s\n", fmt.fmt.pix.width, fmt.fmt.pix.height,
             use_mjpeg ? "MJPEG" : "YUYV");

    /* Set framerate */
    struct v4l2_streamparm parm = {0};
    parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    parm.parm.capture.timeperframe.numerator = 1;
    parm.parm.capture.timeperframe.denominator = fps;

    if (ioctl(fd, VIDIOC_S_PARM, &parm) < 0) {
        log_info("VIDIOC_S_PARM failed (non-fatal): %s\n", strerror(errno));
    } else {
        int actual_fps = parm.parm.capture.timeperframe.denominator /
                         parm.parm.capture.timeperframe.numerator;
        log_info("Framerate: %d fps\n", actual_fps);
    }

    /* Request buffers */
    struct v4l2_requestbuffers req = {0};
    req.count = V4L2_BUFFER_COUNT;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;

    if (ioctl(fd, VIDIOC_REQBUFS, &req) < 0) {
        log_error("VIDIOC_REQBUFS failed: %s\n", strerror(errno));
        close(fd);
        return -1;
    }

    log_info("Allocated %d buffers\n", req.count);

    /* Map buffers */
    V4L2Buffer *buffers = calloc(req.count, sizeof(V4L2Buffer));
    if (!buffers) {
        log_error("Failed to allocate buffer info\n");
        close(fd);
        return -1;
    }

    for (unsigned int i = 0; i < req.count; i++) {
        struct v4l2_buffer buf = {0};
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;

        if (ioctl(fd, VIDIOC_QUERYBUF, &buf) < 0) {
            log_error("VIDIOC_QUERYBUF failed: %s\n", strerror(errno));
            free(buffers);
            close(fd);
            return -1;
        }

        buffers[i].length = buf.length;
        buffers[i].start = mmap(NULL, buf.length, PROT_READ | PROT_WRITE,
                                MAP_SHARED, fd, buf.m.offset);

        if (buffers[i].start == MAP_FAILED) {
            log_error("mmap failed: %s\n", strerror(errno));
            free(buffers);
            close(fd);
            return -1;
        }
    }

    /* Queue all buffers */
    for (unsigned int i = 0; i < req.count; i++) {
        struct v4l2_buffer buf = {0};
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;

        if (ioctl(fd, VIDIOC_QBUF, &buf) < 0) {
            log_error("VIDIOC_QBUF failed: %s\n", strerror(errno));
            free(buffers);
            close(fd);
            return -1;
        }
    }

    /* Start streaming */
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(fd, VIDIOC_STREAMON, &type) < 0) {
        log_error("VIDIOC_STREAMON failed: %s\n", strerror(errno));
        free(buffers);
        close(fd);
        return -1;
    }

    log_info("V4L2 capture started\n");

    *fd_out = fd;
    *buffers_out = buffers;
    return req.count;
}

static void v4l2_stop(int fd, V4L2Buffer *buffers, int buffer_count) {
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    ioctl(fd, VIDIOC_STREAMOFF, &type);

    for (int i = 0; i < buffer_count; i++) {
        if (buffers[i].start && buffers[i].start != MAP_FAILED) {
            munmap(buffers[i].start, buffers[i].length);
        }
    }

    free(buffers);
    close(fd);
}

/*
 * TurboJPEG software decoder context
 * Note: RV1106 does not have MJPEG hardware decoder, so we use software decoding
 */
static tjhandle g_tjhandle = NULL;

/* Pre-allocated YUV buffer for JPEG decode (reused across frames) */
static uint8_t *g_yuv_buffer = NULL;
static size_t g_yuv_buffer_size = 0;

/* CPU usage tracking for auto-skip */
typedef struct {
    unsigned long long prev_total;
    unsigned long long prev_idle;
    int current_usage;      /* 0-100 */
} CPUStats;

static CPUStats g_cpu_stats = {0};

/*
 * Auto-skip algorithm state for smoothing and hysteresis
 *
 * Design principles:
 * - React FAST to high CPU (increase skip quickly to protect system)
 * - React SLOW to low CPU (decrease skip gradually to avoid oscillation)
 * - Use smoothed CPU for recovery decisions, instant CPU for emergency response
 * - Require sustained low CPU before improving quality
 */
#define AUTOSKIP_HISTORY_SIZE    8       /* Rolling average window (4s at 500ms interval) */
#define AUTOSKIP_COOLDOWN_MS     3000    /* Wait 3s after increase before allowing decrease */
#define AUTOSKIP_STABLE_COUNT    6       /* Need 6 consecutive low readings (~3s) to decrease */
#define AUTOSKIP_HIGH_THRESHOLD  8       /* Increase if CPU > target + 8% (instant reaction) */
#define AUTOSKIP_LOW_THRESHOLD   20      /* Decrease if smoothed CPU < target - 20% */
#define AUTOSKIP_EMERGENCY_THRESHOLD 25  /* Emergency if CPU > target + 25% */

typedef struct {
    int cpu_history[AUTOSKIP_HISTORY_SIZE];  /* Rolling CPU readings */
    int history_idx;                          /* Current index in circular buffer */
    int history_count;                        /* Number of valid readings (0 to SIZE) */
    int stable_low_count;                     /* Consecutive readings below recovery threshold */
    RK_U64 last_increase_time;                /* Timestamp of last skip increase (us) */
} AutoSkipState;

static AutoSkipState g_autoskip = {0};

/*
 * Read CPU usage from /proc/stat
 * Returns CPU usage percentage (0-100)
 */
static int read_cpu_usage(void) {
    FILE *f = fopen("/proc/stat", "r");
    if (!f) return -1;

    char line[256];
    if (!fgets(line, sizeof(line), f)) {
        fclose(f);
        return -1;
    }
    fclose(f);

    unsigned long long user, nice, system, idle, iowait, irq, softirq;
    if (sscanf(line, "cpu %llu %llu %llu %llu %llu %llu %llu",
               &user, &nice, &system, &idle, &iowait, &irq, &softirq) < 4) {
        return -1;
    }

    unsigned long long total = user + nice + system + idle + iowait + irq + softirq;
    unsigned long long idle_all = idle + iowait;

    int usage = 0;
    if (g_cpu_stats.prev_total > 0) {
        unsigned long long total_diff = total - g_cpu_stats.prev_total;
        unsigned long long idle_diff = idle_all - g_cpu_stats.prev_idle;
        if (total_diff > 0) {
            usage = (int)(100 * (total_diff - idle_diff) / total_diff);
        }
    }

    g_cpu_stats.prev_total = total;
    g_cpu_stats.prev_idle = idle_all;
    g_cpu_stats.current_usage = usage;

    return usage;
}

/*
 * Calculate smoothed CPU from rolling history
 * Returns -1 if not enough samples yet
 */
static int get_smoothed_cpu(void) {
    if (g_autoskip.history_count < 3) {
        return -1;  /* Need at least 3 samples for meaningful average */
    }

    int sum = 0;
    for (int i = 0; i < g_autoskip.history_count; i++) {
        sum += g_autoskip.cpu_history[i];
    }
    return sum / g_autoskip.history_count;
}

/*
 * Add CPU reading to history buffer
 */
static void add_cpu_to_history(int cpu) {
    g_autoskip.cpu_history[g_autoskip.history_idx] = cpu;
    g_autoskip.history_idx = (g_autoskip.history_idx + 1) % AUTOSKIP_HISTORY_SIZE;
    if (g_autoskip.history_count < AUTOSKIP_HISTORY_SIZE) {
        g_autoskip.history_count++;
    }
}

/*
 * Auto-adjust skip ratio based on CPU usage with smoothing and hysteresis
 * Called periodically (every 500ms)
 *
 * Algorithm design:
 * - FAST INCREASE: React immediately to high CPU (use instant reading)
 *   - Proportional response: bigger overage = bigger increase
 *   - Emergency mode for extreme CPU spikes
 *
 * - SLOW DECREASE: React conservatively to low CPU (use smoothed reading)
 *   - Require cooldown period after last increase
 *   - Require sustained low CPU (multiple consecutive readings)
 *   - Only decrease by 1 step at a time
 *   - Wider hysteresis band to prevent oscillation
 */
static void auto_adjust_skip_ratio(void) {
    if (!g_ctrl.auto_skip) return;

    int cpu = read_cpu_usage();
    if (cpu < 0) return;

    /* Add to history for smoothing */
    add_cpu_to_history(cpu);

    int old_skip = g_ctrl.skip_ratio;
    int target = g_ctrl.target_cpu;
    RK_U64 now = get_timestamp_us();
    const char *action = NULL;

    /*
     * FAST PATH: Increase skip (reduce quality) on high CPU
     * Uses INSTANT CPU reading for fast reaction
     */
    if (cpu > target + AUTOSKIP_HIGH_THRESHOLD) {
        int overage = cpu - target;
        int steps = 1;

        /* Proportional response: more aggressive when further over target */
        if (overage > AUTOSKIP_EMERGENCY_THRESHOLD + 15) {
            steps = 4;  /* Critical: jump aggressively */
            action = "CRITICAL";
        } else if (overage > AUTOSKIP_EMERGENCY_THRESHOLD) {
            steps = 3;  /* Emergency response */
            action = "EMERGENCY";
        } else if (overage > AUTOSKIP_HIGH_THRESHOLD + 7) {
            steps = 2;  /* Moderate increase */
            action = "HIGH";
        } else {
            steps = 1;  /* Gentle increase */
            action = "above";
        }

        g_ctrl.skip_ratio += steps;
        if (g_ctrl.skip_ratio > g_ctrl.max_skip) {
            g_ctrl.skip_ratio = g_ctrl.max_skip;
        }

        /* Record increase time and reset stability counter */
        g_autoskip.last_increase_time = now;
        g_autoskip.stable_low_count = 0;
    }
    /*
     * SLOW PATH: Decrease skip (improve quality) on low CPU
     * Uses SMOOTHED CPU reading and requires stability
     */
    else {
        int smoothed_cpu = get_smoothed_cpu();

        /* Check if CPU is below recovery threshold */
        if (smoothed_cpu >= 0 && smoothed_cpu < target - AUTOSKIP_LOW_THRESHOLD) {
            g_autoskip.stable_low_count++;
        } else {
            /* Reset stability counter if not consistently low */
            g_autoskip.stable_low_count = 0;
        }

        /* Check all conditions for decreasing skip ratio */
        int cooldown_elapsed = (now - g_autoskip.last_increase_time) >=
                               (AUTOSKIP_COOLDOWN_MS * 1000ULL);
        int stable_enough = g_autoskip.stable_low_count >= AUTOSKIP_STABLE_COUNT;
        int can_decrease = g_ctrl.skip_ratio > g_ctrl.min_skip;

        if (cooldown_elapsed && stable_enough && can_decrease) {
            g_ctrl.skip_ratio--;
            g_autoskip.stable_low_count = 0;  /* Reset after decrease */
            action = "stable-low";
        }
    }

    /* Log changes */
    if (g_ctrl.skip_ratio != old_skip) {
        int smoothed = get_smoothed_cpu();
        log_info("Auto-skip: CPU=%d%% (avg=%d%%, target=%d%%), skip %d->%d [%s]\n",
                 cpu, smoothed >= 0 ? smoothed : cpu, target,
                 old_skip, g_ctrl.skip_ratio, action ? action : "?");
        write_ctrl_file();
    }
}

/*
 * Time-based MJPEG frame rate control
 *
 * Called on each captured frame. Outputs a frame only if enough time
 * has passed since the last scheduled output time.
 *
 * Key insight: We advance last_output_time by target_interval (not set to now)
 * to allow "catching up" when frames arrive late. This gives accurate ±1fps.
 *
 * Returns: 1 if this frame should be processed, 0 if it should be skipped
 */
static int mjpeg_rate_control(RK_U64 frame_count) {
    (void)frame_count;  /* Not used in time-based approach */
    RK_U64 now = get_timestamp_us();
    int output = 0;

    /* Count incoming frames */
    g_mjpeg_ctrl.frames_in++;

    /* Initialize on first frame */
    if (g_mjpeg_ctrl.last_output_time == 0) {
        g_mjpeg_ctrl.last_output_time = now;
        g_mjpeg_ctrl.last_log_time = now;
        g_mjpeg_ctrl.frames_out++;
        return 1;  /* Always output first frame */
    }

    /* Check if enough time has passed for next frame */
    if (now >= g_mjpeg_ctrl.last_output_time + g_mjpeg_ctrl.target_interval) {
        /* Time to output a frame - advance by interval, not to now
         * This allows catching up if frames arrive late */
        g_mjpeg_ctrl.last_output_time += g_mjpeg_ctrl.target_interval;

        /* If we've fallen behind by more than 2 intervals, reset to now
         * (prevents burst of frames after a stall) */
        if (now > g_mjpeg_ctrl.last_output_time + g_mjpeg_ctrl.target_interval * 2) {
            g_mjpeg_ctrl.last_output_time = now;
        }

        g_mjpeg_ctrl.frames_out++;
        output = 1;
    }

    /* Log stats every 5 seconds */
    if ((now - g_mjpeg_ctrl.last_log_time) >= 5000000) {
        float log_elapsed = (now - g_mjpeg_ctrl.last_log_time) / 1000000.0f;
        float in_fps = g_mjpeg_ctrl.frames_in / log_elapsed;
        g_mjpeg_ctrl.actual_fps = g_mjpeg_ctrl.frames_out / log_elapsed;

        log_info("MJPEG rate: camera=%.1f fps, output=%.1f fps (target=%d)\n",
                 in_fps, g_mjpeg_ctrl.actual_fps, g_mjpeg_ctrl.target_fps);

        /* Reset counters */
        g_mjpeg_ctrl.frames_in = 0;
        g_mjpeg_ctrl.frames_out = 0;
        g_mjpeg_ctrl.last_log_time = now;
    }

    return output;
}

/*
 * Initialize TurboJPEG software decoder
 */
static int init_turbojpeg_decoder(void) {
    g_tjhandle = tjInitDecompress();
    if (g_tjhandle == NULL) {
        log_error("tjInitDecompress failed: %s\n", tjGetErrorStr());
        return -1;
    }
    log_info("TurboJPEG decoder initialized (software, fast mode)\n");
    return 0;
}

static void cleanup_turbojpeg_decoder(void) {
    if (g_tjhandle) {
        tjDestroy(g_tjhandle);
        g_tjhandle = NULL;
    }
    /* Free pre-allocated YUV buffer */
    if (g_yuv_buffer) {
        free(g_yuv_buffer);
        g_yuv_buffer = NULL;
        g_yuv_buffer_size = 0;
    }
}

/*
 * Ensure pre-allocated YUV buffer is large enough
 */
static int ensure_yuv_buffer(int width, int height) {
    size_t y_size = width * height;
    size_t uv_size = (width / 2) * (height / 2);
    size_t needed = y_size + uv_size * 2;

    if (g_yuv_buffer && g_yuv_buffer_size >= needed) {
        return 0;  /* Already allocated */
    }

    if (g_yuv_buffer) {
        free(g_yuv_buffer);
    }

    g_yuv_buffer = malloc(needed);
    if (!g_yuv_buffer) {
        log_error("Failed to allocate YUV buffer (%zu bytes)\n", needed);
        g_yuv_buffer_size = 0;
        return -1;
    }

    g_yuv_buffer_size = needed;
    log_info("Allocated YUV buffer: %zu bytes\n", needed);
    return 0;
}

/*
 * UV interleave (I420 U+V planes to NV12 UV plane)
 * Simple scalar version - NEON version had issues with color
 */
static void interleave_uv(const uint8_t *u_plane, const uint8_t *v_plane,
                          uint8_t *nv12_uv, size_t uv_size) {
    for (size_t i = 0; i < uv_size; i++) {
        nv12_uv[i * 2] = u_plane[i];
        nv12_uv[i * 2 + 1] = v_plane[i];
    }
}

/*
 * Decode JPEG to NV12 using TurboJPEG with optional scaling
 * Decodes to YUV420P first (I420), then converts to NV12 (YUV420SP)
 *
 * Parameters:
 * - src_width/src_height: Expected JPEG source dimensions
 * - dst_width/dst_height: Desired output dimensions (for scaling)
 *
 * Optimizations:
 * - Pre-allocated buffer (no malloc per frame)
 * - TJFLAG_FASTDCT | TJFLAG_FASTUPSAMPLE for faster decode
 * - NEON SIMD for UV interleaving
 * - TurboJPEG DCT scaling (decodes fewer pixels when scaling down)
 */
static int decode_jpeg_to_nv12(const uint8_t *jpeg_data, size_t jpeg_len,
                                uint8_t *nv12_y, uint8_t *nv12_uv,
                                int src_width, int src_height,
                                int dst_width, int dst_height) {
    int jpeg_width, jpeg_height, jpeg_subsamp, jpeg_colorspace;

    /* Get JPEG parameters */
    if (tjDecompressHeader3(g_tjhandle, jpeg_data, jpeg_len,
                            &jpeg_width, &jpeg_height, &jpeg_subsamp, &jpeg_colorspace) != 0) {
        log_error("tjDecompressHeader3 failed: %s\n", tjGetErrorStr());
        return -1;
    }

    if (jpeg_width != src_width || jpeg_height != src_height) {
        log_error("JPEG size mismatch: %dx%d vs expected %dx%d\n",
                  jpeg_width, jpeg_height, src_width, src_height);
        return -1;
    }

    /* Determine if scaling is needed */
    int scale_num = 1, scale_denom = 1;
    int decode_width = src_width;
    int decode_height = src_height;

    if (dst_width < src_width || dst_height < src_height) {
        /* Find best TurboJPEG scaling factor
         * Available: 1/1, 7/8, 3/4, 5/8, 1/2, 3/8, 1/4, 1/8 */
        int num_factors;
        tjscalingfactor *factors = tjGetScalingFactors(&num_factors);
        if (factors) {
            /* Find smallest factor that produces >= dst dimensions */
            for (int i = num_factors - 1; i >= 0; i--) {
                int sw = TJSCALED(src_width, factors[i]);
                int sh = TJSCALED(src_height, factors[i]);
                if (sw >= dst_width && sh >= dst_height) {
                    scale_num = factors[i].num;
                    scale_denom = factors[i].denom;
                    decode_width = sw;
                    decode_height = sh;
                    break;
                }
            }
        }
        static int logged_scale = 0;
        if (!logged_scale) {
            log_info("JPEG scaling: %dx%d -> %dx%d (factor %d/%d, target %dx%d)\n",
                     src_width, src_height, decode_width, decode_height,
                     scale_num, scale_denom, dst_width, dst_height);
            logged_scale = 1;
        }
    }

    /* Use decode dimensions for the rest of the function */
    int width = decode_width;
    int height = decode_height;

    /* Debug: log JPEG format (once) */
    static int logged_format = 0;
    if (!logged_format) {
        const char *subsamp_names[] = {"444", "422", "420", "GRAY", "440", "411"};
        const char *cs_names[] = {"RGB", "YCbCr", "GRAY", "CMYK", "YCCK"};
        log_info("JPEG format: subsamp=%s colorspace=%s\n",
                 (jpeg_subsamp >= 0 && jpeg_subsamp < 6) ? subsamp_names[jpeg_subsamp] : "?",
                 (jpeg_colorspace >= 0 && jpeg_colorspace < 5) ? cs_names[jpeg_colorspace] : "?");
        logged_format = 1;
    }

    /* Calculate buffer size using TurboJPEG helper (handles all subsamplings correctly) */
    size_t y_size = width * height;
    unsigned long needed = tjBufSizeYUV2(width, 1, height, jpeg_subsamp);
    if (needed == (unsigned long)-1) {
        log_error("tjBufSizeYUV2 failed\n");
        return -1;
    }

    /* Ensure buffer is large enough */
    if (g_yuv_buffer_size < needed) {
        free(g_yuv_buffer);
        g_yuv_buffer = malloc(needed);
        if (!g_yuv_buffer) {
            log_error("Failed to allocate YUV buffer (%lu bytes)\n", needed);
            g_yuv_buffer_size = 0;
            return -1;
        }
        g_yuv_buffer_size = needed;
        log_info("Allocated YUV buffer: %lu bytes (for %dx%d %s)\n", needed, width, height,
                 jpeg_subsamp == TJSAMP_422 ? "4:2:2" : "4:2:0");
    }

    /* Decode JPEG to YUV with fast flags
     * tjDecompressToYUV2 supports scaling - width/height can be smaller than source */
    int flags = TJFLAG_FASTDCT | TJFLAG_FASTUPSAMPLE;
    if (tjDecompressToYUV2(g_tjhandle, jpeg_data, jpeg_len,
                           g_yuv_buffer, width, 1, height, flags) != 0) {
        log_error("tjDecompressToYUV2 failed: %s\n", tjGetErrorStr());
        return -1;
    }

    /* tjDecompressToYUV2 outputs in planar format with proper plane sizes
     * Calculate UV plane dimensions based on subsampling */
    size_t uv_width = (jpeg_subsamp == TJSAMP_444) ? width : width / 2;
    size_t uv_height;
    if (jpeg_subsamp == TJSAMP_422 || jpeg_subsamp == TJSAMP_444) {
        uv_height = height;  /* 4:2:2 or 4:4:4: full height */
    } else {
        uv_height = height / 2;  /* 4:2:0: half height */
    }
    size_t uv_plane_size = uv_width * uv_height;

    uint8_t *planes[3] = {
        g_yuv_buffer,
        g_yuv_buffer + y_size,
        g_yuv_buffer + y_size + uv_plane_size
    };

    /* Copy Y plane directly to output */
    memcpy(nv12_y, planes[0], y_size);

    /* Convert U/V to NV12 (interleaved, always half width/height for NV12) */
    size_t dst_uv_width = width / 2;
    size_t dst_uv_height = height / 2;

    if (jpeg_subsamp == TJSAMP_422) {
        /* 4:2:2 to NV12: need to vertically subsample U/V (average pairs of rows) */
        for (size_t y = 0; y < dst_uv_height; y++) {
            const uint8_t *u_row0 = planes[1] + (y * 2) * uv_width;
            const uint8_t *u_row1 = planes[1] + (y * 2 + 1) * uv_width;
            const uint8_t *v_row0 = planes[2] + (y * 2) * uv_width;
            const uint8_t *v_row1 = planes[2] + (y * 2 + 1) * uv_width;
            uint8_t *dst = nv12_uv + y * dst_uv_width * 2;

            for (size_t x = 0; x < dst_uv_width; x++) {
                /* Average two vertical samples for each U and V */
                dst[x * 2] = (u_row0[x] + u_row1[x] + 1) >> 1;      /* U */
                dst[x * 2 + 1] = (v_row0[x] + v_row1[x] + 1) >> 1;  /* V */
            }
        }
    } else {
        /* 4:2:0: direct interleave */
        interleave_uv(planes[1], planes[2], nv12_uv, dst_uv_width * dst_uv_height);
    }

    return 0;
}

/*
 * Initialize VENC (hardware H.264 encoder)
 */
static int init_venc(EncoderConfig *cfg) {
    RK_S32 ret;
    VENC_CHN_ATTR_S stAttr;
    VENC_RECV_PIC_PARAM_S stRecvParam;

    memset(&stAttr, 0, sizeof(stAttr));
    memset(&stRecvParam, 0, sizeof(stRecvParam));

    /* Encoder type - use h264_width/h264_height for encoding dimensions */
    int enc_width = cfg->h264_width ? cfg->h264_width : cfg->width;
    int enc_height = cfg->h264_height ? cfg->h264_height : cfg->height;

    stAttr.stVencAttr.enType = RK_VIDEO_ID_AVC;  /* H.264 */
    stAttr.stVencAttr.enPixelFormat = RK_FMT_YUV420SP;  /* NV12 input */
    stAttr.stVencAttr.u32Profile = cfg->profile;
    stAttr.stVencAttr.u32PicWidth = enc_width;
    stAttr.stVencAttr.u32PicHeight = enc_height;
    stAttr.stVencAttr.u32VirWidth = enc_width;
    stAttr.stVencAttr.u32VirHeight = enc_height;
    stAttr.stVencAttr.u32StreamBufCnt = 4;
    stAttr.stVencAttr.u32BufSize = enc_width * enc_height * 3 / 2;
    stAttr.stVencAttr.enMirror = MIRROR_NONE;

    /* Rate control */
    if (cfg->use_vbr) {
        stAttr.stRcAttr.enRcMode = VENC_RC_MODE_H264VBR;
        stAttr.stRcAttr.stH264Vbr.u32BitRate = cfg->bitrate;
        stAttr.stRcAttr.stH264Vbr.u32MaxBitRate = cfg->bitrate * 2;
        stAttr.stRcAttr.stH264Vbr.u32MinBitRate = cfg->bitrate / 2;
        stAttr.stRcAttr.stH264Vbr.u32Gop = cfg->gop;
        stAttr.stRcAttr.stH264Vbr.u32SrcFrameRateNum = cfg->fps;
        stAttr.stRcAttr.stH264Vbr.u32SrcFrameRateDen = 1;
        stAttr.stRcAttr.stH264Vbr.fr32DstFrameRateNum = cfg->fps;
        stAttr.stRcAttr.stH264Vbr.fr32DstFrameRateDen = 1;
    } else {
        stAttr.stRcAttr.enRcMode = VENC_RC_MODE_H264CBR;
        stAttr.stRcAttr.stH264Cbr.u32BitRate = cfg->bitrate;
        stAttr.stRcAttr.stH264Cbr.u32Gop = cfg->gop;
        stAttr.stRcAttr.stH264Cbr.u32SrcFrameRateNum = cfg->fps;
        stAttr.stRcAttr.stH264Cbr.u32SrcFrameRateDen = 1;
        stAttr.stRcAttr.stH264Cbr.fr32DstFrameRateNum = cfg->fps;
        stAttr.stRcAttr.stH264Cbr.fr32DstFrameRateDen = 1;
    }

    /* GOP mode */
    stAttr.stGopAttr.enGopMode = VENC_GOPMODE_NORMALP;
    stAttr.stGopAttr.s32VirIdrLen = cfg->gop;

    ret = RK_MPI_VENC_CreateChn(VENC_CHN_ID, &stAttr);
    if (ret != RK_SUCCESS) {
        log_error("RK_MPI_VENC_CreateChn failed: 0x%x\n", ret);
        return -1;
    }

    /* Start receiving frames */
    stRecvParam.s32RecvPicNum = -1;  /* Continuous */
    ret = RK_MPI_VENC_StartRecvFrame(VENC_CHN_ID, &stRecvParam);
    if (ret != RK_SUCCESS) {
        log_error("RK_MPI_VENC_StartRecvFrame failed: 0x%x\n", ret);
        return -1;
    }

    log_info("VENC initialized: %dx%d @ %dfps, %dkbps, GOP=%d, profile=%d, %s\n",
             enc_width, enc_height, cfg->fps, cfg->bitrate, cfg->gop,
             cfg->profile, cfg->use_vbr ? "VBR" : "CBR");
    return 0;
}

static void cleanup_venc(void) {
    RK_MPI_VENC_StopRecvFrame(VENC_CHN_H264);
    RK_MPI_VENC_DestroyChn(VENC_CHN_H264);
}

/*
 * Initialize VENC for JPEG/MJPEG encoding (hardware JPEG encoder)
 * Used in YUYV mode to encode NV12 frames to JPEG
 */
static int init_venc_jpeg(EncoderConfig *cfg) {
    RK_S32 ret;
    VENC_CHN_ATTR_S stAttr;
    VENC_RECV_PIC_PARAM_S stRecvParam;

    memset(&stAttr, 0, sizeof(stAttr));
    memset(&stRecvParam, 0, sizeof(stRecvParam));

    /* Encoder type - MJPEG for continuous frame encoding */
    stAttr.stVencAttr.enType = RK_VIDEO_ID_MJPEG;
    stAttr.stVencAttr.enPixelFormat = RK_FMT_YUV420SP;  /* NV12 input */
    stAttr.stVencAttr.u32PicWidth = cfg->width;
    stAttr.stVencAttr.u32PicHeight = cfg->height;
    stAttr.stVencAttr.u32VirWidth = cfg->width;
    stAttr.stVencAttr.u32VirHeight = cfg->height;
    stAttr.stVencAttr.u32StreamBufCnt = 2;
    stAttr.stVencAttr.u32BufSize = cfg->width * cfg->height * 3 / 2;
    stAttr.stVencAttr.enMirror = MIRROR_NONE;

    /* Rate control - fixed quality for JPEG */
    stAttr.stRcAttr.enRcMode = VENC_RC_MODE_MJPEGFIXQP;
    stAttr.stRcAttr.stMjpegFixQp.u32Qfactor = cfg->jpeg_quality;
    stAttr.stRcAttr.stMjpegFixQp.u32SrcFrameRateNum = cfg->fps;
    stAttr.stRcAttr.stMjpegFixQp.u32SrcFrameRateDen = 1;
    stAttr.stRcAttr.stMjpegFixQp.fr32DstFrameRateNum = cfg->fps;
    stAttr.stRcAttr.stMjpegFixQp.fr32DstFrameRateDen = 1;

    ret = RK_MPI_VENC_CreateChn(VENC_CHN_JPEG, &stAttr);
    if (ret != RK_SUCCESS) {
        log_error("RK_MPI_VENC_CreateChn(JPEG) failed: 0x%x\n", ret);
        return -1;
    }

    /* Start receiving frames */
    stRecvParam.s32RecvPicNum = -1;  /* Continuous */
    ret = RK_MPI_VENC_StartRecvFrame(VENC_CHN_JPEG, &stRecvParam);
    if (ret != RK_SUCCESS) {
        log_error("RK_MPI_VENC_StartRecvFrame(JPEG) failed: 0x%x\n", ret);
        RK_MPI_VENC_DestroyChn(VENC_CHN_JPEG);
        return -1;
    }

    log_info("VENC JPEG initialized: %dx%d, quality=%d\n",
             cfg->width, cfg->height, cfg->jpeg_quality);
    return 0;
}

static void cleanup_venc_jpeg(void) {
    RK_MPI_VENC_StopRecvFrame(VENC_CHN_JPEG);
    RK_MPI_VENC_DestroyChn(VENC_CHN_JPEG);
}

static void print_version(void) {
    fprintf(stderr, "rkmpi_enc version %s (built %s)\n", VERSION, BUILD_DATE);
}

static void print_usage(const char *prog) {
    fprintf(stderr, "Combined MJPEG/H.264 Streamer for RV1106 (USB Camera)\n");
    fprintf(stderr, "Version %s (built %s)\n\n", VERSION, BUILD_DATE);
    fprintf(stderr, "Captures video from USB camera and outputs:\n");
    fprintf(stderr, "  - MJPEG stream on stdout (multipart format for HTTP)\n");
    fprintf(stderr, "  - H.264 stream to file/pipe (optional, runtime controllable)\n\n");
    fprintf(stderr, "Server mode (-S) provides built-in HTTP/MQTT/RPC servers:\n");
    fprintf(stderr, "  - MJPEG: http://localhost:8080/stream, /snapshot\n");
    fprintf(stderr, "  - FLV:   http://localhost:18088/flv\n");
    fprintf(stderr, "  - MQTT:  Video responder on port 9883 (TLS)\n");
    fprintf(stderr, "  - RPC:   Video stream request handler on port 18086\n\n");
    fprintf(stderr, "Capture modes:\n");
    fprintf(stderr, "  Default: MJPEG capture from camera, TurboJPEG decode for H.264\n");
    fprintf(stderr, "  YUYV (-y): YUYV capture, hardware JPEG encode (lower CPU, lower FPS)\n\n");
    fprintf(stderr, "Usage: %s [options]\n\n", prog);
    fprintf(stderr, "Options:\n");
    fprintf(stderr, "  -d, --device <path>  Camera device (default: %s)\n", DEFAULT_DEVICE);
    fprintf(stderr, "  -o, --h264 <path>    H.264 output file/pipe (default: none)\n");
    fprintf(stderr, "  -w, --width <n>      Width (default: %d)\n", DEFAULT_WIDTH);
    fprintf(stderr, "  -h, --height <n>     Height (default: %d)\n", DEFAULT_HEIGHT);
    fprintf(stderr, "  -f, --fps <n>        Target output fps (default: %d)\n", DEFAULT_MJPEG_TARGET_FPS);
    fprintf(stderr, "  -b, --bitrate <n>    H.264 bitrate in kbps (default: %d)\n", DEFAULT_BITRATE);
    fprintf(stderr, "  -g, --gop <n>        H.264 GOP size (default: 30)\n");
    fprintf(stderr, "  -s, --skip <n>       H.264 skip ratio (default: 2, encode every 2nd frame)\n");
    fprintf(stderr, "  -a, --auto-skip      Enable auto-adjust skip ratio based on CPU\n");
    fprintf(stderr, "  -t, --target-cpu <n> Target max CPU %% for auto-skip (default: 60)\n");
    fprintf(stderr, "  -y, --yuyv           Use YUYV capture mode with hardware JPEG encoding\n");
    fprintf(stderr, "  -j, --jpeg-quality <n> JPEG quality for HW encode (1-99, default: %d)\n", DEFAULT_JPEG_QUALITY);
    fprintf(stderr, "  -n, --no-h264        Start with H.264 encoding disabled\n");
    fprintf(stderr, "  -S, --server         Enable built-in HTTP/MQTT/RPC servers\n");
    fprintf(stderr, "  -N, --no-stdout      Disable stdout output (use with -S)\n");
    fprintf(stderr, "  --mode <mode>        Operating mode: go-klipper (default) or vanilla-klipper\n");
    fprintf(stderr, "                       vanilla-klipper: skip MQTT/RPC (for external Klipper)\n");
    fprintf(stderr, "  --streaming-port <n> MJPEG HTTP server port (default: %d)\n", HTTP_MJPEG_PORT);
    fprintf(stderr, "  --h264-resolution <WxH> H.264 encode resolution (rkmpi mode only, default: camera res)\n");
    fprintf(stderr, "                       Lower resolution reduces TurboJPEG decode CPU usage\n");
    fprintf(stderr, "  --display            Enable display framebuffer capture (server mode)\n");
    fprintf(stderr, "  --display-fps <n>    Display capture FPS (default: %d)\n", DISPLAY_DEFAULT_FPS);
    fprintf(stderr, "  -v, --verbose        Verbose output to stderr\n");
    fprintf(stderr, "  -V, --version        Show version and exit\n");
    fprintf(stderr, "  --help               Show this help\n");
    fprintf(stderr, "\nRuntime Control via %s:\n", CTRL_FILE);
    fprintf(stderr, "  h264=0|1             Enable/disable H.264 encoding\n");
    fprintf(stderr, "  skip=N               Encode every Nth frame (1=all, 2=half, etc.)\n");
    fprintf(stderr, "  auto_skip=0|1        Enable/disable auto-skip based on CPU\n");
    fprintf(stderr, "  target_cpu=N         Target max CPU %% (20-90, default 60)\n");
    fprintf(stderr, "\nExamples:\n");
    fprintf(stderr, "  %s -S -N                      # Server mode only (no stdout)\n", prog);
    fprintf(stderr, "  %s -o /tmp/h264.fifo          # MJPEG capture, H.264 to FIFO\n", prog);
    fprintf(stderr, "  %s -y -o /tmp/h264.fifo       # YUYV capture with HW JPEG encode\n", prog);
    fprintf(stderr, "  %s -y -j 75 -o /tmp/h264.fifo # YUYV with lower JPEG quality\n", prog);
    fprintf(stderr, "  echo 'auto_skip=1' > %s       # Enable auto-skip at runtime\n", CTRL_FILE);
    fprintf(stderr, "\nNotes:\n");
    fprintf(stderr, "  - MJPEG mode: Camera delivers MJPEG, TurboJPEG decodes for H.264\n");
    fprintf(stderr, "  - YUYV mode: Lower FPS (~5fps at 720p) but lower CPU usage\n");
    fprintf(stderr, "  - In YUYV mode, both H.264 and JPEG use hardware encoding\n");
}

int main(int argc, char *argv[]) {
    EncoderConfig cfg = {
        .width = DEFAULT_WIDTH,
        .height = DEFAULT_HEIGHT,
        .fps = DEFAULT_FPS_MJPEG,
        .bitrate = DEFAULT_BITRATE,
        .gop = 30,
        .profile = DEFAULT_PROFILE,
        .use_vbr = 0,
        .mjpeg_stdout = 1,
        .yuyv_mode = 0,
        .jpeg_quality = DEFAULT_JPEG_QUALITY,
        .server_mode = 0,
        .no_stdout = 0,
        .vanilla_klipper = 0,
        .streaming_port = 0,
        .h264_width = 0,
        .h264_height = 0,
        .display_capture = 0,
        .display_fps = DISPLAY_DEFAULT_FPS
    };
    strncpy(cfg.device, DEFAULT_DEVICE, sizeof(cfg.device) - 1);
    cfg.h264_output[0] = '\0';

    static struct option long_options[] = {
        {"device",       required_argument, 0, 'd'},
        {"h264",         required_argument, 0, 'o'},
        {"width",        required_argument, 0, 'w'},
        {"height",       required_argument, 0, 'h'},
        {"fps",          required_argument, 0, 'f'},
        {"bitrate",      required_argument, 0, 'b'},
        {"gop",          required_argument, 0, 'g'},
        {"skip",         required_argument, 0, 's'},
        {"auto-skip",    no_argument,       0, 'a'},
        {"target-cpu",   required_argument, 0, 't'},
        {"yuyv",         no_argument,       0, 'y'},
        {"jpeg-quality", required_argument, 0, 'j'},
        {"no-h264",      no_argument,       0, 'n'},
        {"server",       no_argument,       0, 'S'},
        {"no-stdout",    no_argument,       0, 'N'},
        {"quality",      no_argument,       0, 'q'},
        {"verbose",      no_argument,       0, 'v'},
        {"version",      no_argument,       0, 'V'},
        {"help",         no_argument,       0, 'H'},
        {"mode",         required_argument, 0, 'M'},
        {"streaming-port", required_argument, 0, 'P'},
        {"h264-resolution", required_argument, 0, 'R'},
        {"display",      no_argument,       0, 'D'},
        {"display-fps",  required_argument, 0, 'F'},
        {0, 0, 0, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "d:o:w:h:f:b:g:s:at:yj:nSNqvV", long_options, NULL)) != -1) {
        switch (opt) {
            case 'd': strncpy(cfg.device, optarg, sizeof(cfg.device) - 1); break;
            case 'o': strncpy(cfg.h264_output, optarg, sizeof(cfg.h264_output) - 1); break;
            case 'w': cfg.width = atoi(optarg); break;
            case 'h': cfg.height = atoi(optarg); break;
            case 'f': g_mjpeg_ctrl.target_fps = atoi(optarg); break;
            case 'b': cfg.bitrate = atoi(optarg); break;
            case 'g': cfg.gop = atoi(optarg); break;
            case 's': g_ctrl.skip_ratio = atoi(optarg); break;
            case 'a': g_ctrl.auto_skip = 1; break;
            case 't': g_ctrl.target_cpu = atoi(optarg); break;
            case 'y': cfg.yuyv_mode = 1; break;
            case 'j': cfg.jpeg_quality = atoi(optarg); break;
            case 'n': g_ctrl.h264_enabled = 0; break;
            case 'S': cfg.server_mode = 1; break;
            case 'N': cfg.no_stdout = 1; break;
            case 'q': cfg.use_vbr = 1; break;
            case 'v': g_verbose = 1; break;
            case 'V': print_version(); return 0;
            case 'M':
                /* --mode: go-klipper (default) or vanilla-klipper */
                if (strcmp(optarg, "vanilla-klipper") == 0) {
                    cfg.vanilla_klipper = 1;
                }
                break;
            case 'P': cfg.streaming_port = atoi(optarg); break;
            case 'R':
                /* --h264-resolution: WxH format (e.g., 640x480) */
                if (sscanf(optarg, "%dx%d", &cfg.h264_width, &cfg.h264_height) != 2) {
                    fprintf(stderr, "Invalid resolution format: %s (expected WxH)\n", optarg);
                    return 1;
                }
                break;
            case 'D': cfg.display_capture = 1; break;
            case 'F': cfg.display_fps = atoi(optarg); break;
            case 'H':
            case '?':
                print_usage(argv[0]);
                return (opt == 'H') ? 0 : 1;
        }
    }

    /* If no-stdout is set, disable stdout output */
    if (cfg.no_stdout) {
        cfg.mjpeg_stdout = 0;
    }

    /* Validate skip ratio */
    if (g_ctrl.skip_ratio < 1) g_ctrl.skip_ratio = 1;

    /* Validate MJPEG target fps and calculate interval */
    if (g_mjpeg_ctrl.target_fps < 1) g_mjpeg_ctrl.target_fps = 1;
    if (g_mjpeg_ctrl.target_fps > 30) g_mjpeg_ctrl.target_fps = 30;
    g_mjpeg_ctrl.target_interval = 1000000 / g_mjpeg_ctrl.target_fps;

    /* Request target fps from camera (if it supports it, less skipping needed) */
    cfg.fps = g_mjpeg_ctrl.target_fps;

    /* Validate config */
    if (cfg.width < 160 || cfg.width > 1920 ||
        cfg.height < 120 || cfg.height > 1080) {
        log_error("Invalid resolution: %dx%d\n", cfg.width, cfg.height);
        return 1;
    }
    if (cfg.bitrate < 100 || cfg.bitrate > 20000) {
        log_error("Invalid bitrate: %d kbps\n", cfg.bitrate);
        return 1;
    }
    if (cfg.jpeg_quality < 1 || cfg.jpeg_quality > 99) {
        log_error("Invalid JPEG quality: %d (must be 1-99)\n", cfg.jpeg_quality);
        return 1;
    }

    /* Default H.264 resolution to camera resolution if not specified */
    if (cfg.h264_width == 0 || cfg.h264_height == 0) {
        cfg.h264_width = cfg.width;
        cfg.h264_height = cfg.height;
    }
    /* Validate H.264 resolution */
    if (cfg.h264_width < 160 || cfg.h264_width > 1920 ||
        cfg.h264_height < 120 || cfg.h264_height > 1080) {
        log_error("Invalid H.264 resolution: %dx%d\n", cfg.h264_width, cfg.h264_height);
        return 1;
    }

    /* In YUYV mode, use lower default FPS since YUYV is bandwidth-limited */
    if (cfg.yuyv_mode && g_mjpeg_ctrl.target_fps > DEFAULT_FPS_YUYV) {
        log_info("YUYV mode: clamping target FPS from %d to %d\n",
                 g_mjpeg_ctrl.target_fps, DEFAULT_FPS_YUYV);
        g_mjpeg_ctrl.target_fps = DEFAULT_FPS_YUYV;
        g_mjpeg_ctrl.target_interval = 1000000 / g_mjpeg_ctrl.target_fps;
        cfg.fps = g_mjpeg_ctrl.target_fps;
    }

    /* Check if we have H.264 output configured (or server mode which needs H.264 for FLV) */
    int h264_fd = -1;
    int h264_available = (cfg.h264_output[0] != '\0') || cfg.server_mode;

    /* Only open file if we have a path - server mode sends H.264 via HTTP */
    if (cfg.h264_output[0] != '\0') {
        h264_fd = open(cfg.h264_output, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (h264_fd < 0) {
            /* Try opening as FIFO (non-blocking first to avoid hang) */
            h264_fd = open(cfg.h264_output, O_WRONLY | O_NONBLOCK);
            if (h264_fd >= 0) {
                /* Clear non-blocking for actual writes */
                int flags = fcntl(h264_fd, F_GETFL);
                fcntl(h264_fd, F_SETFL, flags & ~O_NONBLOCK);
            }
        }
        if (h264_fd < 0) {
            log_error("Cannot open H.264 output %s: %s\n", cfg.h264_output, strerror(errno));
            log_error("H.264 encoding will be disabled\n");
            h264_available = 0;
        }
    }

    /* Setup signal handlers */
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGPIPE, SIG_IGN);

    /* Read control file first (Python may have written settings) then write */
    read_ctrl_file();
    write_ctrl_file();

    log_info("Combined MJPEG/H.264 Streamer v%s starting...\n", VERSION);
    log_info("Camera: %s %dx%d (%s mode)\n", cfg.device, cfg.width, cfg.height,
             cfg.yuyv_mode ? "YUYV" : "MJPEG");
    if (cfg.yuyv_mode) {
        log_info("MJPEG output: stdout (HW encode, quality=%d, target %d fps)\n",
                 cfg.jpeg_quality, g_mjpeg_ctrl.target_fps);
    } else {
        log_info("MJPEG output: stdout (pass-through, target %d fps)\n", g_mjpeg_ctrl.target_fps);
    }
    if (h264_available) {
        if (cfg.h264_width != cfg.width || cfg.h264_height != cfg.height) {
            log_info("H.264 output: %s (%s, %dx%d scaled from %dx%d, skip=%d)\n",
                     cfg.h264_output[0] ? cfg.h264_output : "server",
                     g_ctrl.h264_enabled ? "enabled" : "disabled",
                     cfg.h264_width, cfg.h264_height, cfg.width, cfg.height,
                     g_ctrl.skip_ratio);
        } else {
            log_info("H.264 output: %s (%s, skip=%d)\n",
                     cfg.h264_output[0] ? cfg.h264_output : "server",
                     g_ctrl.h264_enabled ? "enabled" : "disabled",
                     g_ctrl.skip_ratio);
        }
    } else {
        log_info("H.264 output: disabled (no output path)\n");
    }

    RK_S32 ret;
    int rkmpi_initialized = 0;
    int venc_initialized = 0;
    int venc_jpeg_initialized = 0;
    int turbojpeg_initialized = 0;
    int frame_buffers_initialized = 0;
    int mjpeg_server_initialized = 0;
    int flv_server_initialized = 0;
    int mqtt_initialized = 0;
    int rpc_initialized = 0;

    /* Initialize frame buffers (needed for server mode) */
    if (cfg.server_mode) {
        if (frame_buffers_init() != 0) {
            log_error("Failed to initialize frame buffers\n");
            if (h264_fd >= 0) close(h264_fd);
            return 1;
        }
        frame_buffers_initialized = 1;
        log_info("Frame buffers initialized\n");
    }

    /* Initialize RKMPI system (needed for H.264 encoding and YUYV JPEG encoding) */
    int need_rkmpi = h264_available || cfg.yuyv_mode || cfg.server_mode;
    if (need_rkmpi) {
        ret = RK_MPI_SYS_Init();
        if (ret != RK_SUCCESS) {
            log_error("RK_MPI_SYS_Init failed: 0x%x\n", ret);
            if (cfg.yuyv_mode) {
                log_error("YUYV mode requires RKMPI, aborting\n");
                if (h264_fd >= 0) close(h264_fd);
                return 1;
            }
            log_error("H.264 encoding will be disabled\n");
            h264_available = 0;
        } else {
            rkmpi_initialized = 1;
            log_info("RKMPI system initialized\n");
        }
    }

    /* Initialize TurboJPEG for H.264 encoding (JPEG decode) - NOT needed in YUYV mode */
    if (h264_available && !cfg.yuyv_mode) {
        if (init_turbojpeg_decoder() != 0) {
            log_error("TurboJPEG init failed, H.264 disabled\n");
            h264_available = 0;
        } else {
            turbojpeg_initialized = 1;
        }
    }

    /* Initialize V4L2 camera */
    int v4l2_fd;
    V4L2Buffer *v4l2_buffers;
    int use_mjpeg_capture = cfg.yuyv_mode ? 0 : 1;  /* YUYV mode uses YUYV capture */
    int buffer_count = v4l2_init(cfg.device, cfg.width, cfg.height, cfg.fps,
                                  use_mjpeg_capture, &v4l2_fd, &v4l2_buffers);
    if (buffer_count < 0) {
        if (turbojpeg_initialized) cleanup_turbojpeg_decoder();
        if (rkmpi_initialized) RK_MPI_SYS_Exit();
        if (h264_fd >= 0) close(h264_fd);
        return 1;
    }

    /* Initialize VENC for H.264 encoding (also needed in server mode for FLV) */
    if (h264_available || cfg.server_mode) {
        if (init_venc(&cfg) != 0) {
            log_error("VENC H.264 init failed, H.264 disabled\n");
            h264_available = 0;
        } else {
            venc_initialized = 1;
        }
    }

    /* Initialize VENC for JPEG encoding (YUYV mode only) */
    if (cfg.yuyv_mode && rkmpi_initialized) {
        if (init_venc_jpeg(&cfg) != 0) {
            log_error("VENC JPEG init failed\n");
            if (venc_initialized) cleanup_venc();
            v4l2_stop(v4l2_fd, v4l2_buffers, buffer_count);
            RK_MPI_SYS_Exit();
            if (h264_fd >= 0) close(h264_fd);
            return 1;
        }
        venc_jpeg_initialized = 1;
    }

    if (!h264_available && h264_fd >= 0) {
        close(h264_fd);
        h264_fd = -1;
    }

    if (h264_available && !venc_initialized) {
        if (venc_jpeg_initialized) cleanup_venc_jpeg();
        if (turbojpeg_initialized) cleanup_turbojpeg_decoder();
        v4l2_stop(v4l2_fd, v4l2_buffers, buffer_count);
        RK_MPI_SYS_Exit();
        return 1;
    }

    /* Calculate NV12 buffer size for H.264 encoding (use h264 dimensions) */
    int h264_w = cfg.h264_width ? cfg.h264_width : cfg.width;
    int h264_h = cfg.h264_height ? cfg.h264_height : cfg.height;
    size_t nv12_size = h264_w * h264_h * 3 / 2;

    /* Allocate DMA buffer for H.264 encoding (JPEG decode output) */
    MB_BLK mb_blk = MB_INVALID_HANDLE;
    void *mb_vaddr = NULL;
    int mb_cacheable = 0;

    /* Allocate DMA buffer for VENC (needed for H.264 encoding and YUYV mode) */
    int need_dma_buffer = h264_available || cfg.yuyv_mode;
    if (need_dma_buffer) {
        ret = RK_MPI_MMZ_Alloc(&mb_blk, nv12_size, RK_MMZ_ALLOC_CACHEABLE);
        if (ret != RK_SUCCESS || mb_blk == MB_INVALID_HANDLE) {
            log_error("RK_MPI_MMZ_Alloc failed (ret=0x%x), trying uncacheable\n", ret);
            ret = RK_MPI_MMZ_Alloc(&mb_blk, nv12_size, RK_MMZ_ALLOC_UNCACHEABLE);
            if (ret != RK_SUCCESS || mb_blk == MB_INVALID_HANDLE) {
                log_error("RK_MPI_MMZ_Alloc failed completely: 0x%x\n", ret);
                if (venc_jpeg_initialized) cleanup_venc_jpeg();
                if (venc_initialized) cleanup_venc();
                v4l2_stop(v4l2_fd, v4l2_buffers, buffer_count);
                RK_MPI_SYS_Exit();
                return 1;
            }
        }
        mb_vaddr = RK_MPI_MMZ_Handle2VirAddr(mb_blk);
        mb_cacheable = RK_MPI_MMZ_IsCacheable(mb_blk);
        log_info("Allocated DMA buffer: %zu bytes at %p (cacheable=%d)\n", nv12_size, mb_vaddr, mb_cacheable);
    }

    /* Allocate stream pack for H.264 encoder output */
    VENC_STREAM_S stStream;
    memset(&stStream, 0, sizeof(stStream));
    if (h264_available || cfg.server_mode) {
        stStream.pstPack = (VENC_PACK_S *)malloc(sizeof(VENC_PACK_S));
        if (!stStream.pstPack) {
            log_error("Failed to allocate H.264 stream pack\n");
            if (mb_blk != MB_INVALID_HANDLE) RK_MPI_MMZ_Free(mb_blk);
            if (venc_jpeg_initialized) cleanup_venc_jpeg();
            if (venc_initialized) cleanup_venc();
            if (turbojpeg_initialized) cleanup_turbojpeg_decoder();
            v4l2_stop(v4l2_fd, v4l2_buffers, buffer_count);
            if (rkmpi_initialized) RK_MPI_SYS_Exit();
            if (h264_fd >= 0) close(h264_fd);
            return 1;
        }
    }

    /* Allocate stream pack for JPEG encoder output (YUYV mode) */
    VENC_STREAM_S stJpegStream;
    memset(&stJpegStream, 0, sizeof(stJpegStream));
    if (cfg.yuyv_mode) {
        stJpegStream.pstPack = (VENC_PACK_S *)malloc(sizeof(VENC_PACK_S));
        if (!stJpegStream.pstPack) {
            log_error("Failed to allocate JPEG stream pack\n");
            if (stStream.pstPack) free(stStream.pstPack);
            if (mb_blk != MB_INVALID_HANDLE) RK_MPI_MMZ_Free(mb_blk);
            if (venc_jpeg_initialized) cleanup_venc_jpeg();
            if (venc_initialized) cleanup_venc();
            v4l2_stop(v4l2_fd, v4l2_buffers, buffer_count);
            if (rkmpi_initialized) RK_MPI_SYS_Exit();
            if (h264_fd >= 0) close(h264_fd);
            return 1;
        }
    }

    RK_U64 mjpeg_frame_count = 0;
    RK_U64 h264_frame_count = 0;
    RK_U64 captured_count = 0;
    RK_U64 processed_count = 0;  /* Frames after MJPEG rate control (for H.264 skip) */
    RK_U64 start_time = get_timestamp_us();
    RK_U64 last_stats_time = start_time;
    RK_U64 last_ctrl_check = 0;
    RK_U64 mjpeg_bytes = 0;
    RK_U64 h264_bytes = 0;

    /* Start servers if in server mode */
    if (cfg.server_mode) {
        log_info("Operating mode: %s\n", cfg.vanilla_klipper ? "vanilla-klipper" : "go-klipper");
        log_info("Starting built-in servers...\n");

        /* Start MJPEG HTTP server */
        int mjpeg_port = cfg.streaming_port > 0 ? cfg.streaming_port : HTTP_MJPEG_PORT;
        if (mjpeg_server_start(cfg.streaming_port) == 0) {
            mjpeg_server_initialized = 1;
            log_info("  MJPEG server: http://0.0.0.0:%d/stream\n", mjpeg_port);
        } else {
            log_error("  MJPEG server: failed to start\n");
        }

        /* Start FLV HTTP server (use h264 dimensions for the stream) */
        if (flv_server_start(h264_w, h264_h, g_mjpeg_ctrl.target_fps) == 0) {
            flv_server_initialized = 1;
            log_info("  FLV server: http://0.0.0.0:%d/flv\n", HTTP_FLV_PORT);
        } else {
            log_error("  FLV server: failed to start\n");
        }

        /* Start MQTT/RPC responders (go-klipper mode only) */
        if (!cfg.vanilla_klipper) {
            /* Start MQTT video responder */
            if (mqtt_client_start() == 0) {
                mqtt_initialized = 1;
                log_info("  MQTT responder: localhost:9883 (TLS)\n");
            } else {
                log_error("  MQTT responder: failed to start\n");
            }

            /* Start RPC video responder */
            if (rpc_client_start() == 0) {
                rpc_initialized = 1;
                log_info("  RPC responder: localhost:18086\n");
            } else {
                log_error("  RPC responder: failed to start\n");
            }
        } else {
            log_info("  MQTT/RPC: disabled (vanilla-klipper mode)\n");
        }

        /* Start display capture if enabled */
        if (cfg.display_capture) {
            if (display_capture_start(cfg.display_fps) == 0) {
                log_info("  Display capture: http://0.0.0.0:%d/display (%d fps)\n",
                         mjpeg_port, cfg.display_fps);
            } else {
                log_error("  Display capture: failed to start\n");
            }
        }
    }

    log_info("Starting capture loop...\n");
    if (cfg.mjpeg_stdout) {
        log_info("  MJPEG: stdout (multipart)\n");
    }
    if (h264_available) {
        log_info("  H.264: %s (skip=%d, %s)\n", cfg.h264_output, g_ctrl.skip_ratio,
                 g_ctrl.h264_enabled ? "enabled" : "disabled");
    }

    while (g_running) {
        TIMING_START(total_frame);

        /* Periodically check control file */
        if (captured_count - last_ctrl_check >= CTRL_CHECK_INTERVAL) {
            read_ctrl_file();
            last_ctrl_check = captured_count;
        }

        /*
         * MJPEG mode pre-DQBUF rate control (adaptive)
         * Only sleep if camera delivers faster than target fps.
         * If camera is slower (e.g., 10fps vs 30fps advertised), process every frame.
         */
        if (!cfg.yuyv_mode) {
            int mjpeg_clients = cfg.server_mode ? mjpeg_server_client_count() : 0;
            int flv_clients = cfg.server_mode ? flv_server_client_count() : 0;
            int total_clients = mjpeg_clients + flv_clients;

            /* Idle mode - no clients, sleep longer */
            if (cfg.server_mode && !cfg.mjpeg_stdout && total_clients == 0) {
                usleep(500000);  /* 500ms sleep when fully idle */
                continue;
            }

            /* Only rate limit if camera is faster than target (adaptive) */
            if (g_mjpeg_ctrl.rate_limit_needed) {
                RK_U64 now = get_timestamp_us();
                RK_U64 next_frame_time = g_mjpeg_ctrl.last_output_time + g_mjpeg_ctrl.target_interval;
                if (now < next_frame_time) {
                    RK_U64 sleep_time = next_frame_time - now;
                    usleep(sleep_time);  /* Sleep without touching V4L2 */
                }
            }
        }

        /* Dequeue frame from V4L2 */
        struct v4l2_buffer buf = {0};
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;

        TIMING_START(v4l2_dqbuf);
        if (ioctl(v4l2_fd, VIDIOC_DQBUF, &buf) < 0) {
            if (errno == EAGAIN) {
                usleep(1000);
                continue;
            }
            log_error("VIDIOC_DQBUF failed: %s\n", strerror(errno));
            break;
        }
        TIMING_END(v4l2_dqbuf);

        captured_count++;
        uint8_t *capture_data = v4l2_buffers[buf.index].start;
        size_t capture_len = buf.bytesused;

        /*
         * MJPEG mode: Detect actual camera frame rate (adaptive)
         * Measure inter-frame timing to determine if camera is faster/slower than target
         */
        if (!cfg.yuyv_mode && !g_mjpeg_ctrl.camera_fps_detected) {
            RK_U64 now = get_timestamp_us();
            if (g_mjpeg_ctrl.last_dqbuf_time > 0) {
                RK_U64 interval = now - g_mjpeg_ctrl.last_dqbuf_time;
                /* Use exponential moving average for stability */
                if (g_mjpeg_ctrl.camera_interval == 0) {
                    g_mjpeg_ctrl.camera_interval = interval;
                } else {
                    g_mjpeg_ctrl.camera_interval = (g_mjpeg_ctrl.camera_interval * 3 + interval) / 4;
                }
                /* After 30 frames (1-3 seconds), finalize detection */
                if (captured_count >= 30) {
                    int camera_fps = 1000000 / g_mjpeg_ctrl.camera_interval;
                    g_mjpeg_ctrl.camera_fps_detected = 1;
                    /* Only rate limit if camera is significantly faster than target */
                    g_mjpeg_ctrl.rate_limit_needed = (camera_fps > g_mjpeg_ctrl.target_fps + 2);
                    log_info("Camera rate detected: %d fps (interval %llu us), target %d fps, rate limiting: %s\n",
                             camera_fps, g_mjpeg_ctrl.camera_interval, g_mjpeg_ctrl.target_fps,
                             g_mjpeg_ctrl.rate_limit_needed ? "enabled" : "disabled");
                }
            }
            g_mjpeg_ctrl.last_dqbuf_time = now;
        }

        /*
         * Rate control for both modes to reduce CPU usage
         * YUYV mode: YUYV→NV12 conversion is CPU-intensive
         * MJPEG mode: TurboJPEG decode is CPU-intensive
         */
        if (cfg.yuyv_mode) {
            /* Apply rate control to reduce YUYV→NV12 conversion overhead */
            int process_frame = mjpeg_rate_control(captured_count);
            if (!process_frame) {
                /* Skip this frame - just requeue buffer */
                ioctl(v4l2_fd, VIDIOC_QBUF, &buf);
                /* Small sleep to prevent busy-loop when camera fps > target fps */
                usleep(5000);  /* 5ms */
                continue;
            }
            processed_count++;
            /*
             * YUYV MODE: Hardware JPEG + H.264 encoding from single NV12 buffer
             */
            uint8_t *nv12_y = (uint8_t *)mb_vaddr;
            uint8_t *nv12_uv = nv12_y + cfg.width * cfg.height;

            /* Convert YUYV to NV12 (simple CPU conversion, ~5% CPU at 720p) */
            TIMING_START(yuyv_to_nv12);
            yuyv_to_nv12(capture_data, nv12_y, nv12_uv, cfg.width, cfg.height);

            /* Flush cache for DMA (if cacheable) */
            if (mb_cacheable) {
                RK_MPI_MMZ_FlushCacheEnd(mb_blk, 0, nv12_size, RK_MMZ_SYNC_WRITEONLY);
            }
            TIMING_END(yuyv_to_nv12);

            /* Prepare frame structure (shared by both encoders) */
            VIDEO_FRAME_INFO_S stEncFrame;
            memset(&stEncFrame, 0, sizeof(stEncFrame));
            stEncFrame.stVFrame.u32Width = cfg.width;
            stEncFrame.stVFrame.u32Height = cfg.height;
            stEncFrame.stVFrame.u32VirWidth = cfg.width;
            stEncFrame.stVFrame.u32VirHeight = cfg.height;
            stEncFrame.stVFrame.enPixelFormat = RK_FMT_YUV420SP;
            stEncFrame.stVFrame.pMbBlk = mb_blk;
            stEncFrame.stVFrame.u64PTS = get_timestamp_us();

            /*
             * Encode to JPEG (hardware) and output to stdout/frame buffer
             */
            if (cfg.mjpeg_stdout || cfg.server_mode) {
                TIMING_START(venc_jpeg);
                ret = RK_MPI_VENC_SendFrame(VENC_CHN_JPEG, &stEncFrame, 1000);
                if (ret == RK_SUCCESS) {
                    ret = RK_MPI_VENC_GetStream(VENC_CHN_JPEG, &stJpegStream, 1000);
                    if (ret == RK_SUCCESS) {
                        TIMING_END(venc_jpeg);
                        void *jpeg_data = RK_MPI_MB_Handle2VirAddr(stJpegStream.pstPack->pMbBlk);
                        RK_U32 jpeg_len = stJpegStream.pstPack->u32Len;

                        if (jpeg_data && jpeg_len > 0) {
                            /* Write to frame buffer for HTTP servers */
                            TIMING_START(frame_buffer);
                            if (cfg.server_mode && frame_buffers_initialized) {
                                frame_buffer_write(&g_jpeg_buffer, jpeg_data, jpeg_len,
                                                   get_timestamp_us(), 0);
                            }
                            TIMING_END(frame_buffer);

                            /* Write to stdout */
                            if (cfg.mjpeg_stdout) {
                                /* Multipart header */
                                char header[128];
                                int hlen = snprintf(header, sizeof(header),
                                    "--%s\r\nContent-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n",
                                    MJPEG_BOUNDARY, jpeg_len);

                                ssize_t written = write(STDOUT_FILENO, header, hlen);
                                if (written > 0) {
                                    written = write(STDOUT_FILENO, jpeg_data, jpeg_len);
                                }
                                if (written > 0) {
                                    written = write(STDOUT_FILENO, "\r\n", 2);
                                }

                                if (written < 0) {
                                    if (errno == EPIPE) {
                                        log_info("MJPEG pipe closed, stopping...\n");
                                        g_running = 0;
                                    }
                                }
                            }
                            mjpeg_frame_count++;
                            mjpeg_bytes += jpeg_len;
                        }
                        RK_MPI_VENC_ReleaseStream(VENC_CHN_JPEG, &stJpegStream);
                    }
                }
            }

            /*
             * Encode to H.264 (hardware) - always at full speed in YUYV mode
             * No skip ratio needed since HW encoding has minimal CPU impact
             */
            if (h264_available || cfg.server_mode) {
                TIMING_START(venc_h264);
                ret = RK_MPI_VENC_SendFrame(VENC_CHN_H264, &stEncFrame, 1000);
                if (ret == RK_SUCCESS) {
                    ret = RK_MPI_VENC_GetStream(VENC_CHN_H264, &stStream, 1000);
                    if (ret == RK_SUCCESS) {
                        TIMING_END(venc_h264);
                        void *pData = RK_MPI_MB_Handle2VirAddr(stStream.pstPack->pMbBlk);
                        RK_U32 len = stStream.pstPack->u32Len;

                        if (pData && len > 0) {
                            /* Check if this is a keyframe (IDR) */
                            int is_keyframe = 0;
                            if (len >= 5) {
                                /* Look for NAL type in first NAL unit */
                                const uint8_t *p = (const uint8_t *)pData;
                                for (size_t i = 0; i < len - 4; i++) {
                                    if (p[i] == 0 && p[i+1] == 0 && p[i+2] == 0 && p[i+3] == 1) {
                                        int nal_type = p[i+4] & 0x1F;
                                        if (nal_type == 5) is_keyframe = 1;
                                        break;
                                    }
                                }
                            }

                            /* Write to frame buffer for FLV server */
                            if (cfg.server_mode && frame_buffers_initialized) {
                                frame_buffer_write(&g_h264_buffer, pData, len,
                                                   get_timestamp_us(), is_keyframe);
                            }

                            /* Write to H.264 output file/pipe */
                            if (h264_fd >= 0) {
                                ssize_t written = write(h264_fd, pData, len);
                                if (written > 0) {
                                    h264_frame_count++;
                                    h264_bytes += len;
                                }
                            } else if (cfg.server_mode) {
                                h264_frame_count++;
                                h264_bytes += len;
                            }
                        }
                        RK_MPI_VENC_ReleaseStream(VENC_CHN_H264, &stStream);
                    }
                }
            }

#ifdef ENCODER_TIMING
            TIMING_END(total_frame);
            g_timing.count++;
            TIMING_LOG();
#endif
        } else {
            /*
             * MJPEG MODE: Process frame (rate control already done before DQBUF)
             */
            int mjpeg_clients = cfg.server_mode ? mjpeg_server_client_count() : 0;

            /* Update rate control timestamp (sleep was done before DQBUF) */
            g_mjpeg_ctrl.last_output_time = get_timestamp_us();
            g_mjpeg_ctrl.frames_out++;
            processed_count++;

            /*
             * MJPEG MODE: Pass-through JPEG to stdout, TurboJPEG decode for H.264
             */
            uint8_t *jpeg_data = capture_data;
            size_t jpeg_len = capture_len;

            /* Write JPEG to frame buffer for HTTP servers (only if clients connected) */
            TIMING_START(frame_buffer);
            if (cfg.server_mode && frame_buffers_initialized && mjpeg_clients > 0) {
                frame_buffer_write(&g_jpeg_buffer, jpeg_data, jpeg_len,
                                   get_timestamp_us(), 0);
            }
            TIMING_END(frame_buffer);

            /* Output MJPEG to stdout (multipart format for HTTP streaming) */
            if (cfg.mjpeg_stdout) {
                /* Multipart header */
                char header[128];
                int hlen = snprintf(header, sizeof(header),
                    "--%s\r\nContent-Type: image/jpeg\r\nContent-Length: %zu\r\n\r\n",
                    MJPEG_BOUNDARY, jpeg_len);

                ssize_t written = write(STDOUT_FILENO, header, hlen);
                if (written > 0) {
                    written = write(STDOUT_FILENO, jpeg_data, jpeg_len);
                }
                if (written > 0) {
                    written = write(STDOUT_FILENO, "\r\n", 2);
                }

                if (written < 0) {
                    if (errno == EPIPE) {
                        log_info("MJPEG pipe closed, stopping...\n");
                        g_running = 0;
                    }
                }
            }
            mjpeg_frame_count++;
            mjpeg_bytes += jpeg_len;

            /*
             * Optionally encode to H.264 (based on runtime control)
             * Uses processed_count (post rate-control) for skip ratio
             *
             * Client-based idle logic (server mode):
             * - Skip expensive TurboJPEG decode when no FLV clients connected
             * - When clients connect, ramp up gradually to prevent CPU spikes
             */
            int do_h264 = 0;

            if (cfg.server_mode) {
                /* Server mode: only encode H.264 when FLV clients are connected */
                int flv_clients = flv_server_client_count();
                if (flv_clients > 0) {
                    /* Apply ramp-up logic to prevent CPU spikes */
                    int ramp_ok = client_activity_check(0, flv_clients, 1);
                    do_h264 = ramp_ok && g_ctrl.h264_enabled &&
                              ((processed_count % g_ctrl.skip_ratio) == 1 || g_ctrl.skip_ratio == 1);
                }
                /* else: no FLV clients, skip H.264 entirely (idle mode) */
            } else {
                /* Non-server mode: encode H.264 for file output */
                do_h264 = h264_available && g_ctrl.h264_enabled &&
                          ((processed_count % g_ctrl.skip_ratio) == 1 || g_ctrl.skip_ratio == 1);
            }

            if (do_h264) {
                uint8_t *nv12_y = (uint8_t *)mb_vaddr;
                uint8_t *nv12_uv = nv12_y + h264_w * h264_h;

                /* Decode JPEG to NV12 using TurboJPEG (with optional scaling) */
                TIMING_START(jpeg_decode);
                int decode_ok = decode_jpeg_to_nv12(jpeg_data, jpeg_len, nv12_y, nv12_uv,
                                         cfg.width, cfg.height,
                                         h264_w, h264_h) == 0;
                TIMING_END(jpeg_decode);

                if (decode_ok) {
                    /* Flush cache for DMA (if cacheable) */
                    if (mb_cacheable) {
                        RK_MPI_MMZ_FlushCacheEnd(mb_blk, 0, nv12_size, RK_MMZ_SYNC_WRITEONLY);
                    }

                    /* Prepare frame for encoder (use h264 dimensions) */
                    VIDEO_FRAME_INFO_S stEncFrame;
                    memset(&stEncFrame, 0, sizeof(stEncFrame));
                    stEncFrame.stVFrame.u32Width = h264_w;
                    stEncFrame.stVFrame.u32Height = h264_h;
                    stEncFrame.stVFrame.u32VirWidth = h264_w;
                    stEncFrame.stVFrame.u32VirHeight = h264_h;
                    stEncFrame.stVFrame.enPixelFormat = RK_FMT_YUV420SP;
                    stEncFrame.stVFrame.pMbBlk = mb_blk;
                    stEncFrame.stVFrame.u64PTS = get_timestamp_us();

                    /* Send frame to encoder */
                    TIMING_START(venc_h264);
                    ret = RK_MPI_VENC_SendFrame(VENC_CHN_H264, &stEncFrame, 1000);
                    if (ret == RK_SUCCESS) {
                        /* Get encoded stream */
                        ret = RK_MPI_VENC_GetStream(VENC_CHN_H264, &stStream, 1000);
                        if (ret == RK_SUCCESS) {
                            TIMING_END(venc_h264);
                            void *pData = RK_MPI_MB_Handle2VirAddr(stStream.pstPack->pMbBlk);
                            RK_U32 len = stStream.pstPack->u32Len;

                            if (pData && len > 0) {
                                /* Check if this is a keyframe (IDR) */
                                int is_keyframe = 0;
                                if (len >= 5) {
                                    const uint8_t *p = (const uint8_t *)pData;
                                    for (size_t i = 0; i < len - 4; i++) {
                                        if (p[i] == 0 && p[i+1] == 0 && p[i+2] == 0 && p[i+3] == 1) {
                                            int nal_type = p[i+4] & 0x1F;
                                            if (nal_type == 5) is_keyframe = 1;
                                            break;
                                        }
                                    }
                                }

                                /* Write to frame buffer for FLV server */
                                if (cfg.server_mode && frame_buffers_initialized) {
                                    frame_buffer_write(&g_h264_buffer, pData, len,
                                                       get_timestamp_us(), is_keyframe);
                                }

                                /* Write to H.264 output file/pipe */
                                if (h264_fd >= 0) {
                                    ssize_t written = write(h264_fd, pData, len);
                                    if (written > 0) {
                                        h264_frame_count++;
                                        h264_bytes += len;
                                    }
                                } else if (cfg.server_mode) {
                                    h264_frame_count++;
                                    h264_bytes += len;
                                }
                            }
                            RK_MPI_VENC_ReleaseStream(VENC_CHN_H264, &stStream);
                        }
                    }
                }
            }

#ifdef ENCODER_TIMING
            TIMING_END(total_frame);
            g_timing.count++;
            TIMING_LOG();
#endif
        }

        /* Requeue V4L2 buffer */
        ioctl(v4l2_fd, VIDIOC_QBUF, &buf);

        /* Auto-adjust skip ratio based on CPU (every 500ms for faster response) */
        RK_U64 now = get_timestamp_us();
        static RK_U64 last_auto_skip_time = 0;
        if (g_ctrl.auto_skip && (now - last_auto_skip_time) >= 500000) {
            auto_adjust_skip_ratio();
            last_auto_skip_time = now;
        }

        /* Update and write stats every 1 second (for Python to read) */
        static RK_U64 last_stats_write = 0;
        static RK_U64 prev_mjpeg_count = 0;
        static RK_U64 prev_h264_count = 0;
        if ((now - last_stats_write) >= 1000000) {
            double elapsed_sec = (now - last_stats_write) / 1000000.0;
            if (elapsed_sec > 0) {
                /* Calculate FPS over this interval (not total average) */
                g_stats.mjpeg_fps = (mjpeg_frame_count - prev_mjpeg_count) / elapsed_sec;
                g_stats.h264_fps = (h264_frame_count - prev_h264_count) / elapsed_sec;
            }
            prev_mjpeg_count = mjpeg_frame_count;
            prev_h264_count = h264_frame_count;
            /* Get client counts from HTTP servers */
            if (cfg.server_mode) {
                g_stats.mjpeg_clients = mjpeg_server_client_count();
                g_stats.flv_clients = flv_server_client_count();
            }
            /* Read before write to preserve Python-set values (display settings) */
            read_ctrl_file();
            write_ctrl_file();
            last_stats_write = now;
        }

        /* Print stats every 5 seconds */
        if (g_verbose && (now - last_stats_time) >= 5000000) {
            double elapsed = (now - start_time) / 1000000.0;
            double mjpeg_fps = mjpeg_frame_count / elapsed;
            double h264_fps = h264_frame_count / elapsed;
            log_info("Stats: MJPEG=%llu (%.1f fps), H.264=%llu (%.1f fps, %s skip=%d%s)\n",
                     (unsigned long long)mjpeg_frame_count, mjpeg_fps,
                     (unsigned long long)h264_frame_count, h264_fps,
                     g_ctrl.h264_enabled ? "on" : "off", g_ctrl.skip_ratio,
                     g_ctrl.auto_skip ? " auto" : "");
            last_stats_time = now;
        }
    }

    /* Final stats */
    RK_U64 end_time = get_timestamp_us();
    double elapsed = (end_time - start_time) / 1000000.0;
    if (elapsed > 0) {
        double mjpeg_fps = mjpeg_frame_count / elapsed;
        double h264_fps = h264_frame_count / elapsed;
        log_info("Final: MJPEG=%llu (%.1f fps), H.264=%llu (%.1f fps), time=%.1fs\n",
                 (unsigned long long)mjpeg_frame_count, mjpeg_fps,
                 (unsigned long long)h264_frame_count, h264_fps, elapsed);
    }

    /* Stop servers first (they reference frame buffers) */
    if (rpc_initialized) {
        log_info("Stopping RPC responder...\n");
        rpc_client_stop();
    }
    if (mqtt_initialized) {
        log_info("Stopping MQTT responder...\n");
        mqtt_client_stop();
    }
    if (flv_server_initialized) {
        log_info("Stopping FLV server...\n");
        flv_server_stop();
    }
    if (mjpeg_server_initialized) {
        log_info("Stopping MJPEG server...\n");
        mjpeg_server_stop();
    }
    if (display_capture_is_running()) {
        log_info("Stopping display capture...\n");
        display_capture_stop();
    }

    /* Cleanup */
    if (stStream.pstPack) free(stStream.pstPack);
    if (stJpegStream.pstPack) free(stJpegStream.pstPack);
    if (h264_fd >= 0) close(h264_fd);
    if (turbojpeg_initialized) cleanup_turbojpeg_decoder();
    if (mb_blk != MB_INVALID_HANDLE) RK_MPI_MMZ_Free(mb_blk);
    if (venc_jpeg_initialized) cleanup_venc_jpeg();
    if (venc_initialized) cleanup_venc();
    v4l2_stop(v4l2_fd, v4l2_buffers, buffer_count);
    if (rkmpi_initialized) RK_MPI_SYS_Exit();

    /* Cleanup frame buffers (after servers are stopped) */
    if (frame_buffers_initialized) {
        frame_buffers_cleanup();
    }

    /* Remove control file */
    unlink(CTRL_FILE);

    log_info("Streamer stopped\n");
    return 0;
}
