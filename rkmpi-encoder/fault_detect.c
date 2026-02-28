/*
 * Fault Detection Module
 *
 * Real-time 3D print fault detection using RKNN NPU.
 * Ported from BigEdge-FDM-Models detect tool:
 *   - rknn_model.c: RKNN wrapper (adapted for dlopen)
 *   - preprocess.c: JPEG decode + resize/crop/grayscale
 *   - detect.c: CNN/ProtoNet/Multiclass inference + strategy combining
 *
 * RKNN runtime loaded via dlopen() for printers without NPU.
 */

/* Suppress snprintf truncation warnings — all paths are safely bounded */
#pragma GCC diagnostic ignored "-Wformat-truncation"

#include "fault_detect.h"
#include "timelapse.h"
#include "mqtt_client.h"
#include "cJSON.h"
#include "rknn/rknn_api.h"
#include <turbojpeg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <pthread.h>
#include <unistd.h>
#include <dlfcn.h>
#include <dirent.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <errno.h>

/* Logging macros (match rkmpi_enc.c style) */
#define fd_log(fmt, ...) fprintf(stderr, "[FD] " fmt, ##__VA_ARGS__)
#define fd_err(fmt, ...) fprintf(stderr, "[FD] ERROR: " fmt, ##__VA_ARGS__)

/* Forward declarations (defined in Helpers section) */
static double fd_get_time_ms(void);
static fd_mask196_t fd_get_mask_for_z(const fd_config_t *cfg, float z);

/* Forward declarations (defined in Prototype Management section) */
static void fd_do_proto_computation(void);

/* ============================================================================
 * Buzzer (PWM piezo)
 * ============================================================================ */

#define FD_BUZZER_PWM_DIR   "/sys/class/pwm/pwmchip0/pwm0"
#define FD_BUZZER_PWM_PATH  FD_BUZZER_PWM_DIR "/enable"
#define FD_BEEP_COOLDOWN_MS 15000

/* PWM tone: ~4kHz, 50% duty cycle */
#define FD_BUZZER_PERIOD    "250000"
#define FD_BUZZER_DUTY      "125000"

static int g_buzzer_fd = -1;
static uint64_t g_last_beep_time = 0;

static void fd_write_sysfs(const char *path, const char *val)
{
    int fd = open(path, O_WRONLY);
    if (fd >= 0) {
        write(fd, val, strlen(val));
        close(fd);
    }
}

static void fd_buzzer_init(void)
{
    /* Configure PWM tone before opening enable file */
    fd_write_sysfs(FD_BUZZER_PWM_DIR "/period", FD_BUZZER_PERIOD);
    fd_write_sysfs(FD_BUZZER_PWM_DIR "/duty_cycle", FD_BUZZER_DUTY);

    g_buzzer_fd = open(FD_BUZZER_PWM_PATH, O_WRONLY);
    if (g_buzzer_fd < 0)
        fd_log("Buzzer: cannot open %s: %s\n", FD_BUZZER_PWM_PATH, strerror(errno));
    else
        fd_log("Buzzer: ready (period=%s duty=%s)\n", FD_BUZZER_PERIOD, FD_BUZZER_DUTY);
}

static void fd_buzzer_cleanup(void)
{
    if (g_buzzer_fd >= 0) {
        close(g_buzzer_fd);
        g_buzzer_fd = -1;
    }
}

static void fd_buzz(int ms)
{
    if (g_buzzer_fd < 0) return;
    lseek(g_buzzer_fd, 0, SEEK_SET);
    write(g_buzzer_fd, "1", 1);
    usleep(ms * 1000);
    lseek(g_buzzer_fd, 0, SEEK_SET);
    write(g_buzzer_fd, "0", 1);
}

static void fd_play_pattern(int pattern)
{
    if (pattern <= 0 || g_buzzer_fd < 0) return;

    /* Cooldown check */
    uint64_t now_ms = (uint64_t)(fd_get_time_ms());
    if (g_last_beep_time > 0 && (now_ms - g_last_beep_time) < FD_BEEP_COOLDOWN_MS)
        return;
    g_last_beep_time = now_ms;

    switch (pattern) {
    case 1: /* Short */
        fd_buzz(200);
        break;
    case 2: /* 2 Short */
        fd_buzz(200); usleep(150000); fd_buzz(200);
        break;
    case 3: /* 3 Short */
        fd_buzz(200); usleep(150000); fd_buzz(200); usleep(150000); fd_buzz(200);
        break;
    case 4: /* 2 Short + Long */
        fd_buzz(200); usleep(150000); fd_buzz(200); usleep(150000); fd_buzz(600);
        break;
    case 5: /* SOS: ···−−−··· */
        for (int i = 0; i < 3; i++) { fd_buzz(100); usleep(100000); }
        usleep(200000);
        for (int i = 0; i < 3; i++) { fd_buzz(300); usleep(100000); }
        usleep(200000);
        for (int i = 0; i < 3; i++) { fd_buzz(100); usleep(100000); }
        break;
    }
}

/* ============================================================================
 * RKNN dlopen function pointers
 * ============================================================================ */

/* RKNN library search paths (bundled with app > fault_detect dir > system) */
#define RKNN_LIB_NAME     "librknnmrt.so"
#define RKNN_LIB_PATH_FD  "/useremain/home/rinkhals/fault_detect/" RKNN_LIB_NAME
#define RKNN_LIB_PATH_SYS "/oem/usr/lib/" RKNN_LIB_NAME

typedef int (*fn_rknn_init)(rknn_context *, void *, uint32_t, uint32_t,
                            rknn_init_extend *);
typedef int (*fn_rknn_query)(rknn_context, rknn_query_cmd, void *, uint32_t);
typedef rknn_tensor_mem *(*fn_rknn_create_mem)(rknn_context, uint32_t);
typedef int (*fn_rknn_set_io_mem)(rknn_context, rknn_tensor_mem *,
                                  rknn_tensor_attr *);
typedef int (*fn_rknn_run)(rknn_context, rknn_run_extend *);
typedef int (*fn_rknn_destroy_mem)(rknn_context, rknn_tensor_mem *);
typedef int (*fn_rknn_destroy)(rknn_context);

static struct {
    void *handle;
    fn_rknn_init init;
    fn_rknn_query query;
    fn_rknn_create_mem create_mem;
    fn_rknn_set_io_mem set_io_mem;
    fn_rknn_run run;
    fn_rknn_destroy_mem destroy_mem;
    fn_rknn_destroy destroy;
} g_rknn;

/* ============================================================================
 * RKNN model wrapper (adapted from rknn_model.c)
 * ============================================================================ */

#define FD_MAX_OUTPUTS 2

typedef struct {
    rknn_context ctx;
    rknn_input_output_num io_num;
    rknn_tensor_attr input_attr;
    rknn_tensor_attr output_attrs[FD_MAX_OUTPUTS];
    rknn_tensor_mem *input_mem;
    rknn_tensor_mem *output_mems[FD_MAX_OUTPUTS];
    uint32_t input_size;
} fd_rknn_model_t;

/* Embedding dimension for ProtoNet */
#define EMB_DIM 1024

/* ============================================================================
 * Module state
 * ============================================================================ */

static struct {
    /* Config (protected by mutex) */
    fd_config_t config;
    pthread_mutex_t config_mutex;

    /* State (protected by mutex) */
    fd_state_t state;
    pthread_mutex_t state_mutex;

    /* Thread */
    pthread_t thread;
    int thread_running;
    volatile int thread_stop;

    /* Frame buffer */
    uint8_t jpeg_buf[512 * 1024];
    size_t jpeg_size;
    volatile int need_frame;
    pthread_mutex_t frame_mutex;
    pthread_cond_t frame_cond;

    /* Model directory */
    char models_base_dir[256];

    /* ProtoNet prototypes (classification — 1024-dim) */
    float prototypes[2][EMB_DIM];
    float proto_norms[2];
    int prototypes_loaded;

    /* Spatial prototypes (separate — variable dim from header) */
    float spatial_protos[2][FD_SPATIAL_EMB_MAX];
    float spatial_proto_norms[2];
    int spatial_protos_loaded;
    int spatial_h, spatial_w, spatial_emb_dim, spatial_total;

    /* Coarse spatial prototypes (for multi-scale fusion) */
    float spatial_coarse_protos[2][FD_SPATIAL_EMB_MAX];
    float spatial_coarse_proto_norms[2];
    int spatial_coarse_loaded;
    int spatial_coarse_h, spatial_coarse_w, spatial_coarse_emb_dim, spatial_coarse_total;

    /* Z-dependent mask */
    float current_z;
    pthread_mutex_t z_mutex;

    /* Center-crop region (computed from decoded image dimensions) */
    float crop_x, crop_y, crop_w, crop_h;
    int crop_valid;

    /* CNN logit EMA for temporal smoothing */
    float cnn_ema_logits[2];
    int cnn_ema_init;

    /* Multiclass logit EMA for temporal smoothing */
    float multi_ema_logits[FD_MCLASS_COUNT];
    int multi_ema_init;

    /* Heatmap EMA for temporal smoothing (filters single-frame INT8 spikes) */
    float heatmap_ema[FD_SPATIAL_H_MAX][FD_SPATIAL_W_MAX];
    int heatmap_ema_init;

    /* Last FD-processed frame (for UI overlay) */
    uint8_t  fd_frame_buf[512 * 1024];
    size_t   fd_frame_size;
    uint64_t fd_frame_cycle;
    pthread_mutex_t fd_frame_mutex;

    /* Initialized flag */
    int initialized;
} g_fd;

/* Prototype management state (must be before thread func) */
static struct {
    fd_proto_compute_progress_t progress;
    pthread_mutex_t mutex;
    volatile int cancel;

    fd_download_progress_t dl_progress;
    pthread_mutex_t dl_mutex;
    pthread_t dl_thread;
    int dl_thread_running;
    volatile int dl_cancel;
    char dl_url[256];
    char dl_name[64];
} g_proto = {
    .mutex = PTHREAD_MUTEX_INITIALIZER,
    .dl_mutex = PTHREAD_MUTEX_INITIALIZER,
};

/* ============================================================================
 * Helpers
 * ============================================================================ */

static double fd_get_time_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000.0 + tv.tv_usec / 1000.0;
}

static int fd_get_available_memory_mb(void)
{
    FILE *f = fopen("/proc/meminfo", "r");
    if (!f) return -1;
    char line[128];
    int avail_kb = -1;
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "MemAvailable:", 13) == 0) {
            avail_kb = atoi(line + 13);
            break;
        }
    }
    fclose(f);
    return avail_kb > 0 ? avail_kb / 1024 : -1;
}

static void fd_softmax(float *arr, int n)
{
    float max_val = arr[0];
    for (int i = 1; i < n; i++)
        if (arr[i] > max_val) max_val = arr[i];
    float sum = 0.0f;
    for (int i = 0; i < n; i++) {
        arr[i] = expf(arr[i] - max_val);
        sum += arr[i];
    }
    for (int i = 0; i < n; i++)
        arr[i] /= sum;
}

static float fd_cosine_similarity(const float *a, const float *b,
                                   float norm_b, int n)
{
    float dot = 0.0f, na = 0.0f;
    for (int i = 0; i < n; i++) {
        dot += a[i] * b[i];
        na += a[i] * a[i];
    }
    na = sqrtf(na);
    if (na < 1e-12f || norm_b < 1e-12f)
        return 0.0f;
    return dot / (na * norm_b);
}

/* ============================================================================
 * RKNN dlopen/dlclose
 * ============================================================================ */

static int fd_rknn_load(void)
{
    if (g_rknn.handle) return 0;

    /* Try: 1) same dir as binary, 2) fault_detect dir, 3) system */
    const char *lib_path = NULL;
    char exe_dir_path[512];

    /* Resolve binary's directory from /proc/self/exe */
    ssize_t len = readlink("/proc/self/exe", exe_dir_path, sizeof(exe_dir_path) - 32);
    if (len > 0) {
        exe_dir_path[len] = '\0';
        char *slash = strrchr(exe_dir_path, '/');
        if (slash) {
            snprintf(slash + 1, sizeof(exe_dir_path) - (slash - exe_dir_path) - 1,
                     "%s", RKNN_LIB_NAME);
            lib_path = exe_dir_path;
            g_rknn.handle = dlopen(lib_path, RTLD_LAZY);
        }
    }
    if (!g_rknn.handle) {
        lib_path = RKNN_LIB_PATH_FD;
        g_rknn.handle = dlopen(lib_path, RTLD_LAZY);
    }
    if (!g_rknn.handle) {
        lib_path = RKNN_LIB_PATH_SYS;
        g_rknn.handle = dlopen(lib_path, RTLD_LAZY);
    }
    if (!g_rknn.handle) {
        fd_log("NPU not available: %s\n", dlerror());
        return -1;
    }

#define LOAD_SYM(name) do { \
    g_rknn.name = (fn_rknn_##name)dlsym(g_rknn.handle, "rknn_" #name); \
    if (!g_rknn.name) { \
        fd_err("dlsym rknn_%s failed: %s\n", #name, dlerror()); \
        dlclose(g_rknn.handle); \
        memset(&g_rknn, 0, sizeof(g_rknn)); \
        return -1; \
    } \
} while (0)

    LOAD_SYM(init);
    LOAD_SYM(query);
    LOAD_SYM(create_mem);
    LOAD_SYM(set_io_mem);
    LOAD_SYM(run);
    LOAD_SYM(destroy_mem);
    LOAD_SYM(destroy);
#undef LOAD_SYM

    fd_log("RKNN runtime loaded from %s\n", lib_path);
    return 0;
}

static void fd_rknn_unload(void)
{
    if (g_rknn.handle) {
        dlclose(g_rknn.handle);
        memset(&g_rknn, 0, sizeof(g_rknn));
    }
}

/* ============================================================================
 * RKNN model init/run/output/release (from rknn_model.c, uses dlopen ptrs)
 * ============================================================================ */

static int fd_model_init(fd_rknn_model_t *m, const char *model_path)
{
    int ret;
    memset(m, 0, sizeof(*m));

    ret = g_rknn.init(&m->ctx, (void *)model_path, 0, 0, NULL);
    if (ret < 0) {
        fd_err("rknn_init failed: %d (%s)\n", ret, model_path);
        return ret;
    }

    /* Query I/O counts */
    ret = g_rknn.query(m->ctx, RKNN_QUERY_IN_OUT_NUM, &m->io_num,
                       sizeof(m->io_num));
    if (ret < 0) {
        fd_err("rknn_query IN_OUT_NUM failed: %d\n", ret);
        goto fail;
    }
    if (m->io_num.n_input != 1 || m->io_num.n_output > FD_MAX_OUTPUTS) {
        fd_err("unexpected I/O: %u in, %u out\n",
               m->io_num.n_input, m->io_num.n_output);
        ret = -1;
        goto fail;
    }

    /* Query native input attr */
    m->input_attr.index = 0;
    ret = g_rknn.query(m->ctx, RKNN_QUERY_NATIVE_INPUT_ATTR, &m->input_attr,
                       sizeof(m->input_attr));
    if (ret < 0) {
        fd_err("rknn_query NATIVE_INPUT_ATTR failed: %d\n", ret);
        goto fail;
    }

    /* Override input to UINT8 NHWC */
    m->input_attr.type = RKNN_TENSOR_UINT8;
    m->input_attr.fmt = RKNN_TENSOR_NHWC;
    m->input_size = m->input_attr.size_with_stride;

    /* Allocate input memory (CMA) */
    m->input_mem = g_rknn.create_mem(m->ctx, m->input_attr.size_with_stride);
    if (!m->input_mem) {
        fd_err("CMA alloc failed for input\n");
        ret = -2;  /* memory failure */
        goto fail;
    }
    ret = g_rknn.set_io_mem(m->ctx, m->input_mem, &m->input_attr);
    if (ret < 0) {
        fd_err("rknn_set_io_mem input failed: %d\n", ret);
        goto fail;
    }

    /* Query and allocate outputs */
    for (uint32_t i = 0; i < m->io_num.n_output; i++) {
        m->output_attrs[i].index = i;
        ret = g_rknn.query(m->ctx, RKNN_QUERY_NATIVE_NHWC_OUTPUT_ATTR,
                           &m->output_attrs[i], sizeof(rknn_tensor_attr));
        if (ret < 0) {
            fd_err("rknn_query output[%u] failed: %d\n", i, ret);
            goto fail;
        }

        m->output_mems[i] = g_rknn.create_mem(m->ctx,
                                                m->output_attrs[i].size_with_stride);
        if (!m->output_mems[i]) {
            fd_err("CMA alloc failed for output[%u]\n", i);
            ret = -2;  /* memory failure */
            goto fail;
        }
        ret = g_rknn.set_io_mem(m->ctx, m->output_mems[i],
                                &m->output_attrs[i]);
        if (ret < 0) {
            fd_err("rknn_set_io_mem output[%u] failed: %d\n", i, ret);
            goto fail;
        }
    }

    return 0;

fail:
    /* Clean up partial init */
    if (m->input_mem) {
        g_rknn.destroy_mem(m->ctx, m->input_mem);
        m->input_mem = NULL;
    }
    for (uint32_t i = 0; i < m->io_num.n_output; i++) {
        if (m->output_mems[i]) {
            g_rknn.destroy_mem(m->ctx, m->output_mems[i]);
            m->output_mems[i] = NULL;
        }
    }
    g_rknn.destroy(m->ctx);
    m->ctx = 0;
    return ret;
}

static int fd_model_init_retry(fd_rknn_model_t *m, const char *model_path)
{
    int ret = fd_model_init(m, model_path);
    if (ret == 0) return 0;
    fd_log("Retrying model init after 200ms...\n");
    usleep(200000);
    ret = fd_model_init(m, model_path);
    if (ret == 0) return 0;
    fd_err("Model init failed after retry: %s\n", model_path);
    return ret;
}

static int fd_model_run(fd_rknn_model_t *m, const uint8_t *input_data,
                         uint32_t src_size)
{
    /* Cap copy at source size to prevent over-read when
     * size_with_stride (NC1HWC2 padded) > actual NHWC data */
    uint32_t copy_size = src_size < m->input_size ? src_size : m->input_size;
    memcpy(m->input_mem->virt_addr, input_data, copy_size);
    /* Zero-fill stride padding so NPU gets clean data */
    if (copy_size < m->input_size)
        memset((uint8_t *)m->input_mem->virt_addr + copy_size, 0,
               m->input_size - copy_size);
    return g_rknn.run(m->ctx, NULL);
}

static int fd_model_get_output(fd_rknn_model_t *m, int out_idx,
                                float *out_buf, int max_elems)
{
    if (out_idx < 0 || (uint32_t)out_idx >= m->io_num.n_output)
        return -1;

    rknn_tensor_attr *attr = &m->output_attrs[out_idx];
    const int8_t *raw = (const int8_t *)m->output_mems[out_idx]->virt_addr;
    int32_t zp = attr->zp;
    float scale = attr->scale;
    int n = (int)attr->n_elems;
    if (n > max_elems) n = max_elems;

    /* Linear dequantization — works for H=W=1 models (CNN, ProtoNet, Multiclass)
     * where NC1HWC2 layout is equivalent to flat channel order. */
    for (int i = 0; i < n; i++)
        out_buf[i] = ((float)raw[i] - zp) * scale;

    return n;
}

/* Get spatial model output as NHWC float.
 * Output queried with RKNN_QUERY_NATIVE_NHWC_OUTPUT_ATTR is already in NHWC
 * layout — just dequantize linearly. out_buf: H*W*C floats, [h][w][c]. */
static int fd_model_get_output_nhwc(fd_rknn_model_t *m, int out_idx,
                                     float *out_buf, int H, int W, int C)
{
    if (out_idx < 0 || (uint32_t)out_idx >= m->io_num.n_output)
        return -1;

    rknn_tensor_attr *attr = &m->output_attrs[out_idx];
    const int8_t *raw = (const int8_t *)m->output_mems[out_idx]->virt_addr;
    int32_t zp = attr->zp;
    float scale = attr->scale;
    int total = H * W * C;

    /* NHWC output: data is already in [H, W, C] order, just dequantize */
    for (int i = 0; i < total; i++)
        out_buf[i] = ((float)raw[i] - zp) * scale;

    return total;
}

static void fd_model_release(fd_rknn_model_t *m)
{
    if (!m->ctx) return;

    if (m->input_mem) {
        g_rknn.destroy_mem(m->ctx, m->input_mem);
        m->input_mem = NULL;
    }
    for (uint32_t i = 0; i < m->io_num.n_output; i++) {
        if (m->output_mems[i]) {
            g_rknn.destroy_mem(m->ctx, m->output_mems[i]);
            m->output_mems[i] = NULL;
        }
    }
    g_rknn.destroy(m->ctx);
    m->ctx = 0;
}

/* ============================================================================
 * Preprocessing (from preprocess.c)
 * ============================================================================ */

/* Decode JPEG to RGB using TurboJPEG */
typedef struct {
    uint8_t *data;
    int width, height;
} fd_image_t;

static int fd_decode_jpeg(const uint8_t *jpeg_data, size_t jpeg_size,
                           fd_image_t *img)
{
    tjhandle handle = tjInitDecompress();
    if (!handle) return -1;

    int width, height, subsample, colorspace;
    int ret = tjDecompressHeader3(handle, jpeg_data, (unsigned long)jpeg_size,
                                  &width, &height, &subsample, &colorspace);
    if (ret < 0) {
        tjDestroy(handle);
        return -1;
    }

    /* Find smallest TurboJPEG scaling factor where the decoded image is
     * large enough for fd_resize_crop to downscale (not upscale).
     * Need: resize scale = max(256/sh, 512/sw) <= 1.0
     * i.e., sw >= 512 AND sh >= 256 */
    int num_sf = 0;
    tjscalingfactor *sf = tjGetScalingFactors(&num_sf);
    tjscalingfactor best_sf = {1, 1};  /* default: no scaling */
    if (sf) {
        for (int i = 0; i < num_sf; i++) {
            int sw = TJSCALED(width, sf[i]);
            int sh = TJSCALED(height, sf[i]);
            int best_w = TJSCALED(width, best_sf);
            int best_h = TJSCALED(height, best_sf);
            if (sw >= 512 && sh >= 256 && sw * sh < best_w * best_h) {
                best_sf = sf[i];
            }
        }
    }

    int out_w = TJSCALED(width, best_sf);
    int out_h = TJSCALED(height, best_sf);

    img->data = (uint8_t *)malloc(out_w * out_h * 3);
    if (!img->data) {
        tjDestroy(handle);
        return -1;
    }
    img->width = out_w;
    img->height = out_h;

    ret = tjDecompress2(handle, jpeg_data, (unsigned long)jpeg_size,
                        img->data, out_w, 0, out_h, TJPF_RGB,
                        0);
    tjDestroy(handle);
    if (ret < 0) {
        free(img->data);
        img->data = NULL;
        return -1;
    }
    return 0;
}

/* Fused resize + center crop in single pass (no intermediate buffer).
 * Resizes so result >= 512x256, center-crops 448x224, keeps RGB color.
 * Bilinear interpolation. */
static void fd_resize_crop(const uint8_t *src, int sw, int sh,
                            uint8_t *dst)
{
    const int dw = FD_MODEL_INPUT_WIDTH;
    const int dh = FD_MODEL_INPUT_HEIGHT;
    float scale_h = 256.0f / (float)sh;
    float scale_w = 512.0f / (float)sw;
    float scale = scale_h > scale_w ? scale_h : scale_w;
    int rw = (int)(sw * scale);
    int rh = (int)(sh * scale);
    int cx = (rw - dw) / 2;
    int cy = (rh - dh) / 2;
    float x_ratio = (float)sw / (float)rw;
    float y_ratio = (float)sh / (float)rh;

    if (sw < 2 || sh < 2) {
        memset(dst, 0, dw * dh * 3);
        return;
    }

    for (int dy = 0; dy < dh; dy++) {
        float sy_f = (dy + cy) * y_ratio;
        int sy = (int)sy_f;
        float y_diff = sy_f - sy;
        if (sy < 0) { sy = 0; y_diff = 0.0f; }
        if (sy >= sh - 1) { sy = sh - 2; y_diff = 1.0f; }

        const uint8_t *row0 = src + sy * sw * 3;
        const uint8_t *row1 = src + (sy + 1) * sw * 3;

        for (int dx = 0; dx < dw; dx++) {
            float sx_f = (dx + cx) * x_ratio;
            int sx = (int)sx_f;
            float x_diff = sx_f - sx;
            if (sx < 0) { sx = 0; x_diff = 0.0f; }
            if (sx >= sw - 1) { sx = sw - 2; x_diff = 1.0f; }

            const uint8_t *a = row0 + sx * 3;
            const uint8_t *b = row0 + (sx + 1) * 3;
            const uint8_t *c = row1 + sx * 3;
            const uint8_t *d = row1 + (sx + 1) * 3;

            float w00 = (1.0f - x_diff) * (1.0f - y_diff);
            float w10 = x_diff * (1.0f - y_diff);
            float w01 = (1.0f - x_diff) * y_diff;
            float w11 = x_diff * y_diff;

            /* Bilinear interpolate each channel, keep RGB color */
            int off = (dy * dw + dx) * 3;
            for (int ch = 0; ch < 3; ch++) {
                float v = a[ch]*w00 + b[ch]*w10 + c[ch]*w01 + d[ch]*w11;
                int iv = (int)(v + 0.5f);
                if (iv < 0) iv = 0;
                if (iv > 255) iv = 255;
                dst[off + ch] = (uint8_t)iv;
            }
        }
    }
}

/* Preprocess: scaled-decode image → fused resize+crop (color RGB) */
static int fd_preprocess(const fd_image_t *img, uint8_t *out_buf)
{
    fd_resize_crop(img->data, img->width, img->height, out_buf);
    return 0;
}

/* ============================================================================
 * Prototypes loading
 * ============================================================================ */

static int fd_load_prototypes(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        fd_err("cannot open prototypes: %s\n", path);
        return -1;
    }

    size_t expected = 2 * EMB_DIM * sizeof(float);
    size_t nread = fread(g_fd.prototypes, 1, expected, f);
    fclose(f);

    if (nread != expected) {
        fd_err("prototypes file too short: %zu vs %zu\n", nread, expected);
        return -1;
    }

    for (int k = 0; k < 2; k++) {
        float sum = 0.0f;
        for (int i = 0; i < EMB_DIM; i++)
            sum += g_fd.prototypes[k][i] * g_fd.prototypes[k][i];
        g_fd.proto_norms[k] = sqrtf(sum);
    }
    g_fd.prototypes_loaded = 1;
    return 0;
}

/* Load spatial prototypes with header: [h][w][emb_dim][n_classes] + float data */
static int fd_load_spatial_prototypes(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        fd_log("Spatial prototypes not found: %s (will use classification protos)\n", path);
        return -1;
    }

    /* Read header: 4 x uint32_t */
    uint32_t header[4];
    if (fread(header, sizeof(uint32_t), 4, f) != 4) {
        fd_err("spatial prototypes header too short: %s\n", path);
        fclose(f);
        return -1;
    }

    int sp_h = (int)header[0];
    int sp_w = (int)header[1];
    int emb_dim = (int)header[2];
    int n_classes = (int)header[3];

    if (sp_h < 1 || sp_h > FD_SPATIAL_H_MAX || sp_w < 1 || sp_w > FD_SPATIAL_W_MAX) {
        fd_err("spatial prototypes: invalid grid %dx%d (max %dx%d)\n",
               sp_h, sp_w, FD_SPATIAL_H_MAX, FD_SPATIAL_W_MAX);
        fclose(f);
        return -1;
    }
    if (emb_dim < 1 || emb_dim > FD_SPATIAL_EMB_MAX) {
        fd_err("spatial prototypes: invalid emb_dim %d (max %d)\n",
               emb_dim, FD_SPATIAL_EMB_MAX);
        fclose(f);
        return -1;
    }
    if (n_classes != 2) {
        fd_err("spatial prototypes: expected 2 classes, got %d\n", n_classes);
        fclose(f);
        return -1;
    }

    /* Read prototype vectors — must read each class separately since the
     * array stride is FD_SPATIAL_EMB_MAX (1024) while actual dim may be less */
    memset(g_fd.spatial_protos, 0, sizeof(g_fd.spatial_protos));
    for (int k = 0; k < 2; k++) {
        if (fread(g_fd.spatial_protos[k], sizeof(float), emb_dim, f)
            != (size_t)emb_dim) {
            fd_err("spatial prototypes data too short for class %d\n", k);
            fclose(f);
            return -1;
        }
    }
    fclose(f);

    /* Compute norms */
    for (int k = 0; k < 2; k++) {
        float sum = 0.0f;
        for (int i = 0; i < emb_dim; i++)
            sum += g_fd.spatial_protos[k][i] * g_fd.spatial_protos[k][i];
        g_fd.spatial_proto_norms[k] = sqrtf(sum);
    }

    g_fd.spatial_h = sp_h;
    g_fd.spatial_w = sp_w;
    g_fd.spatial_emb_dim = emb_dim;
    g_fd.spatial_total = sp_h * sp_w * emb_dim;
    g_fd.spatial_protos_loaded = 1;

    fd_log("Spatial prototypes loaded: %dx%d grid, %d-dim embeddings, "
           "norms=[%.4f, %.4f], first5_fail=[%.4f,%.4f,%.4f,%.4f,%.4f], "
           "first5_succ=[%.4f,%.4f,%.4f,%.4f,%.4f]\n",
           sp_h, sp_w, emb_dim,
           g_fd.spatial_proto_norms[0], g_fd.spatial_proto_norms[1],
           g_fd.spatial_protos[0][0], g_fd.spatial_protos[0][1],
           g_fd.spatial_protos[0][2], g_fd.spatial_protos[0][3],
           g_fd.spatial_protos[0][4],
           g_fd.spatial_protos[1][0], g_fd.spatial_protos[1][1],
           g_fd.spatial_protos[1][2], g_fd.spatial_protos[1][3],
           g_fd.spatial_protos[1][4]);
    return 0;
}

/* Load coarse spatial prototypes (for multi-scale fusion) */
static int fd_load_spatial_prototypes_coarse(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        fd_log("Coarse spatial prototypes not found: %s\n", path);
        return -1;
    }

    uint32_t header[4];
    if (fread(header, sizeof(uint32_t), 4, f) != 4) {
        fd_err("coarse spatial prototypes header too short: %s\n", path);
        fclose(f);
        return -1;
    }

    int sp_h = (int)header[0];
    int sp_w = (int)header[1];
    int emb_dim = (int)header[2];
    int n_classes = (int)header[3];

    if (sp_h < 1 || sp_h > FD_SPATIAL_H_MAX || sp_w < 1 || sp_w > FD_SPATIAL_W_MAX ||
        emb_dim < 1 || emb_dim > FD_SPATIAL_EMB_MAX || n_classes != 2) {
        fd_err("coarse spatial prototypes: invalid header %dx%dx%d classes=%d\n",
               sp_h, sp_w, emb_dim, n_classes);
        fclose(f);
        return -1;
    }

    memset(g_fd.spatial_coarse_protos, 0, sizeof(g_fd.spatial_coarse_protos));
    for (int k = 0; k < 2; k++) {
        if (fread(g_fd.spatial_coarse_protos[k], sizeof(float), emb_dim, f)
            != (size_t)emb_dim) {
            fd_err("coarse spatial prototypes data too short for class %d\n", k);
            fclose(f);
            return -1;
        }
    }
    fclose(f);

    for (int k = 0; k < 2; k++) {
        float sum = 0.0f;
        for (int i = 0; i < emb_dim; i++)
            sum += g_fd.spatial_coarse_protos[k][i] * g_fd.spatial_coarse_protos[k][i];
        g_fd.spatial_coarse_proto_norms[k] = sqrtf(sum);
    }

    g_fd.spatial_coarse_h = sp_h;
    g_fd.spatial_coarse_w = sp_w;
    g_fd.spatial_coarse_emb_dim = emb_dim;
    g_fd.spatial_coarse_total = sp_h * sp_w * emb_dim;
    g_fd.spatial_coarse_loaded = 1;

    fd_log("Coarse spatial prototypes loaded: %dx%d grid, %d-dim, norms=[%.4f, %.4f]\n",
           sp_h, sp_w, emb_dim,
           g_fd.spatial_coarse_proto_norms[0], g_fd.spatial_coarse_proto_norms[1]);
    return 0;
}

/* Bilinear upscale heatmap from src_h x src_w to dst_h x dst_w */
static void fd_bilinear_upscale(const float *src, int src_h, int src_w,
                                 float *dst, int dst_h, int dst_w)
{
    for (int r = 0; r < dst_h; r++) {
        float sy = (r + 0.5f) * src_h / (float)dst_h - 0.5f;
        int y0 = (int)floorf(sy);
        float fy = sy - y0;
        if (y0 < 0) { y0 = 0; fy = 0.0f; }
        if (y0 >= src_h - 1) { y0 = src_h - 2; fy = 1.0f; }
        for (int c = 0; c < dst_w; c++) {
            float sx = (c + 0.5f) * src_w / (float)dst_w - 0.5f;
            int x0 = (int)floorf(sx);
            float fx = sx - x0;
            if (x0 < 0) { x0 = 0; fx = 0.0f; }
            if (x0 >= src_w - 1) { x0 = src_w - 2; fx = 1.0f; }
            float v = src[y0 * src_w + x0] * (1 - fy) * (1 - fx)
                    + src[y0 * src_w + (x0 + 1)] * (1 - fy) * fx
                    + src[(y0 + 1) * src_w + x0] * fy * (1 - fx)
                    + src[(y0 + 1) * src_w + (x0 + 1)] * fy * fx;
            dst[r * dst_w + c] = v;
        }
    }
}

/* ============================================================================
 * Model path resolution
 * ============================================================================ */

/* Path scheme: {base_dir}/{set_name}/{class_dir}/{filename} */
static int fd_resolve_model_path(fd_model_class_t cls, const char *set_name,
                                  char *path, size_t path_size,
                                  const fd_config_t *cfg)
{
    const char *class_dir;
    const char *filename;

    switch (cls) {
    case FD_MODEL_CNN:
        class_dir = "cnn";
        filename = cfg->cnn_file[0] ? cfg->cnn_file : "model.rknn";
        break;
    case FD_MODEL_PROTONET:
        class_dir = "protonet";
        filename = cfg->proto_file[0] ? cfg->proto_file : "encoder.rknn";
        break;
    case FD_MODEL_MULTICLASS:
        class_dir = "multiclass";
        filename = cfg->multi_file[0] ? cfg->multi_file : "multiclass.rknn";
        break;
    case FD_MODEL_SPATIAL:
        class_dir = "protonet";
        filename = "spatial_encoder.rknn";
        break;
    case FD_MODEL_SPATIAL_COARSE:
        class_dir = "protonet";
        filename = "spatial_encoder_coarse.rknn";
        break;
    default:
        return -1;
    }

    snprintf(path, path_size, "%s/%s/%s/%s",
             g_fd.models_base_dir, set_name, class_dir, filename);

    /* Check if file exists */
    if (access(path, R_OK) != 0) {
        /* For multiclass, try any .rknn file */
        if (cls == FD_MODEL_MULTICLASS) {
            char dir_path[512];
            snprintf(dir_path, sizeof(dir_path), "%s/%s/%s",
                     g_fd.models_base_dir, set_name, class_dir);
            DIR *dir = opendir(dir_path);
            if (dir) {
                struct dirent *ent;
                while ((ent = readdir(dir)) != NULL) {
                    size_t len = strlen(ent->d_name);
                    if (len > 5 && strcmp(ent->d_name + len - 5, ".rknn") == 0) {
                        snprintf(path, path_size, "%s/%s", dir_path,
                                 ent->d_name);
                        closedir(dir);
                        return 0;
                    }
                }
                closedir(dir);
            }
        }
        return -1;
    }
    return 0;
}

/* ============================================================================
 * Per-model inference (from detect.c)
 * ============================================================================ */

/* All thresholds extracted from config with fallback defaults */
typedef struct {
    float cnn_th, proto_th, multi_th;
    float cnn_dyn_th, proto_dyn_trigger;
    float heatmap_boost_th;
    int   boost_min_cells;
    float boost_cell_th;
    float boost_lean_factor;
    float boost_proto_lean;
    float boost_multi_lean;
    float boost_proto_veto;
    float boost_proto_strong;
    float boost_amp_cap;
    float boost_conf_cap;
    float ema_alpha;
    float heatmap_coarse_wt;
} fd_thresholds_t;

/* Thresholds: read from config, fallback to hardcoded defaults.
 * Defaults calibrated for INT8 on RV1106 hardware.
 * See BigEdge-FDM-Models CLAUDE.md for calibration details. */
static void fd_get_thresholds(const fd_config_t *cfg, int strategy,
                               fd_thresholds_t *th)
{
    const fd_active_thresholds_t *t = &cfg->thresholds;

    th->cnn_th   = t->cnn_threshold > 0 ? t->cnn_threshold : 0.50f;
    th->proto_th = t->proto_threshold > 0 ? t->proto_threshold : 0.65f;

    th->cnn_dyn_th      = t->cnn_dynamic_threshold > 0 ? t->cnn_dynamic_threshold : 0.45f;
    th->proto_dyn_trigger = t->proto_dynamic_trigger > 0 ? t->proto_dynamic_trigger : 0.60f;

    /* Multi-class threshold:
     * - VERIFY/CLASSIFY: low threshold (MC just labels fault type, doesn't decide binary)
     * - All others: configurable, default 0.81 printer-calibrated */
    if (strategy == FD_STRATEGY_VERIFY || strategy == FD_STRATEGY_CLASSIFY ||
        strategy == FD_STRATEGY_CLASSIFY_AND)
        th->multi_th = 0.10f;
    else
        th->multi_th = t->multi_threshold > 0 ? t->multi_threshold : 0.81f;

    /* Heatmap boost threshold: minimum heatmap_max for Path 1 (heatmap-only) override.
     * Default 1.6 calibrated from live print (worst OK=1.24, weakest fault=1.66). */
    th->heatmap_boost_th = t->heatmap_boost_threshold > 0 ? t->heatmap_boost_threshold : 1.6f;

    /* Advanced boost tuning — 0 means "use default" */
    th->boost_min_cells   = t->boost_min_cells > 0 ? t->boost_min_cells : 3;
    th->boost_cell_th     = t->boost_cell_threshold > 0 ? t->boost_cell_threshold : 0.30f;
    th->boost_lean_factor = t->boost_lean_factor > 0 ? t->boost_lean_factor : 0.50f;
    th->boost_proto_lean  = t->boost_proto_lean > 0 ? t->boost_proto_lean : 0.60f;
    th->boost_multi_lean  = t->boost_multi_lean > 0 ? t->boost_multi_lean : 0.25f;
    th->boost_proto_veto  = t->boost_proto_veto > 0 ? t->boost_proto_veto : 0.35f;
    th->boost_proto_strong = t->boost_proto_strong > 0 ? t->boost_proto_strong : 0.85f;
    th->boost_amp_cap     = t->boost_amplifier_cap > 0 ? t->boost_amplifier_cap : 2.0f;
    th->boost_conf_cap    = t->boost_confidence_cap > 0 ? t->boost_confidence_cap : 0.95f;
    th->ema_alpha         = t->ema_alpha > 0 ? t->ema_alpha : 0.30f;
    th->heatmap_coarse_wt = t->heatmap_coarse_weight > 0 ? t->heatmap_coarse_weight : 0.70f;
}

static int fd_run_cnn(const uint8_t *input, fd_result_t *r, float threshold,
                       const fd_config_t *cfg, float ema_alpha)
{
    char path[512];

    if (fd_resolve_model_path(FD_MODEL_CNN, cfg->model_set,
                               path, sizeof(path), cfg) < 0) {
        fd_err("CNN model not found in set: %s\n", cfg->model_set);
        return -1;
    }

    fd_rknn_model_t model;
    int init_ret = fd_model_init_retry(&model, path);
    if (init_ret < 0)
        return init_ret;

    double t0 = fd_get_time_ms();
    int ret = fd_model_run(&model, input, FD_MODEL_INPUT_BYTES);
    if (ret < 0) {
        fd_err("CNN run failed: %d\n", ret);
        fd_model_release(&model);
        return -1;
    }

    float logits[2] = {0};
    fd_model_get_output(&model, 0, logits, 2);
    double t1 = fd_get_time_ms();
    r->cnn_ms = (float)(t1 - t0);

    fd_model_release(&model);

    /* EMA smoothing on logits to reduce camera noise sensitivity.
     * The model amplifies tiny pixel-level noise into large logit swings
     * (~30% softmax spread on near-identical frames). Configurable alpha
     * (default 0.3) gives ~3x noise reduction with ~15s effective time
     * constant at 5s interval. */
    {
        if (!g_fd.cnn_ema_init) {
            g_fd.cnn_ema_logits[0] = logits[0];
            g_fd.cnn_ema_logits[1] = logits[1];
            g_fd.cnn_ema_init = 1;
        } else {
            g_fd.cnn_ema_logits[0] = ema_alpha * logits[0] +
                                      (1.0f - ema_alpha) * g_fd.cnn_ema_logits[0];
            g_fd.cnn_ema_logits[1] = ema_alpha * logits[1] +
                                      (1.0f - ema_alpha) * g_fd.cnn_ema_logits[1];
        }
        logits[0] = g_fd.cnn_ema_logits[0];
        logits[1] = g_fd.cnn_ema_logits[1];
    }

    fd_softmax(logits, 2);

    /* Model class ordering: [failure, success] — logits[0] is fault probability.
     * PyTorch ImageFolder alphabetical sort: failure=0, success=1.
     * RKNN preserves this ordering (verified via ONNX + RKNN simulator). */
    int cnn_class = logits[0] > threshold ? FD_CLASS_FAULT : FD_CLASS_OK;
    float cnn_conf = logits[0] > logits[1] ? logits[0] : logits[1];

    fd_log("  CNN: fail=%.3f th=%.2f -> %s (%.0fms)\n",
           logits[0], threshold,
           cnn_class == FD_CLASS_FAULT ? "FAULT" : "OK", r->cnn_ms);

    r->result = cnn_class;
    r->confidence = cnn_conf;
    return 0;
}

static int fd_run_protonet(const uint8_t *input, fd_result_t *r,
                            float proto_threshold, const fd_config_t *cfg)
{
    char path[512];

    if (fd_resolve_model_path(FD_MODEL_PROTONET, cfg->model_set,
                               path, sizeof(path), cfg) < 0) {
        fd_err("ProtoNet model not found in set: %s\n", cfg->model_set);
        return -1;
    }

    /* Load prototypes if not already loaded */
    if (!g_fd.prototypes_loaded) {
        const char *proto_file = cfg->proto_prototypes[0] ?
                                  cfg->proto_prototypes : "prototypes.bin";
        char proto_path[512];
        snprintf(proto_path, sizeof(proto_path), "%s/%s/protonet/%s",
                 g_fd.models_base_dir, cfg->model_set, proto_file);
        if (fd_load_prototypes(proto_path) < 0)
            return -1;
    }

    fd_rknn_model_t model;
    int init_ret = fd_model_init_retry(&model, path);
    if (init_ret < 0)
        return init_ret;

    double t0 = fd_get_time_ms();
    int ret = fd_model_run(&model, input, FD_MODEL_INPUT_BYTES);
    if (ret < 0) {
        fd_err("ProtoNet run failed: %d\n", ret);
        fd_model_release(&model);
        return -1;
    }

    float embedding[EMB_DIM];
    fd_model_get_output(&model, 0, embedding, EMB_DIM);
    double t1 = fd_get_time_ms();
    r->proto_ms = (float)(t1 - t0);

    fd_model_release(&model);

    float cos_fail = fd_cosine_similarity(embedding, g_fd.prototypes[0],
                                           g_fd.proto_norms[0], EMB_DIM);
    float cos_succ = fd_cosine_similarity(embedding, g_fd.prototypes[1],
                                           g_fd.proto_norms[1], EMB_DIM);
    float cos_margin = cos_fail - cos_succ;

    r->result = cos_margin > proto_threshold ? FD_CLASS_FAULT : FD_CLASS_OK;
    r->confidence = cos_margin;  /* signed margin for threshold-relative confidence */

    fd_log("  Proto: margin=%.3f th=%.2f -> %s (%.0fms)\n",
           cos_margin, proto_threshold,
           r->result == FD_CLASS_FAULT ? "FAULT" : "OK", r->proto_ms);

    return 0;
}

static int fd_run_multiclass(const uint8_t *input, fd_result_t *r,
                              float multi_threshold, const fd_config_t *cfg,
                              float ema_alpha)
{
    char path[512];

    if (fd_resolve_model_path(FD_MODEL_MULTICLASS, cfg->model_set,
                               path, sizeof(path), cfg) < 0) {
        fd_err("Multiclass model not found in set: %s\n", cfg->model_set);
        return -1;
    }

    fd_rknn_model_t model;
    int init_ret = fd_model_init_retry(&model, path);
    if (init_ret < 0)
        return init_ret;

    double t0 = fd_get_time_ms();
    int ret = fd_model_run(&model, input, FD_MODEL_INPUT_BYTES);
    if (ret < 0) {
        fd_err("Multiclass run failed: %d\n", ret);
        fd_model_release(&model);
        return -1;
    }

    float logits[FD_MCLASS_COUNT] = {0};
    fd_model_get_output(&model, 0, logits, FD_MCLASS_COUNT);
    double t1 = fd_get_time_ms();
    r->multi_ms = (float)(t1 - t0);

    fd_model_release(&model);

    /* EMA smoothing on logits — same approach as CNN EMA.
     * Multiclass scores swing ~15% between frames on static scenes.
     * Configurable alpha (default 0.3) smooths to ~3-5% effective variance. */
    {
        if (!g_fd.multi_ema_init) {
            for (int i = 0; i < FD_MCLASS_COUNT; i++)
                g_fd.multi_ema_logits[i] = logits[i];
            g_fd.multi_ema_init = 1;
        } else {
            for (int i = 0; i < FD_MCLASS_COUNT; i++)
                g_fd.multi_ema_logits[i] = ema_alpha * logits[i] +
                                            (1.0f - ema_alpha) * g_fd.multi_ema_logits[i];
        }
        for (int i = 0; i < FD_MCLASS_COUNT; i++)
            logits[i] = g_fd.multi_ema_logits[i];
    }

    fd_softmax(logits, FD_MCLASS_COUNT);

    /* Find argmax */
    int best = 0;
    for (int i = 1; i < FD_MCLASS_COUNT; i++)
        if (logits[i] > logits[best]) best = i;
    r->fault_class = best;
    snprintf(r->fault_class_name, sizeof(r->fault_class_name), "%s",
             fd_fault_class_name(best));

    /* Binary collapse: FAULT if 1 - p(Success) > threshold */
    float multi_conf = 1.0f - logits[FD_MCLASS_SUCCESS];
    r->result = multi_conf > multi_threshold ? FD_CLASS_FAULT : FD_CLASS_OK;
    r->confidence = multi_conf;

    fd_log("  Multi: 1-p(Succ)=%.3f class=%s (%.0fms)\n",
           multi_conf, r->fault_class_name, r->multi_ms);

    return 0;
}

/* ============================================================================
 * Spatial heatmap inference
 * ============================================================================ */

/* Compute per-location heatmap from features and prototypes.
 * Returns max margin value. Fills heatmap array. */
static float fd_compute_heatmap(const float *features, int sp_h, int sp_w,
                                 int emb_dim,
                                 const float protos[][FD_SPATIAL_EMB_MAX],
                                 const float *proto_norms,
                                 float heatmap[][FD_SPATIAL_W_MAX])
{
    int use_dot_product = (proto_norms[0] < 1.1f && proto_norms[1] < 1.1f);
    float max_margin = -999.0f;

    for (int h = 0; h < sp_h; h++) {
        for (int w = 0; w < sp_w; w++) {
            const float *vec = &features[(h * sp_w + w) * emb_dim];
            float margin;
            if (use_dot_product) {
                float dot_fail = 0.0f, dot_succ = 0.0f;
                for (int i = 0; i < emb_dim; i++) {
                    dot_fail += vec[i] * protos[0][i];
                    dot_succ += vec[i] * protos[1][i];
                }
                margin = dot_fail - dot_succ;
            } else {
                float cos_fail = fd_cosine_similarity(vec, protos[0],
                                                       proto_norms[0], emb_dim);
                float cos_succ = fd_cosine_similarity(vec, protos[1],
                                                       proto_norms[1], emb_dim);
                margin = cos_fail - cos_succ;
            }
            heatmap[h][w] = margin;
            if (margin > max_margin)
                max_margin = margin;
        }
    }
    return max_margin;
}

/* Run a single spatial encoder and read output features into spatial_buf.
 * Output is converted from RKNN NC1HWC2 format to NHWC for correct per-cell
 * channel ordering (required for heatmap cosine similarity computation).
 * Returns 0=ok, -1=error, timing stored in *ms_out. */
static int fd_run_spatial_encoder(const char *model_path,
                                   const uint8_t *input, float *spatial_buf,
                                   int sp_h, int sp_w, int emb_dim,
                                   float *ms_out)
{
    fd_rknn_model_t model;
    int init_ret = fd_model_init_retry(&model, model_path);
    if (init_ret < 0)
        return init_ret;

    double t0 = fd_get_time_ms();
    int ret = fd_model_run(&model, input, FD_MODEL_INPUT_BYTES);
    if (ret < 0) {
        fd_err("Spatial run failed: %d (model=%s)\n", ret, model_path);
        fd_model_release(&model);
        return -1;
    }

    int sp_total = sp_h * sp_w * emb_dim;
    int n = fd_model_get_output_nhwc(&model, 0, spatial_buf,
                                      sp_h, sp_w, emb_dim);
    double t1 = fd_get_time_ms();

    fd_model_release(&model);

    if (ms_out) *ms_out = (float)(t1 - t0);

    if (n < sp_total) {
        fd_err("Spatial output too short: %d vs %d\n", n, sp_total);
        return -1;
    }

    return 0;
}

/* Run spatial encoder and compute per-location heatmap.
 * Auto-detects multi-scale mode when both coarse + fine encoders exist.
 * spatial_buf must be pre-allocated (large enough for model output).
 * Returns 0=ok, -1=error, -2=CMA failure */
static int fd_run_heatmap(const uint8_t *input, fd_result_t *r,
                          const fd_config_t *cfg, float *spatial_buf,
                          fd_mask196_t active_mask, float heatmap_coarse_wt,
                          float ema_alpha)
{
    char fine_path[512], coarse_path[512];

    int have_fine = (fd_resolve_model_path(FD_MODEL_SPATIAL, cfg->model_set,
                                            fine_path, sizeof(fine_path), cfg) == 0);
    int have_coarse = (fd_resolve_model_path(FD_MODEL_SPATIAL_COARSE, cfg->model_set,
                                              coarse_path, sizeof(coarse_path), cfg) == 0);

    if (!have_fine && !have_coarse) {
        fd_err("No spatial model found in set: %s\n", cfg->model_set);
        return -1;
    }

    /* Load fine spatial prototypes on first call */
    if (have_fine && !g_fd.spatial_protos_loaded) {
        char sp_path[512];
        snprintf(sp_path, sizeof(sp_path), "%s/%s/protonet/spatial_prototypes.bin",
                 g_fd.models_base_dir, cfg->model_set);
        fd_load_spatial_prototypes(sp_path);
    }

    /* Load coarse spatial prototypes on first call */
    if (have_coarse && !g_fd.spatial_coarse_loaded) {
        char sp_path[512];
        snprintf(sp_path, sizeof(sp_path), "%s/%s/protonet/spatial_prototypes_coarse.bin",
                 g_fd.models_base_dir, cfg->model_set);
        fd_load_spatial_prototypes_coarse(sp_path);
    }

    /* Memory gate */
    int mem_mb = fd_get_available_memory_mb();
    if (mem_mb > 0 && mem_mb < cfg->min_free_mem_mb) {
        fd_log("  Heatmap: skipping, %dMB free < %dMB min\n",
               mem_mb, cfg->min_free_mem_mb);
        return -2;
    }

    /* Clear entire heatmap array */
    memset(r->heatmap, 0, sizeof(r->heatmap));
    double t_total_start = fd_get_time_ms();

    /* ---- Multi-scale mode: coarse + fine → blend ---- */
    if (have_coarse && g_fd.spatial_coarse_loaded && have_fine && g_fd.spatial_protos_loaded) {
        int ch = g_fd.spatial_coarse_h, cw = g_fd.spatial_coarse_w;
        int c_emb = g_fd.spatial_coarse_emb_dim;
        int fh = g_fd.spatial_h, fw = g_fd.spatial_w;
        int f_emb = g_fd.spatial_emb_dim;

        /* Step 1: Run coarse encoder → compute coarse heatmap.
         * Use a temporary 2D array for fd_compute_heatmap (stride=FD_SPATIAL_W_MAX),
         * then compact to flat array for upscaling (stride=cw). */
        float coarse_ms = 0;
        int rc = fd_run_spatial_encoder(coarse_path, input, spatial_buf,
                                         ch, cw, c_emb, &coarse_ms);
        if (rc < 0) return rc;

        float coarse_hm[FD_SPATIAL_H_MAX][FD_SPATIAL_W_MAX] = {{0}};
        fd_compute_heatmap(spatial_buf, ch, cw, c_emb,
                           (const float (*)[FD_SPATIAL_EMB_MAX])g_fd.spatial_coarse_protos,
                           g_fd.spatial_coarse_proto_norms, coarse_hm);

        /* Compact coarse heatmap to flat array (stride=cw) for bilinear upscale */
        float coarse_flat[FD_SPATIAL_H_MAX * FD_SPATIAL_W_MAX];
        for (int h = 0; h < ch; h++)
            for (int w = 0; w < cw; w++)
                coarse_flat[h * cw + w] = coarse_hm[h][w];

        /* Step 2: Run fine encoder → compute fine heatmap */
        float fine_ms = 0;
        rc = fd_run_spatial_encoder(fine_path, input, spatial_buf,
                                     fh, fw, f_emb, &fine_ms);
        if (rc < 0) return rc;

        float fine_hm[FD_SPATIAL_H_MAX][FD_SPATIAL_W_MAX] = {{0}};
        fd_compute_heatmap(spatial_buf, fh, fw, f_emb,
                           (const float (*)[FD_SPATIAL_EMB_MAX])g_fd.spatial_protos,
                           g_fd.spatial_proto_norms, fine_hm);

        /* Step 3: Upscale coarse to fine resolution */
        float coarse_up[FD_SPATIAL_H_MAX * FD_SPATIAL_W_MAX];
        fd_bilinear_upscale(coarse_flat, ch, cw, coarse_up, fh, fw);

        /* Step 4: Normalize fine to match coarse value range */
        float c_min = 999.0f, c_max = -999.0f;
        float f_min = 999.0f, f_max = -999.0f;
        for (int i = 0; i < fh * fw; i++) {
            if (coarse_up[i] < c_min) c_min = coarse_up[i];
            if (coarse_up[i] > c_max) c_max = coarse_up[i];
        }
        for (int h = 0; h < fh; h++)
            for (int w = 0; w < fw; w++) {
                if (fine_hm[h][w] < f_min) f_min = fine_hm[h][w];
                if (fine_hm[h][w] > f_max) f_max = fine_hm[h][w];
            }
        float c_range = c_max - c_min;
        float f_range = f_max - f_min;
        float fine_scale = (f_range > 1e-8f) ? c_range / f_range : 0.0f;

        /* Step 5: Blend — coarse_wt% coarse + (1-coarse_wt)% fine (scaled) */
        int mask_active = !fd_mask_is_zero(&active_mask);
        float fine_wt = 1.0f - heatmap_coarse_wt;

        for (int h = 0; h < fh; h++) {
            for (int w = 0; w < fw; w++) {
                float v = heatmap_coarse_wt * coarse_up[h * fw + w]
                        + fine_wt * fine_hm[h][w] * fine_scale;
                r->heatmap[h][w] = v;
            }
        }

        /* Step 5b: EMA smoothing — filters single-frame INT8 quantization spikes.
         * Applied to blended heatmap before max search and boost logic. */
        if (!g_fd.heatmap_ema_init) {
            for (int h = 0; h < fh; h++)
                for (int w = 0; w < fw; w++)
                    g_fd.heatmap_ema[h][w] = r->heatmap[h][w];
            g_fd.heatmap_ema_init = 1;
        } else {
            float inv_alpha = 1.0f - ema_alpha;
            for (int h = 0; h < fh; h++)
                for (int w = 0; w < fw; w++) {
                    g_fd.heatmap_ema[h][w] = ema_alpha * r->heatmap[h][w]
                                            + inv_alpha * g_fd.heatmap_ema[h][w];
                }
        }

        /* Copy EMA values back to result (used by boost logic, UI overlay, logging) */
        float max_margin = -999.0f;
        int max_h = 0, max_w = 0;
        for (int h = 0; h < fh; h++) {
            for (int w = 0; w < fw; w++) {
                r->heatmap[h][w] = g_fd.heatmap_ema[h][w];

                int cell_idx = h * fw + w;
                if (mask_active && !fd_mask_test_bit(&active_mask, cell_idx))
                    continue;
                if (r->heatmap[h][w] > max_margin) {
                    max_margin = r->heatmap[h][w];
                    max_h = h;
                    max_w = w;
                }
            }
        }

        r->has_heatmap = 1;
        r->spatial_h = fh;
        r->spatial_w = fw;
        r->heatmap_max = max_margin;
        r->heatmap_max_h = max_h;
        r->heatmap_max_w = max_w;
        r->spatial_ms = (float)(fd_get_time_ms() - t_total_start);

        /* Per-encoder diagnostics (debug_logging only) */
        if (cfg->debug_logging) {
            int c_ch = ch / 2, c_cw = cw / 2;
            int f_ch = fh / 2, f_cw = fw / 2;
            float c_max_v = -999.0f, f_max_v = -999.0f;
            for (int h = 0; h < ch; h++)
                for (int w = 0; w < cw; w++)
                    if (coarse_hm[h][w] > c_max_v) c_max_v = coarse_hm[h][w];
            for (int h = 0; h < fh; h++)
                for (int w = 0; w < fw; w++)
                    if (fine_hm[h][w] > f_max_v) f_max_v = fine_hm[h][w];
            fd_log("  HEATMAP_SPLIT: coarse center[%d][%d]=%.3f max=%.3f | "
                   "fine center[%d][%d]=%.3f max=%.3f | "
                   "fine_scale=%.3f coarse_wt=%.2f\n",
                   c_ch, c_cw, coarse_hm[c_ch][c_cw], c_max_v,
                   f_ch, f_cw, fine_hm[f_ch][f_cw], f_max_v,
                   fine_scale, heatmap_coarse_wt);
        }
        fd_log("  Heatmap: %dx%d multi-scale max=%.2f at [%d,%d] "
               "(coarse=%.0fms fine=%.0fms total=%.0fms)\n",
               fh, fw, max_margin, max_h, max_w,
               coarse_ms, fine_ms, r->spatial_ms);
        return 0;
    }

    /* ---- Single-encoder mode (fallback) ---- */
    const char *model_path;
    int sp_h, sp_w, emb_dim;
    const float (*protos)[FD_SPATIAL_EMB_MAX];
    const float *proto_norms;

    if (have_coarse && g_fd.spatial_coarse_loaded) {
        model_path = coarse_path;

        sp_h = g_fd.spatial_coarse_h;
        sp_w = g_fd.spatial_coarse_w;
        emb_dim = g_fd.spatial_coarse_emb_dim;
        protos = (const float (*)[FD_SPATIAL_EMB_MAX])g_fd.spatial_coarse_protos;
        proto_norms = g_fd.spatial_coarse_proto_norms;
    } else if (have_fine && g_fd.spatial_protos_loaded) {
        model_path = fine_path;

        sp_h = g_fd.spatial_h;
        sp_w = g_fd.spatial_w;
        emb_dim = g_fd.spatial_emb_dim;
        protos = (const float (*)[FD_SPATIAL_EMB_MAX])g_fd.spatial_protos;
        proto_norms = g_fd.spatial_proto_norms;
    } else if (have_fine && g_fd.prototypes_loaded) {
        /* Fallback: fine encoder with classification prototypes */
        model_path = fine_path;

        sp_h = 7;
        sp_w = 7;
        emb_dim = EMB_DIM;
        protos = (const float (*)[FD_SPATIAL_EMB_MAX])g_fd.prototypes;
        proto_norms = g_fd.proto_norms;
    } else {
        model_path = have_coarse ? coarse_path : fine_path;
        sp_h = 7;
        sp_w = 7;
        emb_dim = EMB_DIM;
        protos = (const float (*)[FD_SPATIAL_EMB_MAX])g_fd.prototypes;
        proto_norms = g_fd.proto_norms;
    }

    float enc_ms = 0;
    int rc = fd_run_spatial_encoder(model_path, input, spatial_buf,
                                     sp_h, sp_w, emb_dim, &enc_ms);
    if (rc < 0) return rc;

    fd_compute_heatmap(spatial_buf, sp_h, sp_w, emb_dim, protos, proto_norms,
                       r->heatmap);

    /* EMA smoothing — filters single-frame INT8 quantization spikes */
    if (!g_fd.heatmap_ema_init) {
        for (int h = 0; h < sp_h; h++)
            for (int w = 0; w < sp_w; w++)
                g_fd.heatmap_ema[h][w] = r->heatmap[h][w];
        g_fd.heatmap_ema_init = 1;
    } else {
        float inv_alpha = 1.0f - ema_alpha;
        for (int h = 0; h < sp_h; h++)
            for (int w = 0; w < sp_w; w++) {
                g_fd.heatmap_ema[h][w] = ema_alpha * r->heatmap[h][w]
                                        + inv_alpha * g_fd.heatmap_ema[h][w];
            }
    }

    /* Find max within active mask (from EMA-smoothed values) */
    float max_margin = -999.0f;
    int max_h = 0, max_w = 0;
    int mask_active = !fd_mask_is_zero(&active_mask);
    for (int h = 0; h < sp_h; h++) {
        for (int w = 0; w < sp_w; w++) {
            r->heatmap[h][w] = g_fd.heatmap_ema[h][w];

            int cell_idx = h * sp_w + w;
            if (mask_active && !fd_mask_test_bit(&active_mask, cell_idx))
                continue;
            if (r->heatmap[h][w] > max_margin) {
                max_margin = r->heatmap[h][w];
                max_h = h;
                max_w = w;
            }
        }
    }

    r->has_heatmap = 1;
    r->spatial_h = sp_h;
    r->spatial_w = sp_w;
    r->heatmap_max = max_margin;
    r->heatmap_max_h = max_h;
    r->heatmap_max_w = max_w;
    r->spatial_ms = enc_ms;

    fd_log("  Heatmap: %dx%d max=%.2f at [%d,%d] (%.0fms)\n",
           sp_h, sp_w, max_margin, max_h, max_w, r->spatial_ms);

    return 0;
}

/* ============================================================================
 * Combined detection + strategy (from detect.c)
 * ============================================================================ */

/* Returns 0 on success, -1 if a model failed to load (skip cycle) */
static int fd_run_detection(const uint8_t *preprocessed, fd_result_t *result,
                             fd_config_t *cfg, float *spatial_buf)
{
    double t0 = fd_get_time_ms();
    memset(result, 0, sizeof(*result));
    snprintf(result->fault_class_name, sizeof(result->fault_class_name), "-");

    /* Get thresholds from config (or fallback to hardcoded defaults) */
    fd_thresholds_t th;
    fd_get_thresholds(cfg, cfg->strategy, &th);
    float cnn_th = th.cnn_th, proto_th = th.proto_th, multi_th = th.multi_th;
    float cnn_dyn_th = th.cnn_dyn_th, proto_dyn_trigger = th.proto_dyn_trigger;
    float heatmap_boost_th = th.heatmap_boost_th;

    int have_cnn = cfg->cnn_enabled;
    int have_proto = cfg->proto_enabled;
    int have_multi = cfg->multi_enabled;

    /* Single-model strategies override enables */
    if (cfg->strategy == FD_STRATEGY_CNN) {
        have_cnn = 1; have_proto = 0; have_multi = 0;
    } else if (cfg->strategy == FD_STRATEGY_PROTONET) {
        have_cnn = 0; have_proto = 1; have_multi = 0;
    } else if (cfg->strategy == FD_STRATEGY_MULTICLASS) {
        have_cnn = 0; have_proto = 0; have_multi = 1;
    } else if (cfg->strategy == FD_STRATEGY_AND ||
               cfg->strategy == FD_STRATEGY_OR) {
        have_multi = 0;  /* AND/OR: CNN+Proto only, no multiclass */
    }

    /* Per-model results */
    int cnn_class = FD_CLASS_OK, proto_class = FD_CLASS_OK, multi_class = FD_CLASS_OK;
    float cnn_conf = 0.5f, proto_conf = 0.0f, multi_conf = 0.5f;
    fd_result_t model_result;

    int pace_us = cfg->pace_ms * 1000;
    int rc;

    /* Run ProtoNet FIRST (its margin gates the CNN threshold) */
    if (have_proto) {
        memset(&model_result, 0, sizeof(model_result));
        rc = fd_run_protonet(preprocessed, &model_result, proto_th, cfg);
        if (rc < 0) {
            result->total_ms = (float)(fd_get_time_ms() - t0);
            return rc;
        }
        proto_class = model_result.result;
        proto_conf = model_result.confidence;
        result->proto_ms = model_result.proto_ms;
        if (pace_us > 0 && have_cnn) usleep(pace_us);
    }

    /* Dynamic CNN threshold: when ProtoNet is moderately suspicious,
     * lower the CNN threshold to catch light faults.
     * Only for OR/majority/verify/classify — for AND/all strategies it's
     * counterproductive (increases false agreement between models). */
    if (have_proto && have_cnn && proto_conf >= proto_dyn_trigger &&
        cfg->strategy != FD_STRATEGY_AND &&
        cfg->strategy != FD_STRATEGY_CLASSIFY_AND &&
        cfg->strategy != FD_STRATEGY_ALL) {
        cnn_th = cnn_dyn_th;
        fd_log("  Dynamic CNN th: %.2f (proto=%.3f trigger=%.2f)\n",
               cnn_th, proto_conf, proto_dyn_trigger);
    }

    /* Memory gate: skip remaining models if memory is low */
    if (have_cnn) {
        int mem_mb = fd_get_available_memory_mb();
        if (mem_mb > 0 && mem_mb < cfg->min_free_mem_mb) {
            fd_log("  Skipping CNN: %dMB free < %dMB min\n",
                   mem_mb, cfg->min_free_mem_mb);
            have_cnn = 0;
        }
    }

    /* Run CNN */
    if (have_cnn) {
        memset(&model_result, 0, sizeof(model_result));
        rc = fd_run_cnn(preprocessed, &model_result, cnn_th, cfg, th.ema_alpha);
        if (rc < 0) {
            result->total_ms = (float)(fd_get_time_ms() - t0);
            return rc;
        }
        cnn_class = model_result.result;
        cnn_conf = model_result.confidence;
        result->cnn_ms = model_result.cnn_ms;
    }

    /* VERIFY/CLASSIFY: only run multiclass if CNN or ProtoNet flagged FAULT,
     * unless heatmap is enabled — then always run multi for boost corroboration
     * and consistent reporting on OK cycles. */
    int run_multi = have_multi;
    if (run_multi && !cfg->heatmap_enabled &&
        (cfg->strategy == FD_STRATEGY_VERIFY ||
         cfg->strategy == FD_STRATEGY_CLASSIFY ||
         cfg->strategy == FD_STRATEGY_CLASSIFY_AND)) {
        int or_fault = 0;
        if (have_cnn && cnn_class == FD_CLASS_FAULT) or_fault = 1;
        if (have_proto && proto_class == FD_CLASS_FAULT) or_fault = 1;
        run_multi = or_fault;
    }

    /* Memory gate before multiclass */
    if (run_multi) {
        int mem_mb = fd_get_available_memory_mb();
        if (mem_mb > 0 && mem_mb < cfg->min_free_mem_mb) {
            fd_log("  Skipping Multi: %dMB free < %dMB min\n",
                   mem_mb, cfg->min_free_mem_mb);
            run_multi = 0;
        }
    }

    /* Run Multiclass */
    if (run_multi) {
        if (pace_us > 0) usleep(pace_us);
        memset(&model_result, 0, sizeof(model_result));
        if (fd_run_multiclass(preprocessed, &model_result, multi_th, cfg, th.ema_alpha) == 0) {
            multi_class = model_result.result;
            multi_conf = model_result.confidence;
            result->multi_ms = model_result.multi_ms;
            result->fault_class = model_result.fault_class;
            snprintf(result->fault_class_name,
                     sizeof(result->fault_class_name), "%s",
                     model_result.fault_class_name);
        }
    }

    /* Combine results by strategy */
    int n_models = 0, n_fault = 0;
    int votes[3] = {-1, -1, -1};

    if (have_cnn) {
        votes[0] = cnn_class; n_models++;
        if (cnn_class == FD_CLASS_FAULT) n_fault++;
        result->cnn_vote = (cnn_class == FD_CLASS_FAULT) ? 1 : 0;
    }
    if (have_proto) {
        votes[1] = proto_class; n_models++;
        if (proto_class == FD_CLASS_FAULT) n_fault++;
        result->proto_vote = (proto_class == FD_CLASS_FAULT) ? 1 : 0;
    }
    if (run_multi) {
        votes[2] = multi_class; n_models++;
        if (multi_class == FD_CLASS_FAULT) n_fault++;
        result->multi_vote = (multi_class == FD_CLASS_FAULT) ? 1 : 0;
    }

    switch (cfg->strategy) {
    case FD_STRATEGY_OR:
        result->result = n_fault > 0 ? FD_CLASS_FAULT : FD_CLASS_OK;
        break;
    case FD_STRATEGY_MAJORITY:
        result->result = (n_fault * 2 > n_models) ? FD_CLASS_FAULT : FD_CLASS_OK;
        break;
    case FD_STRATEGY_ALL:
        result->result = (n_fault == n_models) ? FD_CLASS_FAULT : FD_CLASS_OK;
        break;
    case FD_STRATEGY_CNN:
        result->result = cnn_class;
        break;
    case FD_STRATEGY_PROTONET:
        result->result = proto_class;
        break;
    case FD_STRATEGY_MULTICLASS:
        result->result = multi_class;
        break;
    case FD_STRATEGY_VERIFY:
        if (!run_multi)
            result->result = FD_CLASS_OK;
        else
            result->result = (multi_class == FD_CLASS_FAULT) ?
                              FD_CLASS_FAULT : FD_CLASS_OK;
        break;
    case FD_STRATEGY_CLASSIFY: {
        int or_fault = 0;
        if (have_cnn && cnn_class == FD_CLASS_FAULT) or_fault = 1;
        if (have_proto && proto_class == FD_CLASS_FAULT) or_fault = 1;
        result->result = or_fault ? FD_CLASS_FAULT : FD_CLASS_OK;
        break;
    }
    case FD_STRATEGY_AND:
    case FD_STRATEGY_CLASSIFY_AND: {
        int and_fault = 0;
        if (have_cnn && have_proto)
            and_fault = (cnn_class == FD_CLASS_FAULT && proto_class == FD_CLASS_FAULT);
        else if (have_cnn)
            and_fault = (cnn_class == FD_CLASS_FAULT);
        else if (have_proto)
            and_fault = (proto_class == FD_CLASS_FAULT);
        result->result = and_fault ? FD_CLASS_FAULT : FD_CLASS_OK;
        break;
    }
    }

    /* Count agreement */
    result->agreement = 0;
    for (int i = 0; i < 3; i++) {
        if (votes[i] >= 0 && votes[i] == result->result)
            result->agreement++;
    }

    /* Combined confidence — continuous fault likelihood [0, 1].
     * Each model produces a directional score (higher = more likely fault)
     * independent of the binary threshold decision. Combined by strategy
     * so confidence varies smoothly, no cliff at threshold boundaries. */
    float cnn_fault_lk = 0.5f;
    float proto_fault_lk = 0.5f;
    float multi_fault_lk = 0.5f;

    if (have_cnn) {
        /* CNN softmax: fail_prob already in [0, 1] */
        cnn_fault_lk = (cnn_class == FD_CLASS_FAULT) ? cnn_conf : 1.0f - cnn_conf;
    }
    if (have_proto) {
        /* Proto confidence is signed margin in ~[-1, 1], map to [0, 1] */
        proto_fault_lk = 0.5f + 0.5f * proto_conf;
        if (proto_fault_lk < 0.0f) proto_fault_lk = 0.0f;
        if (proto_fault_lk > 1.0f) proto_fault_lk = 1.0f;
    }
    if (run_multi) {
        /* Multi: 1-p(success) is already a [0, 1] fault likelihood */
        multi_fault_lk = multi_conf;
    }

    /* Store per-model detail in result */
    result->cnn_ran = have_cnn;
    result->proto_ran = have_proto;
    result->multi_ran = run_multi;
    result->cnn_raw = cnn_fault_lk;           /* fail prob [0,1] */
    result->proto_raw = proto_conf;            /* signed margin ~[-1,1] */
    result->multi_raw = multi_conf;            /* 1-p(success) [0,1] */
    result->cnn_fault_lk = cnn_fault_lk;
    result->proto_fault_lk = proto_fault_lk;
    result->multi_fault_lk = multi_fault_lk;

    /* Combine by strategy — each strategy's confidence matches its
     * decision logic so the score varies smoothly around thresholds. */
    switch (cfg->strategy) {
    case FD_STRATEGY_AND:
    case FD_STRATEGY_CLASSIFY_AND: {
        /* AND: weakest of CNN + Proto (multi is labeling only) */
        float min_lk = 1.0f;
        if (have_cnn && cnn_fault_lk < min_lk) min_lk = cnn_fault_lk;
        if (have_proto && proto_fault_lk < min_lk) min_lk = proto_fault_lk;
        result->confidence = min_lk;
        break;
    }
    case FD_STRATEGY_ALL: {
        /* ALL: weakest of all active models */
        float min_lk = 1.0f;
        if (have_cnn && cnn_fault_lk < min_lk) min_lk = cnn_fault_lk;
        if (have_proto && proto_fault_lk < min_lk) min_lk = proto_fault_lk;
        if (run_multi && multi_fault_lk < min_lk) min_lk = multi_fault_lk;
        result->confidence = min_lk;
        break;
    }
    case FD_STRATEGY_OR: {
        /* OR: strongest signal across all active models */
        float max_lk = 0.0f;
        if (have_cnn && cnn_fault_lk > max_lk) max_lk = cnn_fault_lk;
        if (have_proto && proto_fault_lk > max_lk) max_lk = proto_fault_lk;
        if (run_multi && multi_fault_lk > max_lk) max_lk = multi_fault_lk;
        result->confidence = max_lk;
        break;
    }
    case FD_STRATEGY_CLASSIFY: {
        /* CLASSIFY: OR of CNN + Proto for decision.
         * Confidence: multi-weighted (3x) when available, else max(CNN,Proto). */
        if (run_multi) {
            float w_sum = 3.0f * multi_fault_lk;
            float w_total = 3.0f;
            if (have_cnn) { w_sum += cnn_fault_lk; w_total += 1.0f; }
            if (have_proto) { w_sum += proto_fault_lk; w_total += 1.0f; }
            result->confidence = w_sum / w_total;
        } else {
            float max_lk = 0.0f;
            if (have_cnn && cnn_fault_lk > max_lk) max_lk = cnn_fault_lk;
            if (have_proto && proto_fault_lk > max_lk) max_lk = proto_fault_lk;
            result->confidence = max_lk;
        }
        break;
    }
    case FD_STRATEGY_MAJORITY: {
        /* MAJORITY: average of all active models */
        float sum = 0.0f;
        int n = 0;
        if (have_cnn) { sum += cnn_fault_lk; n++; }
        if (have_proto) { sum += proto_fault_lk; n++; }
        if (run_multi) { sum += multi_fault_lk; n++; }
        result->confidence = n > 0 ? sum / n : 0.5f;
        break;
    }
    case FD_STRATEGY_VERIFY: {
        /* VERIFY: multiclass confirms CNN/Proto. Confidence follows
         * the decision chain — multi when it ran, CNN/Proto average when not. */
        if (run_multi)
            result->confidence = multi_fault_lk;
        else {
            float sum = 0.0f;
            int n = 0;
            if (have_cnn) { sum += cnn_fault_lk; n++; }
            if (have_proto) { sum += proto_fault_lk; n++; }
            result->confidence = n > 0 ? sum / n : 0.5f;
        }
        break;
    }
    case FD_STRATEGY_CNN:
        result->confidence = cnn_fault_lk;
        break;
    case FD_STRATEGY_PROTONET:
        result->confidence = proto_fault_lk;
        break;
    case FD_STRATEGY_MULTICLASS:
        result->confidence = multi_fault_lk;
        break;
    }

    /* Confidence should reflect how confident the final verdict is:
     * FAULT → confidence = fault likelihood (higher = more sure it's a fault)
     * OK    → confidence = 1 - fault likelihood (higher = more sure it's OK) */
    if (result->result == FD_CLASS_OK)
        result->confidence = 1.0f - result->confidence;

    /* Spatial heatmap: always run when enabled + protos loaded.
     * The 448x224 global classifiers (CNN/ProtoNet) use GAP which dilutes fault
     * signal for small/localized defects.  The spatial heatmap detects per-cell
     * and can boost the classification when global models miss. */
    if (cfg->heatmap_enabled &&
        (g_fd.prototypes_loaded || g_fd.spatial_protos_loaded) && spatial_buf) {
        if (pace_us > 0) usleep(pace_us);
        /* Resolve Z-dependent mask */
        pthread_mutex_lock(&g_fd.z_mutex);
        float cur_z = g_fd.current_z;
        pthread_mutex_unlock(&g_fd.z_mutex);
        fd_mask196_t active_mask = fd_get_mask_for_z(cfg, cur_z);
        int hm_ret = fd_run_heatmap(preprocessed, result, cfg, spatial_buf, active_mask, th.heatmap_coarse_wt, th.ema_alpha);
        if (hm_ret < 0) {
            fd_log("  Heatmap: skipped (%s)\n",
                   hm_ret == -2 ? "low memory" : "error");
            result->has_heatmap = 0;
        }

        /* Spatial boost: override OK→FAULT when heatmap shows strong localized
         * fault signal that the global classifiers missed.  The 448x224 wide FOV
         * dilutes GAP for sparse defects (spaghetti covering <20% of frame).
         *
         * Path 1 — Heatmap-only (all strategies):
         *   heatmap_max > 1.5 + >=3 strong cells.  No model gate needed.
         *   For tiny/distant defects where all global models miss (GAP dilution).
         *
         * Path 2 — Strategy-aware corroboration:
         *   heatmap_max > 0.45 + >=3 strong cells + model corroboration.
         *   Corroboration level matches strategy philosophy:
         *     Permissive (or/classify):       any model "leaning" (>50% threshold)
         *     Balanced (majority/verify):     any model above threshold
         *     Conservative (and/classify_and): CNN above + proto leaning
         *     Strict (all):                   both CNN and proto above threshold
         *     Single-model:                   that model "leaning" (>50% threshold)
         *
         * Calibrated thresholds (KS1, Feb 2026, coarse projection encoder):
         *   Empty bed:        heatmap 0.07-0.49,  CNN 0.01,  Proto lk 0.08
         *   Object on bed:    heatmap 0.75-1.24,  CNN 0.04,  Proto lk 0.10
         *   3 objects:        heatmap 0.49-0.61,  CNN 0.11,  Proto lk 0.09
         *   Tiny spaghetti:   heatmap 1.66-1.96,  CNN 0.07,  Proto lk 0.38
         *   Small spaghetti:  heatmap 2.09-2.11,  CNN 0.76,  Proto lk 0.95
         *   Big spaghetti:    heatmap 2.09-2.14,  CNN 0.81,  Proto lk 0.94
         * Path 1 gap: worst_OK=1.24 vs worst_FAULT=1.66 (margin=0.42) */
        if (result->has_heatmap && result->heatmap_max > th.boost_cell_th * 1.5f) {
            int strong_cells = 0, total_active = 0;
            int mask_on = !fd_mask_is_zero(&active_mask);
            for (int h = 0; h < result->spatial_h; h++) {
                for (int w = 0; w < result->spatial_w; w++) {
                    int idx = h * result->spatial_w + w;
                    if (mask_on && !fd_mask_test_bit(&active_mask, idx))
                        continue;
                    total_active++;
                    if (result->heatmap[h][w] > th.boost_cell_th)
                        strong_cells++;
                }
            }

            int do_boost = 0;
            const char *boost_path = "unknown";

            /* Path 1: Heatmap with model corroboration.
             * Coarse projection (cos_sim=-0.998): OK < 1.24, FAULT > 1.66.
             * Default 1.6 calibrated from live print (spurious hit at 1.54).
             * Multi has overwhelming weight — it's the most stable corroborator
             * for small/localized defects where CNN/Proto swing. */
            if (result->heatmap_max > heatmap_boost_th && strong_cells >= th.boost_min_cells) {
                int any_leaning = (have_cnn  && cnn_fault_lk  > cnn_th * th.boost_lean_factor) ||
                                  (have_proto && proto_fault_lk > th.boost_proto_lean) ||
                                  (run_multi && multi_fault_lk > th.boost_multi_lean);
                if (any_leaning) {
                    do_boost = 1;
                    boost_path = "heatmap-only";
                }
            }

            /* Path 2: Strategy-aware corroboration with moderate heatmap.
             * "above"   = model exceeds its detection threshold
             * "leaning" = model shows some fault signal (lean_factor of threshold)
             * Proto veto: if proto strongly says OK, block boost — proto is
             * the most reliable OK signal on empty bed. */
            if (!do_boost && strong_cells >= th.boost_min_cells) {
                int cnn_above   = have_cnn   && cnn_fault_lk   > cnn_th;
                int cnn_leaning = have_cnn   && cnn_fault_lk   > cnn_th * th.boost_lean_factor;
                int proto_above = have_proto  && proto_fault_lk > th.boost_proto_strong;
                int proto_lean  = have_proto  && proto_fault_lk > th.boost_proto_lean;
                /* Floor at boost_multi_lean: classify strategy uses multi_th=0.10
                 * for labeling, but boost corroboration needs real fault signal */
                float multi_lean_th = multi_th * th.boost_lean_factor;
                if (multi_lean_th < th.boost_multi_lean) multi_lean_th = th.boost_multi_lean;
                int multi_lean  = run_multi   && multi_fault_lk > multi_lean_th;
                int proto_veto  = have_proto  && proto_fault_lk < th.boost_proto_veto;

                switch (cfg->strategy) {
                case FD_STRATEGY_OR:
                case FD_STRATEGY_CLASSIFY:
                    /* Permissive: any model leaning toward fault,
                     * but proto veto blocks if proto strongly says OK */
                    if (!proto_veto && (cnn_leaning || proto_lean || multi_lean)) {
                        do_boost = 1; boost_path = "or+heatmap";
                    }
                    break;
                case FD_STRATEGY_MAJORITY:
                    /* Heatmap as 3rd voter: heatmap + one model = 2-of-3 */
                    if (cnn_above || proto_above) {
                        do_boost = 1; boost_path = "majority+heatmap";
                    }
                    break;
                case FD_STRATEGY_VERIFY:
                    /* Override multi veto: primary model above threshold */
                    if (cnn_above || proto_above) {
                        do_boost = 1; boost_path = "verify+heatmap";
                    }
                    break;
                case FD_STRATEGY_AND:
                case FD_STRATEGY_CLASSIFY_AND:
                    /* Conservative: CNN above threshold + proto leaning */
                    if (cnn_above && (proto_lean || !have_proto)) {
                        do_boost = 1; boost_path = "and+heatmap";
                    }
                    break;
                case FD_STRATEGY_ALL:
                    /* Strict: both models above threshold */
                    if (cnn_above && proto_above) {
                        do_boost = 1; boost_path = "all+heatmap";
                    }
                    break;
                case FD_STRATEGY_CNN:
                    /* Single-model: CNN leaning + heatmap */
                    if (cnn_leaning) {
                        do_boost = 1; boost_path = "cnn+heatmap";
                    }
                    break;
                case FD_STRATEGY_PROTONET:
                    /* Single-model: proto leaning + heatmap */
                    if (proto_lean) {
                        do_boost = 1; boost_path = "proto+heatmap";
                    }
                    break;
                case FD_STRATEGY_MULTICLASS:
                    /* Single-model: multi leaning + heatmap */
                    if (multi_lean) {
                        do_boost = 1; boost_path = "multi+heatmap";
                    }
                    break;
                }
            }

            if (do_boost) {
                result->boost_active = 1;
                result->boost_strong_cells = strong_cells;
                result->boost_total_cells = total_active;
                if (result->result == FD_CLASS_OK) {
                    result->boost_overrode = 1;
                    result->result = FD_CLASS_FAULT;
                    fd_log("  Spatial BOOST: OK->FAULT (max=%.2f, %d/%d strong cells, path=%s)\n",
                           result->heatmap_max, strong_cells, total_active, boost_path);

                    /* Run multiclass for fault classification if not already run */
                    if (have_multi && !run_multi) {
                        memset(&model_result, 0, sizeof(model_result));
                        if (fd_run_multiclass(preprocessed, &model_result, multi_th, cfg, th.ema_alpha) == 0) {
                            result->multi_ran = 1;
                            result->multi_ms = model_result.multi_ms;
                            result->fault_class = model_result.fault_class;
                            result->multi_fault_lk = model_result.confidence;
                            snprintf(result->fault_class_name,
                                     sizeof(result->fault_class_name), "%s",
                                     model_result.fault_class_name);
                            fd_log("  Multi (post-boost): class=%s conf=%.3f\n",
                                   result->fault_class_name, model_result.confidence);
                        }
                    }

                    /* Boost confidence: multi-dominant, amplified by heatmap
                     * strength above boost threshold.  Multi is the most stable
                     * signal for heatmap-triggered detections; heatmap ratio
                     * scales it proportionally to spatial evidence strength. */
                    if (result->multi_ran && result->multi_fault_lk > 0.0f) {
                        float hm_amp = result->heatmap_max / heatmap_boost_th;
                        if (hm_amp > th.boost_amp_cap) hm_amp = th.boost_amp_cap;
                        result->confidence = result->multi_fault_lk * hm_amp;
                        if (result->confidence > th.boost_conf_cap) result->confidence = th.boost_conf_cap;
                    } else {
                        float boost_conf = 0.0f;
                        if (have_cnn && cnn_fault_lk > boost_conf) boost_conf = cnn_fault_lk;
                        if (have_proto && proto_fault_lk > boost_conf) boost_conf = proto_fault_lk;
                        if (boost_conf < 0.50f) boost_conf = 0.50f;
                        result->confidence = boost_conf;
                    }
                }
            }
        }
    }

    result->total_ms = (float)(fd_get_time_ms() - t0);
    return 0;
}

/* ============================================================================
 * Detection thread
 * ============================================================================ */

static void fd_set_state(fd_status_t status, const fd_result_t *result,
                          const char *err_msg)
{
    pthread_mutex_lock(&g_fd.state_mutex);
    g_fd.state.status = status;
    if (result)
        g_fd.state.last_result = *result;
    if (err_msg)
        snprintf(g_fd.state.error_msg, sizeof(g_fd.state.error_msg),
                 "%s", err_msg);
    pthread_mutex_unlock(&g_fd.state_mutex);
}

static void *fd_thread_func(void *arg)
{
    (void)arg;
    fd_log("Detection thread started\n");

    fd_buzzer_init();

    uint8_t *preprocessed = (uint8_t *)malloc(FD_MODEL_INPUT_BYTES);
    if (!preprocessed) {
        fd_err("Failed to allocate preprocess buffer\n");
        fd_set_state(FD_STATUS_ERROR, NULL, "malloc failed");
        return NULL;
    }

    /* Persistent spatial buffer — allocated for max possible size, reused each cycle.
     * Max: 14x28x1024 = 401408 floats = ~1.5MB (covers 7x14x1024 and 14x28x232) */
    int spatial_buf_size = FD_SPATIAL_H_MAX * FD_SPATIAL_W_MAX * FD_SPATIAL_EMB_MAX;
    float *spatial_buf = (float *)malloc(spatial_buf_size * sizeof(float));
    if (!spatial_buf) {
        fd_log("Warning: spatial buffer alloc failed, heatmap disabled\n");
    }

    int consecutive_ok = 0;
    int use_verify_interval = 0;
    uint64_t last_led_check = 0;
    uint64_t last_led_keepalive = 0;

    while (!g_fd.thread_stop) {
        /* Check for pending prototype computation (runs before normal FD) */
        {
            pthread_mutex_lock(&g_proto.mutex);
            int proto_pending = (g_proto.progress.state == PROTO_COMPUTE_PENDING);
            pthread_mutex_unlock(&g_proto.mutex);
            if (proto_pending) {
                fd_set_state(FD_STATUS_DISABLED, NULL, "computing prototypes");
                fd_do_proto_computation();
                /* Reset EMA state since prototypes changed */
                g_fd.cnn_ema_init = 0;
                g_fd.multi_ema_init = 0;
                g_fd.heatmap_ema_init = 0;
                continue;
            }
        }

        /* Get current config */
        pthread_mutex_lock(&g_fd.config_mutex);
        fd_config_t cfg = g_fd.config;
        pthread_mutex_unlock(&g_fd.config_mutex);

        if (!cfg.enabled) {
            fd_set_state(FD_STATUS_DISABLED, NULL, NULL);
            usleep(1000000);
            continue;
        }

        /* Sleep for the appropriate interval */
        int interval = use_verify_interval ? cfg.verify_interval_s
                                           : cfg.interval_s;
        for (int i = 0; i < interval * 10 && !g_fd.thread_stop; i++)
            usleep(100000);  /* 100ms chunks for responsive shutdown */

        if (g_fd.thread_stop) break;

        /* Skip cycle while timelapse is encoding (VENC recovery uses CMA) */
        {
            TimelapseEncodeStatus tl_status = timelapse_get_encode_status();
            if (tl_status == TL_ENCODE_PENDING || tl_status == TL_ENCODE_RUNNING) {
                fd_log("Skipping cycle: timelapse encoding in progress\n");
                continue;
            }
        }

        /* Check available memory */
        int avail_mb = fd_get_available_memory_mb();
        if (avail_mb > 0 && avail_mb < cfg.min_free_mem_mb) {
            fd_set_state(FD_STATUS_MEM_LOW, NULL, "memory low");
            fd_log("Skipping cycle: %d MB available < %d MB threshold\n",
                   avail_mb, cfg.min_free_mem_mb);
            continue;
        }

        /* LED keepalive — mandatory ON every 5 min to prevent printer standby,
         * query+wait every 60s to detect LED-off and allow camera re-exposure */
        {
            struct timeval tv_now;
            gettimeofday(&tv_now, NULL);
            uint64_t now_ms = (uint64_t)tv_now.tv_sec * 1000 + tv_now.tv_usec / 1000;

            /* Mandatory LED ON every 5 min (standby timeout is ~10 min) */
            if (now_ms - last_led_keepalive >= 300000) {
                last_led_keepalive = now_ms;
                mqtt_send_led(1, 100);
                fd_log("LED keepalive (5min)\n");
            }

            /* Check LED state every 60s — if off, turn on and wait for exposure */
            if (now_ms - last_led_check >= 60000) {
                last_led_check = now_ms;
                int led = mqtt_query_led(1000);
                if (led == 0) {
                    mqtt_send_led(1, 100);
                    last_led_keepalive = now_ms;
                    fd_log("LED was off, turning on and waiting 3s for exposure\n");
                    for (int i = 0; i < 30 && !g_fd.thread_stop; i++)
                        usleep(100000);  /* 3s in 100ms chunks */
                }
            }
        }

        /* Request frame from main capture loop */
        pthread_mutex_lock(&g_fd.frame_mutex);
        g_fd.need_frame = 1;

        /* Wait for frame with timeout (3 seconds) */
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += 3;
        while (g_fd.need_frame && !g_fd.thread_stop) {
            int rc = pthread_cond_timedwait(&g_fd.frame_cond,
                                             &g_fd.frame_mutex, &ts);
            if (rc != 0) break;  /* timeout or error */
        }

        if (g_fd.thread_stop || g_fd.need_frame || g_fd.jpeg_size == 0) {
            pthread_mutex_unlock(&g_fd.frame_mutex);
            continue;
        }

        /* Copy JPEG data under lock */
        size_t jpeg_size = g_fd.jpeg_size;
        uint8_t *jpeg_copy = (uint8_t *)malloc(jpeg_size);
        if (jpeg_copy)
            memcpy(jpeg_copy, g_fd.jpeg_buf, jpeg_size);

        /* Retain copy for UI overlay (separate mutex, no contention) */
        if (jpeg_copy && jpeg_size <= sizeof(g_fd.fd_frame_buf)) {
            pthread_mutex_lock(&g_fd.fd_frame_mutex);
            memcpy(g_fd.fd_frame_buf, g_fd.jpeg_buf, jpeg_size);
            g_fd.fd_frame_size = jpeg_size;
            g_fd.fd_frame_cycle = g_fd.state.cycle_count;
            pthread_mutex_unlock(&g_fd.fd_frame_mutex);
        }
        pthread_mutex_unlock(&g_fd.frame_mutex);

        if (!jpeg_copy) continue;

        fd_set_state(FD_STATUS_ACTIVE, NULL, NULL);
        int pace_us = cfg.pace_ms * 1000;

        /* Decode JPEG (with TurboJPEG scaled decode) */
        fd_image_t img = {0};
        if (fd_decode_jpeg(jpeg_copy, jpeg_size, &img) < 0) {
            free(jpeg_copy);
            fd_set_state(FD_STATUS_ERROR, NULL, "JPEG decode failed");
            continue;
        }
        free(jpeg_copy);

        /* Compute center-crop region from decoded image dimensions.
         * Scale = max(256/h, 512/w) to ensure >= 512x256, then crop 448x224 */
        if (img.width > 0 && img.height > 0) {
            float sc_h = 256.0f / (float)img.height;
            float sc_w = 512.0f / (float)img.width;
            float sc = sc_h > sc_w ? sc_h : sc_w;
            float rw = img.width * sc;
            float rh = img.height * sc;
            g_fd.crop_w = (float)FD_MODEL_INPUT_WIDTH / rw;
            g_fd.crop_h = (float)FD_MODEL_INPUT_HEIGHT / rh;
            g_fd.crop_x = (1.0f - g_fd.crop_w) * 0.5f;
            g_fd.crop_y = (1.0f - g_fd.crop_h) * 0.5f;
            g_fd.crop_valid = 1;
        }

        if (pace_us > 0) usleep(pace_us);

        /* Fused resize+crop+grayscale (single pass, no intermediate alloc) */
        if (fd_preprocess(&img, preprocessed) < 0) {
            free(img.data);
            fd_set_state(FD_STATUS_ERROR, NULL, "preprocess failed");
            continue;
        }
        free(img.data);

        if (pace_us > 0) usleep(pace_us);

        /* Run detection (pacing between models handled inside) */
        fd_result_t result;
        int det_ret = fd_run_detection(preprocessed, &result, &cfg, spatial_buf);
        if (det_ret < 0) {
            if (det_ret == -2)
                fd_set_state(FD_STATUS_MEM_LOW, NULL, "CMA alloc failed");
            else
                fd_set_state(FD_STATUS_ERROR, NULL, "model load failed");
            continue;  /* Skip cycle entirely */
        }

        /* Diagnostic: log center cell + EMA state (debug_logging only) */
        if (cfg.debug_logging && result.has_heatmap &&
            result.spatial_h > 0 && result.spatial_w > 0) {
            int ch = result.spatial_h / 2, cw = result.spatial_w / 2;
            fd_log("  HEATMAP_DIAG: center[%d][%d]=%.3f max=%.3f cnn_ema=[%.3f,%.3f]\n",
                   ch, cw, result.heatmap[ch][cw], result.heatmap_max,
                   g_fd.cnn_ema_logits[0], g_fd.cnn_ema_logits[1]);
        }

        /* Attach center-crop region to result */
        result.crop_x = g_fd.crop_x;
        result.crop_y = g_fd.crop_y;
        result.crop_w = g_fd.crop_w;
        result.crop_h = g_fd.crop_h;

        /* Update state */
        pthread_mutex_lock(&g_fd.state_mutex);
        g_fd.state.status = FD_STATUS_ENABLED;
        g_fd.state.last_result = result;
        g_fd.state.last_check_time = (uint64_t)time(NULL);
        g_fd.state.cycle_count++;
        g_fd.state.error_msg[0] = '\0';
        pthread_mutex_unlock(&g_fd.state_mutex);

        /* Buzzer alert on fault */
        if (result.result == FD_CLASS_FAULT && cfg.beep_pattern > 0)
            fd_play_pattern(cfg.beep_pattern);

        /* Dual interval logic */
        if (result.result == FD_CLASS_FAULT) {
            use_verify_interval = 1;
            consecutive_ok = 0;
        } else {
            if (use_verify_interval) {
                consecutive_ok++;
                if (consecutive_ok >= 3) {
                    use_verify_interval = 0;
                    consecutive_ok = 0;
                }
            }
        }

        fd_log("Cycle %llu: %s (conf=%.2f, %s, %.0fms)\n",
               (unsigned long long)g_fd.state.cycle_count,
               result.result == FD_CLASS_FAULT ? "FAULT" : "OK",
               result.confidence,
               fd_strategy_name(cfg.strategy),
               result.total_ms);
    }

    free(preprocessed);
    free(spatial_buf);
    fd_buzzer_cleanup();
    fd_log("Detection thread stopped\n");
    return NULL;
}

/* ============================================================================
 * Z-dependent mask helpers
 * ============================================================================ */

/* Binary search: find largest entry where z_mm <= current_z.
 * Falls back to heatmap_mask when z_mask_count == 0. */
static fd_mask196_t fd_get_mask_for_z(const fd_config_t *cfg, float z)
{
    if (cfg->z_mask_count <= 0)
        return cfg->heatmap_mask;

    /* Binary search for largest z_mm <= z */
    int lo = 0, hi = cfg->z_mask_count - 1;
    int best = -1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (cfg->z_masks[mid].z_mm <= z) {
            best = mid;
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }

    if (best >= 0)
        return cfg->z_masks[best].mask;

    /* z is below all entries — use first entry */
    return cfg->z_masks[0].mask;
}

/* ============================================================================
 * Public API
 * ============================================================================ */

int fault_detect_init(const char *models_base_dir)
{
    memset(&g_fd, 0, sizeof(g_fd));
    pthread_mutex_init(&g_fd.config_mutex, NULL);
    pthread_mutex_init(&g_fd.state_mutex, NULL);
    pthread_mutex_init(&g_fd.frame_mutex, NULL);
    pthread_mutex_init(&g_fd.fd_frame_mutex, NULL);
    pthread_mutex_init(&g_fd.z_mutex, NULL);
    pthread_cond_init(&g_fd.frame_cond, NULL);

    snprintf(g_fd.models_base_dir, sizeof(g_fd.models_base_dir),
             "%s", models_base_dir);

    /* Set initial result to OK (0 = FD_CLASS_FAULT) */
    g_fd.state.last_result.result = FD_CLASS_OK;
    snprintf(g_fd.state.last_result.fault_class_name,
             sizeof(g_fd.state.last_result.fault_class_name), "-");

    /* Set defaults */
    g_fd.config.interval_s = 5;
    g_fd.config.verify_interval_s = 2;
    g_fd.config.min_free_mem_mb = 20;
    g_fd.config.strategy = FD_STRATEGY_OR;

    /* Try loading RKNN runtime */
    if (fd_rknn_load() < 0) {
        g_fd.state.status = FD_STATUS_NO_NPU;
        snprintf(g_fd.state.error_msg, sizeof(g_fd.state.error_msg),
                 "NPU not available");
        fd_log("Fault detection initialized (NPU not available)\n");
    } else {
        g_fd.state.status = FD_STATUS_DISABLED;
        fd_log("Fault detection initialized (NPU available)\n");
    }

    g_fd.initialized = 1;
    return 0;
}

int fault_detect_start(void)
{
    if (!g_fd.initialized || !g_rknn.handle) return -1;
    if (g_fd.thread_running) return 0;  /* already running */

    /* Get config for validation */
    pthread_mutex_lock(&g_fd.config_mutex);
    fd_config_t cfg = g_fd.config;
    pthread_mutex_unlock(&g_fd.config_mutex);

    if (!cfg.enabled) return -1;

    /* Verify model files exist (no RKNN init — that's done in detection thread
     * to avoid CMA conflicts with the running hardware encoder) */
    if (cfg.cnn_enabled || cfg.strategy == FD_STRATEGY_CNN) {
        char path[512];
        if (fd_resolve_model_path(FD_MODEL_CNN, cfg.model_set,
                                   path, sizeof(path), &cfg) < 0) {
            fd_err("CNN model not found in set: %s\n", cfg.model_set);
            fd_set_state(FD_STATUS_ERROR, NULL, "CNN model not found");
            return -1;
        }
    }
    if (cfg.proto_enabled || cfg.strategy == FD_STRATEGY_PROTONET) {
        char path[512];
        if (fd_resolve_model_path(FD_MODEL_PROTONET, cfg.model_set,
                                   path, sizeof(path), &cfg) < 0) {
            fd_err("ProtoNet model not found in set: %s\n", cfg.model_set);
            fd_set_state(FD_STATUS_ERROR, NULL, "ProtoNet model not found");
            return -1;
        }
        const char *proto_file = cfg.proto_prototypes[0] ?
                                  cfg.proto_prototypes : "prototypes.bin";
        char proto_path[512];
        snprintf(proto_path, sizeof(proto_path),
                 "%s/%s/protonet/%s",
                 g_fd.models_base_dir, cfg.model_set, proto_file);
        if (access(proto_path, R_OK) != 0) {
            fd_err("ProtoNet prototypes not found: %s\n", proto_path);
            fd_set_state(FD_STATUS_ERROR, NULL, "prototypes.bin not found");
            return -1;
        }
    }
    if (cfg.multi_enabled || cfg.strategy == FD_STRATEGY_MULTICLASS) {
        char path[512];
        if (fd_resolve_model_path(FD_MODEL_MULTICLASS, cfg.model_set,
                                   path, sizeof(path), &cfg) < 0) {
            fd_err("Multiclass model not found in set: %s\n", cfg.model_set);
            fd_set_state(FD_STATUS_ERROR, NULL, "Multiclass model not found");
            return -1;
        }
    }
    fd_log("Model files verified (set: %s)\n", cfg.model_set);

    /* Start thread */
    g_fd.thread_stop = 0;
    if (pthread_create(&g_fd.thread, NULL, fd_thread_func, NULL) != 0) {
        fd_err("Failed to create detection thread\n");
        fd_set_state(FD_STATUS_ERROR, NULL, "thread creation failed");
        return -1;
    }
    g_fd.thread_running = 1;
    fd_set_state(FD_STATUS_ENABLED, NULL, NULL);
    return 0;
}

void fault_detect_stop(void)
{
    if (!g_fd.thread_running) return;

    g_fd.thread_stop = 1;

    /* Wake up frame wait */
    pthread_mutex_lock(&g_fd.frame_mutex);
    g_fd.need_frame = 0;
    pthread_cond_signal(&g_fd.frame_cond);
    pthread_mutex_unlock(&g_fd.frame_mutex);

    pthread_join(g_fd.thread, NULL);
    g_fd.thread_running = 0;
    fd_set_state(FD_STATUS_DISABLED, NULL, NULL);
}

void fault_detect_cleanup(void)
{
    if (!g_fd.initialized) return;

    fault_detect_stop();
    fd_rknn_unload();

    pthread_mutex_destroy(&g_fd.config_mutex);
    pthread_mutex_destroy(&g_fd.state_mutex);
    pthread_mutex_destroy(&g_fd.frame_mutex);
    pthread_mutex_destroy(&g_fd.fd_frame_mutex);
    pthread_mutex_destroy(&g_fd.z_mutex);
    pthread_cond_destroy(&g_fd.frame_cond);

    g_fd.initialized = 0;
}

int fault_detect_warmup(void)
{
    if (!g_fd.initialized || !g_rknn.handle) return -1;

    pthread_mutex_lock(&g_fd.config_mutex);
    fd_config_t cfg = g_fd.config;
    pthread_mutex_unlock(&g_fd.config_mutex);

    if (!cfg.enabled) return 0;

    /* Find the largest enabled model file to pre-allocate CMA */
    fd_model_class_t classes[] = {
        FD_MODEL_CNN, FD_MODEL_PROTONET, FD_MODEL_MULTICLASS,
        FD_MODEL_SPATIAL, FD_MODEL_SPATIAL_COARSE
    };
    const char *names[] = {
        "CNN", "ProtoNet", "Multiclass", "Spatial", "SpatialCoarse"
    };
    int enables[] = {
        cfg.cnn_enabled, cfg.proto_enabled, cfg.multi_enabled,
        cfg.heatmap_enabled, cfg.heatmap_enabled
    };

    char biggest_path[512] = {0};
    const char *biggest_name = NULL;
    long biggest_size = 0;

    for (int i = 0; i < 5; i++) {
        if (!enables[i]) continue;
        char path[512];
        if (fd_resolve_model_path(classes[i], cfg.model_set,
                                   path, sizeof(path), &cfg) < 0)
            continue;
        struct stat st;
        if (stat(path, &st) == 0 && st.st_size > biggest_size) {
            biggest_size = st.st_size;
            snprintf(biggest_path, sizeof(biggest_path), "%s", path);
            biggest_name = names[i];
        }
    }

    if (!biggest_name) {
        fd_log("CMA warmup: no models found\n");
        return 0;
    }

    fd_log("CMA warmup: loading %s (%ld KB) to pre-allocate CMA...\n",
           biggest_name, biggest_size / 1024);

    fd_rknn_model_t model;
    if (fd_model_init(&model, biggest_path) == 0) {
        fd_model_release(&model);
        fd_log("CMA warmup: %s loaded/released OK\n", biggest_name);
        return 1;
    }

    fd_log("CMA warmup: %s failed to load\n", biggest_name);
    return 0;
}

int fault_detect_needs_frame(void)
{
    return g_fd.initialized && g_fd.need_frame;
}

void fault_detect_feed_jpeg(const uint8_t *data, size_t size)
{
    /* Quick volatile check — no lock needed */
    if (!g_fd.need_frame) return;
    if (size > sizeof(g_fd.jpeg_buf)) return;

    pthread_mutex_lock(&g_fd.frame_mutex);
    if (g_fd.need_frame) {
        memcpy(g_fd.jpeg_buf, data, size);
        g_fd.jpeg_size = size;
        g_fd.need_frame = 0;
        pthread_cond_signal(&g_fd.frame_cond);
    }
    pthread_mutex_unlock(&g_fd.frame_mutex);
}

fd_state_t fault_detect_get_state(void)
{
    fd_state_t state;
    pthread_mutex_lock(&g_fd.state_mutex);
    state = g_fd.state;
    pthread_mutex_unlock(&g_fd.state_mutex);
    return state;
}

fd_config_t fault_detect_get_config(void)
{
    fd_config_t cfg;
    pthread_mutex_lock(&g_fd.config_mutex);
    cfg = g_fd.config;
    pthread_mutex_unlock(&g_fd.config_mutex);
    return cfg;
}

void fault_detect_set_config(const fd_config_t *config)
{
    pthread_mutex_lock(&g_fd.config_mutex);
    g_fd.config = *config;
    pthread_mutex_unlock(&g_fd.config_mutex);

    /* Invalidate prototypes cache, model cache, and EMA state when config changes */
    g_fd.prototypes_loaded = 0;
    g_fd.spatial_protos_loaded = 0;
    g_fd.cnn_ema_init = 0;
    g_fd.multi_ema_init = 0;
    g_fd.heatmap_ema_init = 0;
}

void fault_detect_set_current_z(float z_mm)
{
    pthread_mutex_lock(&g_fd.z_mutex);
    g_fd.current_z = z_mm;
    pthread_mutex_unlock(&g_fd.z_mutex);
}

void fault_detect_set_z_masks(const fd_z_mask_entry_t *entries, int count)
{
    if (count < 0) count = 0;
    if (count > FD_Z_MASK_MAX_ENTRIES) count = FD_Z_MASK_MAX_ENTRIES;

    pthread_mutex_lock(&g_fd.config_mutex);
    if (entries && count > 0)
        memcpy(g_fd.config.z_masks, entries, count * sizeof(fd_z_mask_entry_t));
    g_fd.config.z_mask_count = count;
    pthread_mutex_unlock(&g_fd.config_mutex);

    fd_log("Z-masks: %d entries loaded\n", count);
}

/* Check if a model file exists in {set_path}/{class_dir}/{filename} */
static int fd_check_model_file(const char *set_path, const char *class_dir,
                                const char *filename)
{
    char path[512];
    if (filename && filename[0]) {
        snprintf(path, sizeof(path), "%s/%s/%s", set_path, class_dir, filename);
        return access(path, R_OK) == 0 ? 0 : -1;
    }
    /* No specific filename — scan for any .rknn file */
    char dir_path[512];
    snprintf(dir_path, sizeof(dir_path), "%s/%s", set_path, class_dir);
    DIR *dir = opendir(dir_path);
    if (!dir) return -1;
    struct dirent *ent;
    int found = -1;
    while ((ent = readdir(dir)) != NULL) {
        size_t len = strlen(ent->d_name);
        if (len > 5 && strcmp(ent->d_name + len - 5, ".rknn") == 0) {
            found = 0;
            break;
        }
    }
    closedir(dir);
    return found;
}

/* Helper: get string from cJSON, return NULL if missing */
static const char *fd_json_str(const cJSON *obj, const char *key)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (item && cJSON_IsString(item) && item->valuestring)
        return item->valuestring;
    return NULL;
}

/* Helper: get float from cJSON, return 0 if missing */
static float fd_json_float(const cJSON *obj, const char *key)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (item && cJSON_IsNumber(item))
        return (float)item->valuedouble;
    return 0.0f;
}

/* Parse metadata.json for a model set */
static void fd_parse_set_metadata(fd_model_set_t *s)
{
    char meta_path[512];
    snprintf(meta_path, sizeof(meta_path), "%s/metadata.json", s->path);

    FILE *f = fopen(meta_path, "r");
    if (!f) return;

    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (fsize <= 0 || fsize > 32 * 1024) { fclose(f); return; }

    char *buf = (char *)malloc(fsize + 1);
    if (!buf) { fclose(f); return; }
    size_t nread = fread(buf, 1, fsize, f);
    fclose(f);
    buf[nread] = '\0';

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) return;

    /* Top-level fields */
    const char *name = fd_json_str(root, "name");
    if (name)
        snprintf(s->display_name, sizeof(s->display_name), "%s", name);
    const char *desc = fd_json_str(root, "description");
    if (desc)
        snprintf(s->description, sizeof(s->description), "%s", desc);

    /* Models object */
    const cJSON *models = cJSON_GetObjectItemCaseSensitive(root, "models");
    if (models && cJSON_IsObject(models)) {
        const cJSON *cnn = cJSON_GetObjectItemCaseSensitive(models, "cnn");
        if (cnn) {
            const char *dn = fd_json_str(cnn, "display_name");
            if (dn) snprintf(s->cnn_display_name, sizeof(s->cnn_display_name), "%s", dn);
            const char *fn = fd_json_str(cnn, "file");
            if (fn) snprintf(s->cnn_file, sizeof(s->cnn_file), "%s", fn);
        }
        const cJSON *proto = cJSON_GetObjectItemCaseSensitive(models, "protonet");
        if (proto) {
            const char *dn = fd_json_str(proto, "display_name");
            if (dn) snprintf(s->proto_display_name, sizeof(s->proto_display_name), "%s", dn);
            const char *fn = fd_json_str(proto, "file");
            if (fn) snprintf(s->proto_file, sizeof(s->proto_file), "%s", fn);
            const char *pf = fd_json_str(proto, "prototypes");
            if (pf) snprintf(s->proto_prototypes, sizeof(s->proto_prototypes), "%s", pf);
            const char *spf = fd_json_str(proto, "spatial_prototypes");
            if (spf) snprintf(s->proto_spatial_prototypes, sizeof(s->proto_spatial_prototypes), "%s", spf);
        }
        const cJSON *multi = cJSON_GetObjectItemCaseSensitive(models, "multiclass");
        if (multi) {
            const char *dn = fd_json_str(multi, "display_name");
            if (dn) snprintf(s->multi_display_name, sizeof(s->multi_display_name), "%s", dn);
            const char *fn = fd_json_str(multi, "file");
            if (fn) snprintf(s->multi_file, sizeof(s->multi_file), "%s", fn);
        }
    }

    /* Profiles object (ordered — iterate keys) */
    const cJSON *profiles = cJSON_GetObjectItemCaseSensitive(root, "profiles");
    if (profiles && cJSON_IsObject(profiles)) {
        const cJSON *prof;
        cJSON_ArrayForEach(prof, profiles) {
            if (s->num_profiles >= FD_MAX_PROFILES) break;
            fd_threshold_profile_t *p = &s->profiles[s->num_profiles];
            snprintf(p->name, sizeof(p->name), "%s", prof->string);
            const char *pdesc = fd_json_str(prof, "description");
            if (pdesc) snprintf(p->description, sizeof(p->description), "%s", pdesc);
            p->cnn_threshold = fd_json_float(prof, "cnn_threshold");
            p->cnn_dynamic_threshold = fd_json_float(prof, "cnn_dynamic_threshold");
            p->proto_threshold = fd_json_float(prof, "proto_threshold");
            p->proto_dynamic_trigger = fd_json_float(prof, "proto_dynamic_trigger");
            p->multi_threshold = fd_json_float(prof, "multi_threshold");
            p->heatmap_boost_threshold = fd_json_float(prof, "heatmap_boost_threshold");
            /* Advanced boost tuning */
            p->boost_min_cells = (int)fd_json_float(prof, "boost_min_cells");
            p->boost_cell_threshold = fd_json_float(prof, "boost_cell_threshold");
            p->boost_lean_factor = fd_json_float(prof, "boost_lean_factor");
            p->boost_proto_lean = fd_json_float(prof, "boost_proto_lean");
            p->boost_multi_lean = fd_json_float(prof, "boost_multi_lean");
            p->boost_proto_veto = fd_json_float(prof, "boost_proto_veto");
            p->boost_proto_strong = fd_json_float(prof, "boost_proto_strong");
            p->boost_amplifier_cap = fd_json_float(prof, "boost_amplifier_cap");
            p->boost_confidence_cap = fd_json_float(prof, "boost_confidence_cap");
            p->ema_alpha = fd_json_float(prof, "ema_alpha");
            p->heatmap_coarse_weight = fd_json_float(prof, "heatmap_coarse_weight");
            s->num_profiles++;
        }
    }

    cJSON_Delete(root);
}

int fault_detect_scan_sets(fd_model_set_t *sets, int max_sets)
{
    int count = 0;
    DIR *dir = opendir(g_fd.models_base_dir);
    if (!dir) return 0;

    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL && count < max_sets) {
        if (ent->d_name[0] == '.') continue;

        char sub_path[512];
        snprintf(sub_path, sizeof(sub_path), "%s/%s",
                 g_fd.models_base_dir, ent->d_name);
        struct stat st;
        if (stat(sub_path, &st) != 0 || !S_ISDIR(st.st_mode))
            continue;

        fd_model_set_t *s = &sets[count];
        memset(s, 0, sizeof(*s));
        snprintf(s->dir_name, sizeof(s->dir_name), "%s", ent->d_name);
        snprintf(s->path, sizeof(s->path), "%s", sub_path);

        /* Check which model types exist (default filenames) */
        s->has_cnn = (fd_check_model_file(sub_path, "cnn", "model.rknn") == 0);
        s->has_protonet = (fd_check_model_file(sub_path, "protonet", "encoder.rknn") == 0);
        s->has_multiclass = (fd_check_model_file(sub_path, "multiclass", NULL) == 0);

        /* Parse metadata.json if present */
        fd_parse_set_metadata(s);

        /* Re-check with overridden filenames from metadata */
        if (s->cnn_file[0])
            s->has_cnn = (fd_check_model_file(sub_path, "cnn", s->cnn_file) == 0);
        if (s->proto_file[0])
            s->has_protonet = (fd_check_model_file(sub_path, "protonet", s->proto_file) == 0);
        if (s->multi_file[0])
            s->has_multiclass = (fd_check_model_file(sub_path, "multiclass", s->multi_file) == 0);

        /* Must have at least one model type */
        if (!s->has_cnn && !s->has_protonet && !s->has_multiclass)
            continue;

        /* Default display_name to dir_name if metadata.json missing */
        if (!s->display_name[0])
            snprintf(s->display_name, sizeof(s->display_name), "%s", ent->d_name);

        count++;
    }
    closedir(dir);
    return count;
}

int fault_detect_npu_available(void)
{
    return g_rknn.handle != NULL ? 1 : 0;
}

int fault_detect_installed(void)
{
    struct stat st;
    return (stat(g_fd.models_base_dir, &st) == 0 && S_ISDIR(st.st_mode)) ? 1 : 0;
}

void fault_detect_get_spatial_dims(int *h, int *w)
{
    if (g_fd.spatial_protos_loaded) {
        if (h) *h = g_fd.spatial_h;
        if (w) *w = g_fd.spatial_w;
    } else {
        /* Default to max grid size so masks cover all cells even before
         * models load.  Old default of 7x7 caused a 49-bit mask that
         * excluded rows 2-13 of the 14x28 grid. */
        if (h) *h = FD_SPATIAL_H_MAX;
        if (w) *w = FD_SPATIAL_W_MAX;
    }
}

void fault_detect_get_crop(float *x, float *y, float *w, float *h)
{
    if (g_fd.crop_valid) {
        if (x) *x = g_fd.crop_x;
        if (y) *y = g_fd.crop_y;
        if (w) *w = g_fd.crop_w;
        if (h) *h = g_fd.crop_h;
    } else {
        if (x) *x = 0;
        if (y) *y = 0;
        if (w) *w = 1;
        if (h) *h = 1;
    }
}

size_t fault_detect_get_fd_frame(uint8_t *buf, size_t max_size, uint64_t *cycle_out)
{
    size_t copied = 0;
    pthread_mutex_lock(&g_fd.fd_frame_mutex);
    if (g_fd.fd_frame_size > 0 && g_fd.fd_frame_size <= max_size) {
        memcpy(buf, g_fd.fd_frame_buf, g_fd.fd_frame_size);
        copied = g_fd.fd_frame_size;
        if (cycle_out) *cycle_out = g_fd.fd_frame_cycle;
    }
    pthread_mutex_unlock(&g_fd.fd_frame_mutex);
    return copied;
}

/* ============================================================================
 * Prototype Management — File Helpers
 * ============================================================================ */

static int fd_mkdir_p(const char *path)
{
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
    return mkdir(tmp, 0755) == 0 || errno == EEXIST ? 0 : -1;
}

static int fd_rmdir_recursive(const char *path)
{
    /* Use rm -rf for reliability on FAT32/busybox.
     * system() returns waitpid status, not exit code — use WEXITSTATUS. */
    if (strstr(path, "..") || !path[0]) return -1;
    char cmd[600];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", path);
    int ret = system(cmd);
    return (ret != -1 && WEXITSTATUS(ret) == 0) ? 0 : -1;
}

static int fd_copy_file(const char *src, const char *dst)
{
    FILE *in = fopen(src, "rb");
    if (!in) return -1;
    FILE *out = fopen(dst, "wb");
    if (!out) { fclose(in); return -1; }
    char buf[8192];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0)
        fwrite(buf, 1, n, out);
    fclose(in);
    fclose(out);
    return 0;
}

static int fd_count_jpegs(const char *dir)
{
    DIR *d = opendir(dir);
    if (!d) return 0;
    int count = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        size_t len = strlen(ent->d_name);
        if (len > 4 && (strcasecmp(ent->d_name + len - 4, ".jpg") == 0 ||
                        strcasecmp(ent->d_name + len - 5, ".jpeg") == 0))
            count++;
    }
    closedir(d);
    return count;
}

/* Collect JPEG paths sorted alphabetically. Caller must free each entry and the array. */
static char **fd_collect_jpegs(const char *dir, int *out_count)
{
    DIR *d = opendir(dir);
    if (!d) { *out_count = 0; return NULL; }

    int capacity = 64, count = 0;
    char **list = (char **)malloc(capacity * sizeof(char *));
    struct dirent *ent;

    while ((ent = readdir(d)) != NULL) {
        size_t len = strlen(ent->d_name);
        if (len > 4 && (strcasecmp(ent->d_name + len - 4, ".jpg") == 0 ||
                        strcasecmp(ent->d_name + len - 5, ".jpeg") == 0)) {
            if (count >= capacity) {
                capacity *= 2;
                list = (char **)realloc(list, capacity * sizeof(char *));
            }
            char path[512];
            snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name);
            list[count++] = strdup(path);
        }
    }
    closedir(d);

    /* Sort for reproducibility */
    for (int i = 0; i < count - 1; i++)
        for (int j = i + 1; j < count; j++)
            if (strcmp(list[i], list[j]) > 0) {
                char *tmp = list[i]; list[i] = list[j]; list[j] = tmp;
            }

    *out_count = count;
    return list;
}

static void fd_free_jpeg_list(char **list, int count)
{
    if (!list) return;
    for (int i = 0; i < count; i++) free(list[i]);
    free(list);
}

/* Simple MD5 using /usr/bin/md5sum (busybox). Returns hex string in out (33 bytes).
 * WARNING: uses popen/fork — only safe for small numbers of calls (e.g., 3 encoder models).
 * Do NOT use in loops over hundreds of files (causes OOM on RV1106). */
static int fd_md5_file(const char *path, char *out)
{
    char cmd[600];
    snprintf(cmd, sizeof(cmd), "md5sum '%s' 2>/dev/null", path);
    FILE *p = popen(cmd, "r");
    if (!p) return -1;
    if (fgets(out, 33, p) == NULL) { pclose(p); return -1; }
    out[32] = '\0';
    pclose(p);
    return 0;
}

/* In-process FNV-1a hash of raw data. Returns 16-char hex string in out (17 bytes min).
 * No forking — safe for thousands of calls on memory-constrained devices. */
static void fd_fnv1a_hash(const uint8_t *data, size_t len, char *out)
{
    uint64_t h = 0xcbf29ce484222325ULL;
    for (size_t i = 0; i < len; i++) {
        h ^= data[i];
        h *= 0x100000001b3ULL;
    }
    snprintf(out, 17, "%016llx", (unsigned long long)h);
}

/* Get total size of directory tree (bytes) */
static size_t fd_dir_size(const char *path)
{
    DIR *d = opendir(path);
    if (!d) return 0;
    size_t total = 0;
    struct dirent *ent;
    char child[512];
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
            continue;
        snprintf(child, sizeof(child), "%s/%s", path, ent->d_name);
        struct stat st;
        if (stat(child, &st) == 0) {
            if (S_ISDIR(st.st_mode))
                total += fd_dir_size(child);
            else
                total += st.st_size;
        }
    }
    closedir(d);
    return total;
}

/* ============================================================================
 * Prototype Management — Computation Core
 * ============================================================================ */

/* Model names for progress display */
static const char *g_proto_model_names[3] = {
    "Classification", "Spatial Fine", "Spatial Coarse"
};

/* RKNN model filenames relative to model set dir */
static const char *g_proto_model_files[3] = {
    "protonet/encoder.rknn",
    "protonet/spatial_encoder.rknn",
    "protonet/spatial_encoder_coarse.rknn"
};

/* Output prototype filenames */
static const char *g_proto_output_files[3] = {
    "prototypes.bin",
    "spatial_prototypes.bin",
    "spatial_prototypes_coarse.bin"
};

/* Destination filenames in model dir (when activating) */
static const char *g_proto_dest_files[3] = {
    "protonet/prototypes.bin",
    "protonet/spatial_prototypes.bin",
    "protonet/spatial_prototypes_coarse.bin"
};

static void fd_proto_set_state(fd_proto_compute_state_t state,
                                const char *error_msg)
{
    pthread_mutex_lock(&g_proto.mutex);
    g_proto.progress.state = state;
    if (error_msg)
        snprintf(g_proto.progress.error_msg, sizeof(g_proto.progress.error_msg),
                 "%s", error_msg);
    pthread_mutex_unlock(&g_proto.mutex);
}

/* Core prototype computation — runs in FD thread context.
 * Pauses normal FD inference during computation. */
static void fd_do_proto_computation(void)
{
    pthread_mutex_lock(&g_proto.mutex);
    fd_proto_compute_progress_t prog = g_proto.progress;
    pthread_mutex_unlock(&g_proto.mutex);

    fd_log("Proto compute: dataset=%s set=%s incremental=%d\n",
           prog.dataset_name, prog.set_name, prog.incremental);

    /* Validate dataset */
    char fail_dir[512], succ_dir[512];
    snprintf(fail_dir, sizeof(fail_dir), "%s/%s/failure", FD_DATASETS_DIR, prog.dataset_name);
    snprintf(succ_dir, sizeof(succ_dir), "%s/%s/success", FD_DATASETS_DIR, prog.dataset_name);

    int n_fail, n_succ;
    char **fail_files = fd_collect_jpegs(fail_dir, &n_fail);
    char **succ_files = fd_collect_jpegs(succ_dir, &n_succ);

    if (n_fail == 0 || n_succ == 0) {
        fd_err("Proto compute: empty dataset (fail=%d succ=%d)\n", n_fail, n_succ);
        fd_proto_set_state(PROTO_COMPUTE_ERROR, "Dataset has no images in failure/ or success/");
        fd_free_jpeg_list(fail_files, n_fail);
        fd_free_jpeg_list(succ_files, n_succ);
        return;
    }

    /* Total images across all 3 models */
    int total_images = (n_fail + n_succ) * 3;

    pthread_mutex_lock(&g_proto.mutex);
    g_proto.progress.state = PROTO_COMPUTE_RUNNING;
    g_proto.progress.total_images_all = total_images;
    g_proto.progress.total_images_processed = 0;
    pthread_mutex_unlock(&g_proto.mutex);

    /* Create output directory */
    char set_dir[512];
    snprintf(set_dir, sizeof(set_dir), "%s/%s", FD_PROTO_SETS_DIR, prog.set_name);
    fd_mkdir_p(set_dir);

    /* Allocate preprocessing buffer */
    uint8_t *preproc_buf = (uint8_t *)malloc(FD_MODEL_INPUT_BYTES);
    if (!preproc_buf) {
        fd_proto_set_state(PROTO_COMPUTE_ERROR, "malloc failed for preprocess buffer");
        fd_free_jpeg_list(fail_files, n_fail);
        fd_free_jpeg_list(succ_files, n_succ);
        return;
    }

    /* Allocate image hash arrays — computed during first model pass (mi==0)
     * to avoid forking 1000 md5sum processes which caused OOM on RV1106 */
    char (*fail_hashes)[17] = (char (*)[17])calloc(n_fail, 17);
    char (*succ_hashes)[17] = (char (*)[17])calloc(n_succ, 17);

    /* --- Incremental mode: load existing metadata for hash dedup + running mean --- */
    int old_n_fail = 0, old_n_succ = 0;
    int n_old_hashes = 0;
    char **old_hashes = NULL;       /* existing image hash strings */
    float *old_protos[3][2];        /* old prototype vectors per model per class */
    int old_emb_dims[3] = {0};
    memset(old_protos, 0, sizeof(old_protos));

    if (prog.incremental) {
        /* Load existing metadata.json for hash list + counts */
        char meta_path[512];
        snprintf(meta_path, sizeof(meta_path), "%s/%s/metadata.json",
                 FD_PROTO_SETS_DIR, prog.set_name);
        FILE *mf = fopen(meta_path, "r");
        if (!mf) {
            fd_err("Proto incremental: no existing metadata at %s, doing full compute\n", meta_path);
            prog.incremental = 0;
        } else {
            fseek(mf, 0, SEEK_END);
            long msz = ftell(mf);
            fseek(mf, 0, SEEK_SET);
            char *mbuf = (char *)malloc(msz + 1);
            if (mbuf) {
                fread(mbuf, 1, msz, mf);
                mbuf[msz] = '\0';
                fclose(mf);
                cJSON *mj = cJSON_Parse(mbuf);
                free(mbuf);
                if (mj) {
                    const cJSON *nf = cJSON_GetObjectItemCaseSensitive(mj, "n_failure");
                    const cJSON *ns = cJSON_GetObjectItemCaseSensitive(mj, "n_success");
                    if (nf) old_n_fail = nf->valueint;
                    if (ns) old_n_succ = ns->valueint;

                    /* Load existing hash array */
                    const cJSON *ih = cJSON_GetObjectItemCaseSensitive(mj, "image_hashes");
                    if (ih && cJSON_IsArray(ih)) {
                        n_old_hashes = cJSON_GetArraySize(ih);
                        old_hashes = (char **)calloc(n_old_hashes, sizeof(char *));
                        if (old_hashes) {
                            int idx = 0;
                            const cJSON *h;
                            cJSON_ArrayForEach(h, ih) {
                                if (cJSON_IsString(h) && idx < n_old_hashes)
                                    old_hashes[idx++] = strdup(h->valuestring);
                            }
                        }
                    }
                    cJSON_Delete(mj);
                    fd_log("Proto incremental: existing set has %d fail + %d succ, %d hashes\n",
                           old_n_fail, old_n_succ, n_old_hashes);
                } else {
                    fclose(mf);
                    prog.incremental = 0;
                }
            } else {
                fclose(mf);
                prog.incremental = 0;
            }
        }

        /* Load existing prototype .bin files for running mean merge */
        if (prog.incremental) {
            /* Classification: raw 2*emb float32 (no header) */
            char ppath[512];
            snprintf(ppath, sizeof(ppath), "%s/%s/%s",
                     FD_PROTO_SETS_DIR, prog.set_name, g_proto_output_files[0]);
            FILE *pf = fopen(ppath, "rb");
            if (pf) {
                old_emb_dims[0] = EMB_DIM;
                for (int c = 0; c < 2; c++) {
                    old_protos[0][c] = (float *)malloc(EMB_DIM * sizeof(float));
                    if (old_protos[0][c])
                        fread(old_protos[0][c], sizeof(float), EMB_DIM, pf);
                }
                fclose(pf);
            }
            /* Spatial: 4-uint32 header + 2*emb float32 */
            for (int si = 1; si <= 2; si++) {
                snprintf(ppath, sizeof(ppath), "%s/%s/%s",
                         FD_PROTO_SETS_DIR, prog.set_name, g_proto_output_files[si]);
                pf = fopen(ppath, "rb");
                if (pf) {
                    uint32_t hdr[4];
                    if (fread(hdr, sizeof(uint32_t), 4, pf) == 4) {
                        old_emb_dims[si] = (int)hdr[2];
                        for (int c = 0; c < 2; c++) {
                            old_protos[si][c] = (float *)malloc(hdr[2] * sizeof(float));
                            if (old_protos[si][c])
                                fread(old_protos[si][c], sizeof(float), hdr[2], pf);
                        }
                    }
                    fclose(pf);
                }
            }
        }
    }

    /* Build skip-set from old hashes for O(1) lookup (simple linear scan is fine
     * for <2000 hashes — avoids hash table complexity on embedded target) */

    /* Get current model set name for path resolution */
    pthread_mutex_lock(&g_fd.config_mutex);
    char set_name[FD_SET_NAME_LEN];
    strncpy(set_name, g_fd.config.model_set, sizeof(set_name) - 1);
    set_name[sizeof(set_name) - 1] = '\0';
    pthread_mutex_unlock(&g_fd.config_mutex);

    struct timeval tv_start;
    gettimeofday(&tv_start, NULL);

    char encoder_hashes[3][33];
    memset(encoder_hashes, 0, sizeof(encoder_hashes));
    int all_ok = 1;
    int final_new_counts[2] = {0, 0};  /* new images across all models (set by mi==0) */

    /* Process each of 3 models */
    for (int mi = 0; mi < 3 && all_ok && !g_proto.cancel; mi++) {
        char model_path[512];
        snprintf(model_path, sizeof(model_path), "%s/%s/%s",
                 g_fd.models_base_dir, set_name, g_proto_model_files[mi]);

        /* Check if model exists */
        if (access(model_path, R_OK) != 0) {
            fd_log("Proto compute: model %s not found, skipping\n", g_proto_model_names[mi]);
            /* Mark as skipped in metrics */
            pthread_mutex_lock(&g_proto.mutex);
            g_proto.progress.cos_sim[mi] = 0;
            g_proto.progress.margin[mi] = 0;
            g_proto.progress.total_images_processed += (n_fail + n_succ);
            pthread_mutex_unlock(&g_proto.mutex);
            continue;
        }

        /* Compute MD5 of encoder model */
        fd_md5_file(model_path, encoder_hashes[mi]);

        /* Load RKNN model */
        fd_rknn_model_t model;
        int ret = fd_model_init_retry(&model, model_path);
        if (ret != 0) {
            fd_err("Proto compute: failed to load model %s\n", model_path);
            fd_proto_set_state(PROTO_COMPUTE_ERROR, "Failed to load RKNN model");
            all_ok = 0;
            break;
        }

        /* Determine output dimensions */
        rknn_tensor_attr *oattr = &model.output_attrs[0];
        int out_h = 1, out_w = 1, out_c = oattr->n_elems;

        /* For spatial models, query NHWC dimensions from shape */
        if (mi > 0 && oattr->n_elems > 1024) {
            /* NHWC: dims[0]=N, dims[1]=H, dims[2]=W, dims[3]=C */
            if (oattr->n_dims == 4) {
                out_h = oattr->dims[1];
                out_w = oattr->dims[2];
                out_c = oattr->dims[3];
            } else {
                /* Fallback: try to infer from known architectures */
                if (oattr->n_elems == 50176) { out_h = 14; out_w = 28; out_c = 128; }
                else if (oattr->n_elems == 25088) { out_h = 7; out_w = 14; out_c = 256; }
                else { out_h = 1; out_w = 1; out_c = oattr->n_elems; }
            }
        }
        /* Classification model: 1x1x1024 */
        if (mi == 0) { out_h = 1; out_w = 1; out_c = EMB_DIM; }

        int emb_dim = out_c;  /* After GAP, embedding is C-dimensional */
        int is_spatial = (out_h > 1 || out_w > 1);

        fd_log("Proto compute: model[%d]=%s output=%dx%dx%d emb=%d spatial=%d\n",
               mi, g_proto_model_names[mi], out_h, out_w, out_c, emb_dim, is_spatial);

        /* Allocate output buffer for dequantized model output */
        float *out_buf = (float *)malloc(oattr->n_elems * sizeof(float));
        if (!out_buf) {
            fd_model_release(&model);
            fd_proto_set_state(PROTO_COMPUTE_ERROR, "malloc failed for output buffer");
            all_ok = 0;
            break;
        }

        /* Accumulate embeddings for each class (only NEW images in incremental mode) */
        float *proto_accum[2];
        proto_accum[0] = (float *)calloc(emb_dim, sizeof(float));
        proto_accum[1] = (float *)calloc(emb_dim, sizeof(float));
        if (!proto_accum[0] || !proto_accum[1]) {
            free(out_buf);
            free(proto_accum[0]); free(proto_accum[1]);
            fd_model_release(&model);
            fd_proto_set_state(PROTO_COMPUTE_ERROR, "malloc failed for accumulators");
            all_ok = 0;
            break;
        }
        int new_counts[2] = {0, 0};  /* count of NEW images processed per class */

        /* Process both classes */
        char **class_files[2] = { fail_files, succ_files };
        int class_counts[2] = { n_fail, n_succ };

        for (int ci = 0; ci < 2 && !g_proto.cancel; ci++) {
            pthread_mutex_lock(&g_proto.mutex);
            g_proto.progress.current_model = mi;
            g_proto.progress.model_name = g_proto_model_names[mi];
            g_proto.progress.current_class = ci;
            g_proto.progress.images_processed = 0;
            g_proto.progress.images_total = class_counts[ci];
            pthread_mutex_unlock(&g_proto.mutex);

            for (int fi = 0; fi < class_counts[ci] && !g_proto.cancel; fi++) {
                /* Load and preprocess image */
                FILE *f = fopen(class_files[ci][fi], "rb");
                if (!f) continue;
                fseek(f, 0, SEEK_END);
                long fsize = ftell(f);
                fseek(f, 0, SEEK_SET);
                if (fsize <= 0 || fsize > 2 * 1024 * 1024) { fclose(f); continue; }

                uint8_t *jpeg_data = (uint8_t *)malloc(fsize);
                if (!jpeg_data) { fclose(f); continue; }
                fread(jpeg_data, 1, fsize, f);
                fclose(f);

                /* Hash image data during first model pass (no fork/popen) */
                if (mi == 0) {
                    char (*hashes)[17] = ci == 0 ? fail_hashes : succ_hashes;
                    if (hashes)
                        fd_fnv1a_hash(jpeg_data, fsize, hashes[fi]);
                }

                /* Incremental: skip images whose hash is in the existing set */
                if (prog.incremental && old_hashes && n_old_hashes > 0) {
                    char (*hashes)[17] = ci == 0 ? fail_hashes : succ_hashes;
                    if (hashes && hashes[fi][0]) {
                        int found = 0;
                        for (int hi = 0; hi < n_old_hashes && !found; hi++) {
                            if (old_hashes[hi] && strcmp(hashes[fi], old_hashes[hi]) == 0)
                                found = 1;
                        }
                        if (found) {
                            free(jpeg_data);
                            pthread_mutex_lock(&g_proto.mutex);
                            g_proto.progress.images_processed = fi + 1;
                            g_proto.progress.total_images_processed++;
                            pthread_mutex_unlock(&g_proto.mutex);
                            continue;  /* skip — already in prototype */
                        }
                    }
                }

                fd_image_t img = {0};
                if (fd_decode_jpeg(jpeg_data, fsize, &img) != 0) {
                    free(jpeg_data);
                    continue;
                }
                free(jpeg_data);

                fd_resize_crop(img.data, img.width, img.height, preproc_buf);
                free(img.data);

                /* Run inference */
                ret = fd_model_run(&model, preproc_buf, FD_MODEL_INPUT_BYTES);
                if (ret != 0) continue;

                /* Get output */
                if (is_spatial) {
                    fd_model_get_output_nhwc(&model, 0, out_buf, out_h, out_w, out_c);
                    /* Apply GAP: average over H*W spatial positions */
                    for (int c = 0; c < emb_dim; c++) {
                        float sum = 0;
                        for (int hw = 0; hw < out_h * out_w; hw++)
                            sum += out_buf[hw * out_c + c];
                        proto_accum[ci][c] += sum / (float)(out_h * out_w);
                    }
                } else {
                    fd_model_get_output(&model, 0, out_buf, emb_dim);
                    for (int c = 0; c < emb_dim; c++)
                        proto_accum[ci][c] += out_buf[c];
                }
                new_counts[ci]++;

                /* Update progress */
                pthread_mutex_lock(&g_proto.mutex);
                g_proto.progress.images_processed = fi + 1;
                g_proto.progress.total_images_processed++;

                /* Update ETA */
                struct timeval tv_now;
                gettimeofday(&tv_now, NULL);
                int elapsed = (int)(tv_now.tv_sec - tv_start.tv_sec);
                g_proto.progress.elapsed_s = elapsed;
                if (g_proto.progress.total_images_processed > 0) {
                    float rate = (float)g_proto.progress.total_images_processed / (float)(elapsed > 0 ? elapsed : 1);
                    g_proto.progress.estimated_total_s = (int)(total_images / rate);
                }
                pthread_mutex_unlock(&g_proto.mutex);

                /* Yield to let kernel reclaim pages after each image cycle */
                usleep(1000);
            }
        }

        if (g_proto.cancel) {
            free(out_buf);
            free(proto_accum[0]); free(proto_accum[1]);
            fd_model_release(&model);
            break;
        }

        /* Average the accumulators, with running mean merge for incremental mode.
         * Incremental: proto = (old_proto * n_old + sum_new) / (n_old + n_new) */
        int old_counts[2] = { old_n_fail, old_n_succ };
        for (int ci = 0; ci < 2; ci++) {
            int n_new = new_counts[ci];
            if (prog.incremental && old_protos[mi][ci] && old_emb_dims[mi] == emb_dim
                && old_counts[ci] > 0) {
                /* Running mean: merge old prototype with new sum */
                int n_total = old_counts[ci] + n_new;
                if (n_total > 0) {
                    for (int c = 0; c < emb_dim; c++) {
                        float old_contrib = old_protos[mi][ci][c] * (float)old_counts[ci];
                        proto_accum[ci][c] = (old_contrib + proto_accum[ci][c]) / (float)n_total;
                    }
                }
                fd_log("Proto incremental: %s class %d: %d old + %d new = %d total\n",
                       g_proto_model_names[mi], ci, old_counts[ci], n_new, n_total);
            } else {
                /* Full mode: simple average */
                if (n_new > 0)
                    for (int c = 0; c < emb_dim; c++)
                        proto_accum[ci][c] /= (float)n_new;
            }
        }

        /* Compute cosine similarity */
        float dot = 0, norm0 = 0, norm1 = 0;
        for (int c = 0; c < emb_dim; c++) {
            dot += proto_accum[0][c] * proto_accum[1][c];
            norm0 += proto_accum[0][c] * proto_accum[0][c];
            norm1 += proto_accum[1][c] * proto_accum[1][c];
        }
        norm0 = sqrtf(norm0);
        norm1 = sqrtf(norm1);
        float cos_sim = (norm0 > 1e-12f && norm1 > 1e-12f) ?
                         dot / (norm0 * norm1) : 0.0f;
        float margin = 1.0f - cos_sim;

        pthread_mutex_lock(&g_proto.mutex);
        g_proto.progress.cos_sim[mi] = cos_sim;
        g_proto.progress.margin[mi] = margin;
        pthread_mutex_unlock(&g_proto.mutex);

        fd_log("Proto compute: %s cos_sim=%.3f margin=%.3f (new: %d fail + %d succ)\n",
               g_proto_model_names[mi], cos_sim, margin, new_counts[0], new_counts[1]);

        /* Save new image counts from first model pass for metadata */
        if (mi == 0) {
            final_new_counts[0] = new_counts[0];
            final_new_counts[1] = new_counts[1];
        }

        /* Save prototype binary */
        fd_proto_set_state(PROTO_COMPUTE_SAVING, NULL);

        char proto_path[512];
        snprintf(proto_path, sizeof(proto_path), "%s/%s",
                 set_dir, g_proto_output_files[mi]);

        FILE *pf = fopen(proto_path, "wb");
        if (pf) {
            if (is_spatial) {
                /* Spatial format: 4-uint32 header [h,w,emb_dim,n_classes] + data */
                uint32_t header[4] = { (uint32_t)out_h, (uint32_t)out_w,
                                       (uint32_t)emb_dim, 2 };
                fwrite(header, sizeof(uint32_t), 4, pf);
            }
            /* Write fail then success prototype */
            fwrite(proto_accum[0], sizeof(float), emb_dim, pf);
            fwrite(proto_accum[1], sizeof(float), emb_dim, pf);
            fclose(pf);
            fd_log("Proto compute: saved %s (%d floats/class)\n",
                   proto_path, emb_dim);
        } else {
            fd_err("Proto compute: cannot write %s\n", proto_path);
        }

        free(out_buf);
        free(proto_accum[0]);
        free(proto_accum[1]);
        fd_model_release(&model);

        /* Pause between models to let CMA/system reclaim memory.
         * Each model load/release cycles CMA; give time for cleanup. */
        usleep(500000);
    }

    free(preproc_buf);

    if (g_proto.cancel) {
        fd_proto_set_state(PROTO_COMPUTE_CANCELLED, "Cancelled by user");
        goto cleanup;
    }

    if (!all_ok) {
        goto cleanup_no_meta;
    }

    /* Skip past cleanup labels */
    goto write_meta;

cleanup:
cleanup_no_meta:
    fd_free_jpeg_list(fail_files, n_fail);
    fd_free_jpeg_list(succ_files, n_succ);
    free(fail_hashes); free(succ_hashes);
    for (int i = 0; i < n_old_hashes; i++) free(old_hashes[i]);
    free(old_hashes);
    for (int mi2 = 0; mi2 < 3; mi2++)
        for (int ci2 = 0; ci2 < 2; ci2++) free(old_protos[mi2][ci2]);
    return;

write_meta:
    ;

    /* Write metadata.json — use merged counts for incremental */
    int meta_n_fail = prog.incremental ? (old_n_fail + final_new_counts[0]) : n_fail;
    int meta_n_succ = prog.incremental ? (old_n_succ + final_new_counts[1]) : n_succ;

    cJSON *meta = cJSON_CreateObject();
    cJSON_AddStringToObject(meta, "name", prog.set_name);
    cJSON_AddStringToObject(meta, "source_dataset", prog.dataset_name);
    cJSON_AddNumberToObject(meta, "n_failure", meta_n_fail);
    cJSON_AddNumberToObject(meta, "n_success", meta_n_succ);

    /* Timestamp */
    time_t now = time(NULL);
    char ts[32];
    struct tm *tm_info = gmtime(&now);
    strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%SZ", tm_info);
    cJSON_AddStringToObject(meta, "created", ts);

    /* Encoder hashes */
    cJSON *hashes = cJSON_CreateObject();
    cJSON_AddStringToObject(hashes, "classification", encoder_hashes[0]);
    cJSON_AddStringToObject(hashes, "spatial_fine", encoder_hashes[1]);
    cJSON_AddStringToObject(hashes, "spatial_coarse", encoder_hashes[2]);
    cJSON_AddItemToObject(meta, "encoder_hashes", hashes);

    /* Metrics */
    cJSON *metrics = cJSON_CreateObject();
    for (int mi = 0; mi < 3; mi++) {
        cJSON *m = cJSON_CreateObject();
        pthread_mutex_lock(&g_proto.mutex);
        cJSON_AddNumberToObject(m, "cos_sim", g_proto.progress.cos_sim[mi]);
        cJSON_AddNumberToObject(m, "margin", g_proto.progress.margin[mi]);
        pthread_mutex_unlock(&g_proto.mutex);
        cJSON_AddItemToObject(metrics, g_proto_model_names[mi], m);
    }
    cJSON_AddItemToObject(meta, "metrics", metrics);

    /* Image hashes (for dedup in incremental mode).
     * Merge old hashes + new hashes for incremental. */
    cJSON *img_hashes = cJSON_CreateArray();
    /* First: carry over existing hashes from previous set */
    if (prog.incremental && old_hashes) {
        for (int i = 0; i < n_old_hashes; i++) {
            if (old_hashes[i])
                cJSON_AddItemToArray(img_hashes, cJSON_CreateString(old_hashes[i]));
        }
    }
    /* Then: add new image hashes (skip duplicates already in old set) */
    for (int ci = 0; ci < 2; ci++) {
        char (*hashes)[17] = ci == 0 ? fail_hashes : succ_hashes;
        int count = ci == 0 ? n_fail : n_succ;
        if (hashes) {
            for (int i = 0; i < count; i++) {
                if (!hashes[i][0]) continue;
                /* Skip if already in old hashes */
                int dup = 0;
                if (prog.incremental && old_hashes) {
                    for (int hi = 0; hi < n_old_hashes && !dup; hi++) {
                        if (old_hashes[hi] && strcmp(hashes[i], old_hashes[hi]) == 0)
                            dup = 1;
                    }
                }
                if (!dup)
                    cJSON_AddItemToArray(img_hashes, cJSON_CreateString(hashes[i]));
            }
        }
    }
    cJSON_AddItemToObject(meta, "image_hashes", img_hashes);

    char meta_path[512];
    snprintf(meta_path, sizeof(meta_path), "%s/metadata.json", set_dir);
    char *json_str = cJSON_Print(meta);
    if (json_str) {
        FILE *mf = fopen(meta_path, "w");
        if (mf) {
            fputs(json_str, mf);
            fclose(mf);
        }
        free(json_str);
    }
    cJSON_Delete(meta);

    fd_free_jpeg_list(fail_files, n_fail);
    fd_free_jpeg_list(succ_files, n_succ);
    free(fail_hashes);
    free(succ_hashes);
    /* Free incremental state */
    for (int i = 0; i < n_old_hashes; i++) free(old_hashes[i]);
    free(old_hashes);
    for (int mi = 0; mi < 3; mi++)
        for (int ci = 0; ci < 2; ci++) free(old_protos[mi][ci]);

    struct timeval tv_end;
    gettimeofday(&tv_end, NULL);
    int total_elapsed = (int)(tv_end.tv_sec - tv_start.tv_sec);

    pthread_mutex_lock(&g_proto.mutex);
    g_proto.progress.state = PROTO_COMPUTE_DONE;
    g_proto.progress.elapsed_s = total_elapsed;
    pthread_mutex_unlock(&g_proto.mutex);

    fd_log("Proto compute: DONE in %ds. Classification margin=%.3f\n",
           total_elapsed, g_proto.progress.margin[0]);
}

/* ============================================================================
 * Prototype Management — Public API
 * ============================================================================ */

int fault_detect_compute_prototypes(const char *dataset_name, const char *set_name)
{
    if (!dataset_name || !set_name || !dataset_name[0] || !set_name[0])
        return -1;
    /* Reject path traversal */
    if (strstr(dataset_name, "..") || strstr(set_name, ".."))
        return -1;

    pthread_mutex_lock(&g_proto.mutex);
    if (g_proto.progress.state == PROTO_COMPUTE_RUNNING ||
        g_proto.progress.state == PROTO_COMPUTE_PENDING) {
        pthread_mutex_unlock(&g_proto.mutex);
        return -1;  /* Already running */
    }
    memset(&g_proto.progress, 0, sizeof(g_proto.progress));
    g_proto.progress.state = PROTO_COMPUTE_PENDING;
    g_proto.progress.incremental = 0;
    g_proto.cancel = 0;
    snprintf(g_proto.progress.dataset_name, sizeof(g_proto.progress.dataset_name),
             "%s", dataset_name);
    snprintf(g_proto.progress.set_name, sizeof(g_proto.progress.set_name),
             "%s", set_name);
    pthread_mutex_unlock(&g_proto.mutex);

    fd_log("Proto compute: queued dataset=%s set=%s\n", dataset_name, set_name);
    return 0;
}

int fault_detect_compute_prototypes_incremental(const char *dataset_name,
                                                  const char *set_name)
{
    if (!dataset_name || !set_name || !dataset_name[0] || !set_name[0])
        return -1;
    if (strstr(dataset_name, "..") || strstr(set_name, ".."))
        return -1;

    pthread_mutex_lock(&g_proto.mutex);
    if (g_proto.progress.state == PROTO_COMPUTE_RUNNING ||
        g_proto.progress.state == PROTO_COMPUTE_PENDING) {
        pthread_mutex_unlock(&g_proto.mutex);
        return -1;
    }
    memset(&g_proto.progress, 0, sizeof(g_proto.progress));
    g_proto.progress.state = PROTO_COMPUTE_PENDING;
    g_proto.progress.incremental = 1;
    g_proto.cancel = 0;
    snprintf(g_proto.progress.dataset_name, sizeof(g_proto.progress.dataset_name),
             "%s", dataset_name);
    snprintf(g_proto.progress.set_name, sizeof(g_proto.progress.set_name),
             "%s", set_name);
    pthread_mutex_unlock(&g_proto.mutex);
    return 0;
}

void fault_detect_cancel_proto_compute(void)
{
    g_proto.cancel = 1;
}

fd_proto_compute_progress_t fault_detect_get_proto_progress(void)
{
    fd_proto_compute_progress_t p;
    pthread_mutex_lock(&g_proto.mutex);
    p = g_proto.progress;
    pthread_mutex_unlock(&g_proto.mutex);
    return p;
}

int fault_detect_list_datasets(fd_dataset_info_t *out, int max)
{
    DIR *d = opendir(FD_DATASETS_DIR);
    if (!d) return 0;

    int count = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL && count < max) {
        if (ent->d_name[0] == '.') continue;
        char path[512];
        snprintf(path, sizeof(path), "%s/%s", FD_DATASETS_DIR, ent->d_name);
        struct stat st;
        if (stat(path, &st) != 0 || !S_ISDIR(st.st_mode)) continue;

        fd_dataset_info_t *info = &out[count];
        memset(info, 0, sizeof(*info));
        snprintf(info->name, sizeof(info->name), "%s", ent->d_name);

        char sub[512];
        snprintf(sub, sizeof(sub), "%s/failure", path);
        info->n_failure = fd_count_jpegs(sub);
        snprintf(sub, sizeof(sub), "%s/success", path);
        info->n_success = fd_count_jpegs(sub);

        info->created = st.st_mtime;
        info->size_bytes = fd_dir_size(path);

        /* Read source from metadata.json if exists */
        snprintf(sub, sizeof(sub), "%s/metadata.json", path);
        FILE *mf = fopen(sub, "r");
        if (mf) {
            char buf[1024];
            size_t n = fread(buf, 1, sizeof(buf) - 1, mf);
            buf[n] = '\0';
            fclose(mf);
            cJSON *mj = cJSON_Parse(buf);
            if (mj) {
                const cJSON *src = cJSON_GetObjectItemCaseSensitive(mj, "source");
                if (src && cJSON_IsString(src))
                    snprintf(info->source, sizeof(info->source), "%s", src->valuestring);
                cJSON_Delete(mj);
            }
        }
        count++;
    }
    closedir(d);
    return count;
}

int fault_detect_create_dataset(const char *name)
{
    if (!name || !name[0] || strstr(name, "..") || strchr(name, '/'))
        return -1;

    char path[512];
    snprintf(path, sizeof(path), "%s/%s", FD_DATASETS_DIR, name);
    if (fd_mkdir_p(path) != 0) return -1;

    char sub[512];
    snprintf(sub, sizeof(sub), "%s/failure", path);
    mkdir(sub, 0755);
    snprintf(sub, sizeof(sub), "%s/success", path);
    mkdir(sub, 0755);

    return 0;
}

int fault_detect_delete_dataset(const char *name)
{
    if (!name || !name[0] || strstr(name, "..") || strchr(name, '/'))
        return -1;
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", FD_DATASETS_DIR, name);
    return fd_rmdir_recursive(path);
}

int fault_detect_save_frame_to_dataset(const char *dataset_name, int class_idx,
                                        const uint8_t *jpeg_buf, size_t jpeg_len)
{
    if (!dataset_name || !dataset_name[0] || strstr(dataset_name, ".."))
        return -1;
    if (class_idx < 0 || class_idx > 1 || !jpeg_buf || jpeg_len == 0)
        return -1;

    const char *class_dir = class_idx == 0 ? "failure" : "success";
    char dir[512];
    snprintf(dir, sizeof(dir), "%s/%s/%s", FD_DATASETS_DIR, dataset_name, class_dir);
    fd_mkdir_p(dir);

    /* Generate filename from timestamp */
    struct timeval tv;
    gettimeofday(&tv, NULL);
    char fname[512];
    snprintf(fname, sizeof(fname), "%s/%ld_%06ld.jpg",
             dir, (long)tv.tv_sec, (long)tv.tv_usec);

    FILE *f = fopen(fname, "wb");
    if (!f) return -1;
    fwrite(jpeg_buf, 1, jpeg_len, f);
    fclose(f);
    return 0;
}

int fault_detect_list_proto_sets(fd_proto_set_info_t *out, int max,
                                  const char *active_set)
{
    DIR *d = opendir(FD_PROTO_SETS_DIR);
    if (!d) return 0;

    int count = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL && count < max) {
        if (ent->d_name[0] == '.') continue;
        char path[512];
        snprintf(path, sizeof(path), "%s/%s", FD_PROTO_SETS_DIR, ent->d_name);
        struct stat st;
        if (stat(path, &st) != 0 || !S_ISDIR(st.st_mode)) continue;

        fd_proto_set_info_t *info = &out[count];
        memset(info, 0, sizeof(*info));
        snprintf(info->name, sizeof(info->name), "%s", ent->d_name);
        info->created = st.st_mtime;
        info->is_active = (active_set && strcmp(active_set, ent->d_name) == 0);

        /* Read metadata.json (can be >20KB with image hashes) */
        char meta_path[512];
        snprintf(meta_path, sizeof(meta_path), "%s/metadata.json", path);
        FILE *mf = fopen(meta_path, "r");
        if (mf) {
            fseek(mf, 0, SEEK_END);
            long fsz = ftell(mf);
            fseek(mf, 0, SEEK_SET);
            if (fsz <= 0 || fsz > 256 * 1024) { fclose(mf); count++; continue; }
            char *buf = (char *)malloc(fsz + 1);
            if (!buf) { fclose(mf); count++; continue; }
            size_t n = fread(buf, 1, fsz, mf);
            buf[n] = '\0';
            fclose(mf);
            cJSON *mj = cJSON_Parse(buf);
            if (mj) {
                const cJSON *ds = cJSON_GetObjectItemCaseSensitive(mj, "source_dataset");
                if (ds && cJSON_IsString(ds))
                    snprintf(info->source_dataset, sizeof(info->source_dataset),
                             "%s", ds->valuestring);
                const cJSON *nf = cJSON_GetObjectItemCaseSensitive(mj, "n_failure");
                if (nf && cJSON_IsNumber(nf)) info->n_failure = nf->valueint;
                const cJSON *ns = cJSON_GetObjectItemCaseSensitive(mj, "n_success");
                if (ns && cJSON_IsNumber(ns)) info->n_success = ns->valueint;

                /* Metrics */
                const cJSON *met = cJSON_GetObjectItemCaseSensitive(mj, "metrics");
                if (met) {
                    const char *keys[3] = {"Classification", "Spatial Fine", "Spatial Coarse"};
                    for (int i = 0; i < 3; i++) {
                        const cJSON *m = cJSON_GetObjectItemCaseSensitive(met, keys[i]);
                        if (m) {
                            const cJSON *mg = cJSON_GetObjectItemCaseSensitive(m, "margin");
                            if (mg && cJSON_IsNumber(mg)) info->margin[i] = (float)mg->valuedouble;
                        }
                    }
                }

                /* Encoder hashes */
                const cJSON *eh = cJSON_GetObjectItemCaseSensitive(mj, "encoder_hashes");
                if (eh) {
                    const char *hkeys[3] = {"classification", "spatial_fine", "spatial_coarse"};
                    for (int i = 0; i < 3; i++) {
                        const cJSON *h = cJSON_GetObjectItemCaseSensitive(eh, hkeys[i]);
                        if (h && cJSON_IsString(h))
                            snprintf(info->encoder_hashes[i], 33, "%s", h->valuestring);
                    }
                }

                cJSON_Delete(mj);
            }
            free(buf);
        }
        count++;
    }
    closedir(d);
    return count;
}

int fault_detect_activate_proto_set(const char *set_name)
{
    if (!set_name || !set_name[0] || strstr(set_name, ".."))
        return -1;

    /* Get current model set for destination path */
    pthread_mutex_lock(&g_fd.config_mutex);
    char model_set[FD_SET_NAME_LEN];
    strncpy(model_set, g_fd.config.model_set, sizeof(model_set) - 1);
    model_set[sizeof(model_set) - 1] = '\0';
    pthread_mutex_unlock(&g_fd.config_mutex);

    if (!model_set[0]) {
        fd_err("Proto activate: no model set configured\n");
        return -1;
    }

    /* Copy each prototype file from set dir to model dir */
    int copied = 0;
    for (int i = 0; i < 3; i++) {
        char src[512], dst[512];
        snprintf(src, sizeof(src), "%s/%s/%s",
                 FD_PROTO_SETS_DIR, set_name, g_proto_output_files[i]);
        snprintf(dst, sizeof(dst), "%s/%s/%s",
                 g_fd.models_base_dir, model_set, g_proto_dest_files[i]);

        if (access(src, R_OK) == 0) {
            if (fd_copy_file(src, dst) == 0) {
                fd_log("Proto activate: copied %s -> %s\n", src, dst);
                copied++;
            } else {
                fd_err("Proto activate: failed to copy %s -> %s\n", src, dst);
            }
        }
    }

    if (copied == 0) {
        fd_err("Proto activate: no prototype files found in %s\n", set_name);
        return -1;
    }

    /* Reload prototypes from the newly copied files */
    char proto_path[512];
    snprintf(proto_path, sizeof(proto_path), "%s/%s/protonet/prototypes.bin",
             g_fd.models_base_dir, model_set);
    if (access(proto_path, R_OK) == 0)
        fd_load_prototypes(proto_path);

    char sp_path[512];
    snprintf(sp_path, sizeof(sp_path), "%s/%s/protonet/spatial_prototypes.bin",
             g_fd.models_base_dir, model_set);
    if (access(sp_path, R_OK) == 0)
        fd_load_spatial_prototypes(sp_path);

    char spc_path[512];
    snprintf(spc_path, sizeof(spc_path), "%s/%s/protonet/spatial_prototypes_coarse.bin",
             g_fd.models_base_dir, model_set);
    if (access(spc_path, R_OK) == 0)
        fd_load_spatial_prototypes_coarse(spc_path);

    fd_log("Proto activate: set '%s' activated (%d files copied)\n", set_name, copied);
    return 0;
}

int fault_detect_delete_proto_set(const char *set_name)
{
    if (!set_name || !set_name[0] || strstr(set_name, "..") || strchr(set_name, '/'))
        return -1;
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", FD_PROTO_SETS_DIR, set_name);
    return fd_rmdir_recursive(path);
}

/* ============================================================================
 * Prototype Management — Dataset Download
 * ============================================================================ */

/* Resolve dataset URL: if URL points to a .json metadata file, fetch it and
 * extract the actual download URL and dataset name from it.
 * Returns 0 on success (url/name updated), -1 on error, 1 if not a metadata URL. */
static int fd_resolve_dataset_metadata(char *url, size_t url_sz, char *name, size_t name_sz)
{
    /* Only resolve if URL ends with .json */
    size_t len = strlen(url);
    if (len < 6 || strcmp(url + len - 5, ".json") != 0)
        return 1;  /* not metadata, use URL directly */

    fd_log("Download: fetching metadata from %s\n", url);

    char tmp_meta[] = "/tmp/fd_dataset_meta.json";
    char cmd[600];
    snprintf(cmd, sizeof(cmd), "wget -q -O '%s' '%s' 2>&1", tmp_meta, url);
    int ret = system(cmd);
    if (ret != 0) {
        fd_log("Download: metadata fetch failed (exit %d)\n", WEXITSTATUS(ret));
        unlink(tmp_meta);
        return -1;
    }

    /* Read and parse JSON */
    FILE *f = fopen(tmp_meta, "r");
    if (!f) { unlink(tmp_meta); return -1; }

    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (fsize <= 0 || fsize > 8192) { fclose(f); unlink(tmp_meta); return -1; }

    char *buf = malloc(fsize + 1);
    if (!buf) { fclose(f); unlink(tmp_meta); return -1; }
    fread(buf, 1, fsize, f);
    buf[fsize] = '\0';
    fclose(f);
    unlink(tmp_meta);

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) {
        fd_log("Download: metadata JSON parse failed\n");
        return -1;
    }

    cJSON *ds = cJSON_GetObjectItemCaseSensitive(root, "prototype_dataset");
    if (!ds) { cJSON_Delete(root); return -1; }

    cJSON *ds_url = cJSON_GetObjectItemCaseSensitive(ds, "url");
    cJSON *ds_name = cJSON_GetObjectItemCaseSensitive(ds, "name");
    if (!ds_url || !cJSON_IsString(ds_url) || !ds_url->valuestring[0]) {
        cJSON_Delete(root);
        return -1;
    }

    snprintf(url, url_sz, "%s", ds_url->valuestring);
    if (ds_name && cJSON_IsString(ds_name) && ds_name->valuestring[0])
        snprintf(name, name_sz, "%s", ds_name->valuestring);

    fd_log("Download: resolved to url=%s name=%s\n", url, name);
    cJSON_Delete(root);
    return 0;
}

static void *fd_download_thread_func(void *arg)
{
    (void)arg;

    pthread_mutex_lock(&g_proto.dl_mutex);
    g_proto.dl_progress.state = FD_DOWNLOAD_RUNNING;
    g_proto.dl_progress.downloaded_bytes = 0;
    g_proto.dl_progress.progress_pct = 0;
    g_proto.dl_progress.error_msg[0] = '\0';
    pthread_mutex_unlock(&g_proto.dl_mutex);

    /* Resolve metadata URL if needed */
    char resolved_url[512];
    char resolved_name[64];
    snprintf(resolved_url, sizeof(resolved_url), "%s", g_proto.dl_url);
    snprintf(resolved_name, sizeof(resolved_name), "%s", g_proto.dl_name);

    int meta_ret = fd_resolve_dataset_metadata(resolved_url, sizeof(resolved_url),
                                                resolved_name, sizeof(resolved_name));
    if (meta_ret < 0) {
        pthread_mutex_lock(&g_proto.dl_mutex);
        g_proto.dl_progress.state = FD_DOWNLOAD_ERROR;
        snprintf(g_proto.dl_progress.error_msg, sizeof(g_proto.dl_progress.error_msg),
                 "failed to fetch dataset metadata");
        pthread_mutex_unlock(&g_proto.dl_mutex);
        g_proto.dl_thread_running = 0;
        return NULL;
    }

    /* Update name if metadata resolved it */
    snprintf(g_proto.dl_name, sizeof(g_proto.dl_name), "%s", resolved_name);

    char tmp_path[] = "/tmp/fd_dataset.tar.gz";

    /* Download with wget (busybox) */
    char cmd[600];
    snprintf(cmd, sizeof(cmd), "wget -q -O '%s' '%s' 2>&1", tmp_path, resolved_url);

    fd_log("Download: starting %s\n", resolved_url);
    int ret = system(cmd);

    if (g_proto.dl_cancel) {
        unlink(tmp_path);
        pthread_mutex_lock(&g_proto.dl_mutex);
        g_proto.dl_progress.state = FD_DOWNLOAD_IDLE;
        pthread_mutex_unlock(&g_proto.dl_mutex);
        g_proto.dl_thread_running = 0;
        return NULL;
    }

    if (ret != 0) {
        unlink(tmp_path);
        pthread_mutex_lock(&g_proto.dl_mutex);
        g_proto.dl_progress.state = FD_DOWNLOAD_ERROR;
        snprintf(g_proto.dl_progress.error_msg, sizeof(g_proto.dl_progress.error_msg),
                 "wget failed (exit code %d)", WEXITSTATUS(ret));
        pthread_mutex_unlock(&g_proto.dl_mutex);
        g_proto.dl_thread_running = 0;
        return NULL;
    }

    /* Check downloaded file size */
    struct stat st;
    if (stat(tmp_path, &st) == 0) {
        pthread_mutex_lock(&g_proto.dl_mutex);
        g_proto.dl_progress.downloaded_bytes = st.st_size;
        g_proto.dl_progress.progress_pct = 100;
        pthread_mutex_unlock(&g_proto.dl_mutex);
    }

    /* Extract */
    pthread_mutex_lock(&g_proto.dl_mutex);
    g_proto.dl_progress.state = FD_DOWNLOAD_EXTRACTING;
    pthread_mutex_unlock(&g_proto.dl_mutex);

    fd_mkdir_p(FD_DATASETS_DIR);

    char dest_dir[512];
    snprintf(dest_dir, sizeof(dest_dir), "%s/%s", FD_DATASETS_DIR, g_proto.dl_name);
    fd_mkdir_p(dest_dir);

    snprintf(cmd, sizeof(cmd), "tar xzf '%s' -C '%s' --strip-components=0 2>&1",
             tmp_path, dest_dir);
    ret = system(cmd);
    unlink(tmp_path);

    if (ret != 0) {
        pthread_mutex_lock(&g_proto.dl_mutex);
        g_proto.dl_progress.state = FD_DOWNLOAD_ERROR;
        snprintf(g_proto.dl_progress.error_msg, sizeof(g_proto.dl_progress.error_msg),
                 "tar extract failed (exit code %d)", WEXITSTATUS(ret));
        pthread_mutex_unlock(&g_proto.dl_mutex);
        g_proto.dl_thread_running = 0;
        return NULL;
    }

    /* Check if tar extracted into a subdirectory — if so, move contents up */
    DIR *d = opendir(dest_dir);
    if (d) {
        struct dirent *ent;
        char only_subdir[512] = "";
        int dir_count = 0, file_count = 0;
        while ((ent = readdir(d)) != NULL) {
            if (ent->d_name[0] == '.') continue;
            char child[512];
            snprintf(child, sizeof(child), "%s/%s", dest_dir, ent->d_name);
            struct stat cst;
            if (stat(child, &cst) == 0) {
                if (S_ISDIR(cst.st_mode)) {
                    snprintf(only_subdir, sizeof(only_subdir), "%s", child);
                    dir_count++;
                } else {
                    file_count++;
                }
            }
        }
        closedir(d);

        /* If exactly 1 subdir and no files, move contents up (strip-components) */
        if (dir_count == 1 && file_count == 0 && only_subdir[0]) {
            snprintf(cmd, sizeof(cmd), "mv '%s'/* '%s'/ 2>/dev/null; rmdir '%s' 2>/dev/null",
                     only_subdir, dest_dir, only_subdir);
            system(cmd);
        }
    }

    /* Ensure failure/success subdirs exist */
    char sub[512];
    snprintf(sub, sizeof(sub), "%s/failure", dest_dir);
    mkdir(sub, 0755);
    snprintf(sub, sizeof(sub), "%s/success", dest_dir);
    mkdir(sub, 0755);

    pthread_mutex_lock(&g_proto.dl_mutex);
    g_proto.dl_progress.state = FD_DOWNLOAD_DONE;
    pthread_mutex_unlock(&g_proto.dl_mutex);

    fd_log("Download: complete, dataset=%s\n", g_proto.dl_name);
    g_proto.dl_thread_running = 0;
    return NULL;
}

int fault_detect_download_dataset(const char *url, const char *name)
{
    if (!url || !url[0] || !name || !name[0])
        return -1;
    if (strstr(name, "..") || strchr(name, '/'))
        return -1;
    if (g_proto.dl_thread_running)
        return -1;

    snprintf(g_proto.dl_url, sizeof(g_proto.dl_url), "%s", url);
    snprintf(g_proto.dl_name, sizeof(g_proto.dl_name), "%s", name);
    g_proto.dl_cancel = 0;
    g_proto.dl_thread_running = 1;

    if (pthread_create(&g_proto.dl_thread, NULL, fd_download_thread_func, NULL) != 0) {
        g_proto.dl_thread_running = 0;
        return -1;
    }
    pthread_detach(g_proto.dl_thread);
    return 0;
}

fd_download_progress_t fault_detect_get_download_progress(void)
{
    fd_download_progress_t p;
    pthread_mutex_lock(&g_proto.dl_mutex);
    p = g_proto.dl_progress;
    pthread_mutex_unlock(&g_proto.dl_mutex);
    return p;
}

void fault_detect_cancel_download(void)
{
    g_proto.dl_cancel = 1;
}

/* ============================================================================
 * Name/enum helpers
 * ============================================================================ */

static const char *g_strategy_names[] = {
    "or", "majority", "all", "verify", "classify", "classify_and", "and",
    "cnn", "protonet", "multiclass"
};

const char *fd_strategy_name(fd_strategy_t strategy)
{
    if (strategy >= 0 && strategy <= FD_STRATEGY_MULTICLASS)
        return g_strategy_names[strategy];
    return "unknown";
}

fd_strategy_t fd_strategy_from_name(const char *name)
{
    for (int i = 0; i <= FD_STRATEGY_MULTICLASS; i++) {
        if (strcmp(name, g_strategy_names[i]) == 0)
            return (fd_strategy_t)i;
    }
    return FD_STRATEGY_OR;
}

static const char *g_mclass_names[FD_MCLASS_COUNT] = {
    "Cracking", "Layer Shifting", "Spaghetti", "Stringing",
    "Success", "Under-Extrusion", "Warping"
};

const char *fd_fault_class_name(int fault_class)
{
    if (fault_class >= 0 && fault_class < FD_MCLASS_COUNT)
        return g_mclass_names[fault_class];
    return "Unknown";
}
