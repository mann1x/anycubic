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
#include <errno.h>

/* Logging macros (match rkmpi_enc.c style) */
#define fd_log(fmt, ...) fprintf(stderr, "[FD] " fmt, ##__VA_ARGS__)
#define fd_err(fmt, ...) fprintf(stderr, "[FD] ERROR: " fmt, ##__VA_ARGS__)

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

    /* ProtoNet prototypes */
    float prototypes[2][EMB_DIM];
    float proto_norms[2];
    int prototypes_loaded;

    /* Initialized flag */
    int initialized;
} g_fd;

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

static void fd_purge_memory(void)
{
    FILE *f = fopen("/proc/sys/vm/drop_caches", "w");
    if (f) { fputs("3\n", f); fclose(f); }
    f = fopen("/proc/sys/vm/compact_memory", "w");
    if (f) { fputs("1\n", f); fclose(f); }
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
        /* CMA failure: purge and retry once */
        fd_log("CMA alloc failed for input, purging memory...\n");
        fd_purge_memory();
        usleep(100000);  /* 100ms settle */
        m->input_mem = g_rknn.create_mem(m->ctx,
                                          m->input_attr.size_with_stride);
        if (!m->input_mem) {
            fd_err("CMA alloc failed for input after purge\n");
            ret = -1;
            goto fail;
        }
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
            ret = -1;
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

static int fd_model_run(fd_rknn_model_t *m, const uint8_t *input_data)
{
    memcpy(m->input_mem->virt_addr, input_data, m->input_size);
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

    /* NC1HWC2 dequantization — linear read works for H=W=1 */
    for (int i = 0; i < n; i++)
        out_buf[i] = ((float)raw[i] - zp) * scale;

    return n;
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

    img->data = (uint8_t *)malloc(width * height * 3);
    if (!img->data) {
        tjDestroy(handle);
        return -1;
    }
    img->width = width;
    img->height = height;

    ret = tjDecompress2(handle, jpeg_data, (unsigned long)jpeg_size,
                        img->data, width, 0, height, TJPF_RGB, 0);
    tjDestroy(handle);
    if (ret < 0) {
        free(img->data);
        img->data = NULL;
        return -1;
    }
    return 0;
}

/* Fused resize (shortest side→256) + center crop (224×224), bilinear */
static void fd_resize_crop_bilinear(const uint8_t *src, int sw, int sh,
                                     uint8_t *dst, int dw, int dh)
{
    float scale = 256.0f / (sw < sh ? sw : sh);
    int rw = (int)(sw * scale);
    int rh = (int)(sh * scale);
    int cx = (rw - dw) / 2;
    int cy = (rh - dh) / 2;
    float x_ratio = (float)sw / (float)rw;
    float y_ratio = (float)sh / (float)rh;

    for (int dy = 0; dy < dh; dy++) {
        float sy_f = (dy + cy) * y_ratio;
        int sy = (int)sy_f;
        float y_diff = sy_f - sy;
        if (sy >= sh - 1) { sy = sh - 2; y_diff = 1.0f; }

        const uint8_t *row0 = src + sy * sw * 3;
        const uint8_t *row1 = src + (sy + 1) * sw * 3;

        for (int dx = 0; dx < dw; dx++) {
            float sx_f = (dx + cx) * x_ratio;
            int sx = (int)sx_f;
            float x_diff = sx_f - sx;
            if (sx >= sw - 1) { sx = sw - 2; x_diff = 1.0f; }

            const uint8_t *a = row0 + sx * 3;
            const uint8_t *b = row0 + (sx + 1) * 3;
            const uint8_t *c = row1 + sx * 3;
            const uint8_t *d = row1 + (sx + 1) * 3;

            float w00 = (1.0f - x_diff) * (1.0f - y_diff);
            float w10 = x_diff * (1.0f - y_diff);
            float w01 = (1.0f - x_diff) * y_diff;
            float w11 = x_diff * y_diff;

            for (int ch = 0; ch < 3; ch++) {
                float val = a[ch] * w00 + b[ch] * w10 +
                            c[ch] * w01 + d[ch] * w11;
                int iv = (int)(val + 0.5f);
                if (iv < 0) iv = 0;
                if (iv > 255) iv = 255;
                dst[(dy * dw + dx) * 3 + ch] = (uint8_t)iv;
            }
        }
    }
}

/* Full preprocessing: resize+crop then 3-channel grayscale (BT.601) */
static int fd_preprocess(const fd_image_t *img, uint8_t *out_buf)
{
    uint8_t *crop_buf = (uint8_t *)malloc(FD_MODEL_INPUT_BYTES);
    if (!crop_buf) return -1;

    fd_resize_crop_bilinear(img->data, img->width, img->height,
                            crop_buf, FD_MODEL_INPUT_SIZE, FD_MODEL_INPUT_SIZE);

    int npix = FD_MODEL_INPUT_SIZE * FD_MODEL_INPUT_SIZE;
    for (int i = 0; i < npix; i++) {
        uint8_t r = crop_buf[i * 3 + 0];
        uint8_t g = crop_buf[i * 3 + 1];
        uint8_t b = crop_buf[i * 3 + 2];
        uint8_t gray = (uint8_t)((306 * r + 601 * g + 117 * b) >> 10);
        out_buf[i * 3 + 0] = gray;
        out_buf[i * 3 + 1] = gray;
        out_buf[i * 3 + 2] = gray;
    }

    free(crop_buf);
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

/* ============================================================================
 * Model path resolution
 * ============================================================================ */

static int fd_resolve_model_path(fd_model_class_t cls, const char *model_name,
                                  char *path, size_t path_size)
{
    const char *class_dir;
    const char *filename;

    switch (cls) {
    case FD_MODEL_CNN:
        class_dir = "cnn";
        filename = "model.rknn";
        break;
    case FD_MODEL_PROTONET:
        class_dir = "protonet";
        filename = "encoder.rknn";
        break;
    case FD_MODEL_MULTICLASS:
        class_dir = "multiclass";
        filename = "multiclass.rknn";
        break;
    default:
        return -1;
    }

    snprintf(path, path_size, "%s/%s/%s/%s",
             g_fd.models_base_dir, class_dir, model_name, filename);

    /* Check if file exists */
    if (access(path, R_OK) != 0) {
        /* For multiclass, try any .rknn file */
        if (cls == FD_MODEL_MULTICLASS) {
            char dir_path[512];
            snprintf(dir_path, sizeof(dir_path), "%s/%s/%s",
                     g_fd.models_base_dir, class_dir, model_name);
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

/* Default thresholds — calibrated for INT8 on RV1106 hardware.
 * These match the detect tool's per-strategy defaults.
 * See BigEdge-FDM-Models CLAUDE.md for calibration details. */
static void fd_get_thresholds(int strategy,
                               float *cnn_th, float *proto_th, float *multi_th)
{
    switch (strategy) {
    case FD_STRATEGY_OR:
    case FD_STRATEGY_VERIFY:
    case FD_STRATEGY_CLASSIFY:
    case FD_STRATEGY_MAJORITY:
    case FD_STRATEGY_ALL:
    case FD_STRATEGY_CNN:
    case FD_STRATEGY_PROTONET:
    case FD_STRATEGY_MULTICLASS:
    default:
        *cnn_th   = 0.29f;   /* Printer-calibrated: idle max=0.286, fault min=0.292 */
        *proto_th = 0.60f;   /* Printer-calibrated: idle max=0.591, fault min=0.578 */
        break;
    }
    /* Multi-class threshold:
     * - VERIFY/CLASSIFY: low threshold (MC just labels fault type, doesn't decide binary)
     * - All others: 0.81 printer-calibrated (idle max=0.807, fault min=0.849) */
    if (strategy == FD_STRATEGY_VERIFY || strategy == FD_STRATEGY_CLASSIFY)
        *multi_th = 0.10f;
    else
        *multi_th = 0.81f;
}

static int fd_run_cnn(const uint8_t *input, fd_result_t *r, float threshold)
{
    fd_rknn_model_t model;
    char path[512];

    if (fd_resolve_model_path(FD_MODEL_CNN, g_fd.config.cnn_model,
                               path, sizeof(path)) < 0) {
        fd_err("CNN model not found: %s\n", g_fd.config.cnn_model);
        return -1;
    }

    if (fd_model_init(&model, path) < 0)
        return -1;

    double t0 = fd_get_time_ms();
    int ret = fd_model_run(&model, input);
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

    fd_softmax(logits, 2);
    fd_log("  CNN probs: fail=%.3f succ=%.3f (th=%.2f)\n",
           logits[0], logits[1], threshold);

    int cnn_class = logits[0] > threshold ? FD_CLASS_FAULT : FD_CLASS_OK;
    float cnn_conf = logits[0] > logits[1] ? logits[0] : logits[1];

    r->result = cnn_class;
    r->confidence = cnn_conf;
    return 0;
}

static int fd_run_protonet(const uint8_t *input, fd_result_t *r, float proto_threshold)
{
    fd_rknn_model_t model;
    char path[512];

    if (fd_resolve_model_path(FD_MODEL_PROTONET, g_fd.config.proto_model,
                               path, sizeof(path)) < 0) {
        fd_err("ProtoNet model not found: %s\n", g_fd.config.proto_model);
        return -1;
    }

    /* Load prototypes if not already loaded */
    if (!g_fd.prototypes_loaded) {
        char proto_path[512];
        snprintf(proto_path, sizeof(proto_path), "%s/protonet/%s/prototypes.bin",
                 g_fd.models_base_dir, g_fd.config.proto_model);
        if (fd_load_prototypes(proto_path) < 0)
            return -1;
    }

    if (fd_model_init(&model, path) < 0)
        return -1;

    double t0 = fd_get_time_ms();
    int ret = fd_model_run(&model, input);
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

    fd_log("  Proto cos: fail=%.4f succ=%.4f margin=%.4f (th=%.2f, norms: %.1f, %.1f)\n",
           cos_fail, cos_succ, cos_margin, proto_threshold,
           g_fd.proto_norms[0], g_fd.proto_norms[1]);

    r->result = cos_margin > proto_threshold ? FD_CLASS_FAULT : FD_CLASS_OK;
    r->confidence = fabsf(cos_margin);
    return 0;
}

static int fd_run_multiclass(const uint8_t *input, fd_result_t *r, float multi_threshold)
{
    fd_rknn_model_t model;
    char path[512];

    if (fd_resolve_model_path(FD_MODEL_MULTICLASS, g_fd.config.multi_model,
                               path, sizeof(path)) < 0) {
        fd_err("Multiclass model not found: %s\n", g_fd.config.multi_model);
        return -1;
    }

    if (fd_model_init(&model, path) < 0)
        return -1;

    double t0 = fd_get_time_ms();
    int ret = fd_model_run(&model, input);
    if (ret < 0) {
        fd_err("Multiclass run failed: %d\n", ret);
        fd_model_release(&model);
        return -1;
    }

    float logits[FD_MCLASS_COUNT] = {0};
    fd_model_get_output(&model, 0, logits, FD_MCLASS_COUNT);
    double t1 = fd_get_time_ms();
    r->multi_ms = (float)(t1 - t0);

    /* Log raw INT8 values before releasing model memory */
    {
        const int8_t *raw = (const int8_t *)model.output_mems[0]->virt_addr;
        fd_log("  Multi raw INT8[0..6]: %d %d %d %d %d %d %d (zp=%d scale=%.6f n_elems=%u)\n",
               raw[0], raw[1], raw[2], raw[3], raw[4], raw[5], raw[6],
               (int)model.output_attrs[0].zp, model.output_attrs[0].scale,
               model.output_attrs[0].n_elems);
    }

    fd_model_release(&model);

    fd_softmax(logits, FD_MCLASS_COUNT);

    fd_log("  Multi probs: Crack=%.3f LayShft=%.3f Spag=%.3f Str=%.3f "
           "Succ=%.3f UndrEx=%.3f Warp=%.3f\n",
           logits[0], logits[1], logits[2], logits[3],
           logits[4], logits[5], logits[6]);

    /* Find argmax */
    int best = 0;
    for (int i = 1; i < FD_MCLASS_COUNT; i++)
        if (logits[i] > logits[best]) best = i;
    r->fault_class = best;
    snprintf(r->fault_class_name, sizeof(r->fault_class_name), "%s",
             fd_fault_class_name(best));

    /* Binary collapse: FAULT if 1 - p(Success) > threshold */
    float multi_conf = 1.0f - logits[FD_MCLASS_SUCCESS];
    fd_log("  Multi binary: 1-p(Succ)=%.3f th=%.2f -> %s\n",
           multi_conf, multi_threshold,
           multi_conf > multi_threshold ? "FAULT" : "OK");
    r->result = multi_conf > multi_threshold ? FD_CLASS_FAULT : FD_CLASS_OK;
    r->confidence = multi_conf;
    return 0;
}

/* ============================================================================
 * Combined detection + strategy (from detect.c)
 * ============================================================================ */

static void fd_run_detection(const uint8_t *preprocessed, fd_result_t *result,
                              fd_config_t *cfg)
{
    double t0 = fd_get_time_ms();
    memset(result, 0, sizeof(*result));
    snprintf(result->fault_class_name, sizeof(result->fault_class_name), "-");

    /* Get strategy-dependent thresholds (calibrated for INT8 on RV1106) */
    float cnn_th, proto_th, multi_th;
    fd_get_thresholds(cfg->strategy, &cnn_th, &proto_th, &multi_th);

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
    }

    /* Per-model results */
    int cnn_class = FD_CLASS_OK, proto_class = FD_CLASS_OK, multi_class = FD_CLASS_OK;
    float cnn_conf = 0.5f, proto_conf = 0.0f, multi_conf = 0.5f;
    fd_result_t model_result;

    /* Run CNN */
    if (have_cnn) {
        memset(&model_result, 0, sizeof(model_result));
        if (fd_run_cnn(preprocessed, &model_result, cnn_th) < 0) {
            result->result = FD_CLASS_OK;
            result->total_ms = (float)(fd_get_time_ms() - t0);
            return;
        }
        cnn_class = model_result.result;
        cnn_conf = model_result.confidence;
        result->cnn_ms = model_result.cnn_ms;
    }

    /* Run ProtoNet */
    if (have_proto) {
        memset(&model_result, 0, sizeof(model_result));
        if (fd_run_protonet(preprocessed, &model_result, proto_th) < 0) {
            result->result = FD_CLASS_OK;
            result->total_ms = (float)(fd_get_time_ms() - t0);
            return;
        }
        proto_class = model_result.result;
        proto_conf = model_result.confidence;
        result->proto_ms = model_result.proto_ms;
    }

    /* VERIFY/CLASSIFY: only run multiclass if CNN or ProtoNet flagged FAULT */
    int run_multi = have_multi;
    if (run_multi && (cfg->strategy == FD_STRATEGY_VERIFY ||
                      cfg->strategy == FD_STRATEGY_CLASSIFY)) {
        int or_fault = 0;
        if (have_cnn && cnn_class == FD_CLASS_FAULT) or_fault = 1;
        if (have_proto && proto_class == FD_CLASS_FAULT) or_fault = 1;
        run_multi = or_fault;
    }

    /* Run Multiclass */
    if (run_multi) {
        memset(&model_result, 0, sizeof(model_result));
        if (fd_run_multiclass(preprocessed, &model_result, multi_th) == 0) {
            multi_class = model_result.result;
            multi_conf = model_result.confidence;
            result->multi_ms = model_result.multi_ms;
            result->fault_class = model_result.fault_class;
            snprintf(result->fault_class_name,
                     sizeof(result->fault_class_name), "%s",
                     model_result.fault_class_name);
        }
    }

    /* Log per-model details */
    if (have_cnn)
        fd_log("  CNN: %s conf=%.3f (%.0fms)\n",
               cnn_class == FD_CLASS_FAULT ? "FAULT" : "OK", cnn_conf, result->cnn_ms);
    if (have_proto)
        fd_log("  Proto: %s margin=%.3f (%.0fms)\n",
               proto_class == FD_CLASS_FAULT ? "FAULT" : "OK", proto_conf, result->proto_ms);
    if (run_multi)
        fd_log("  Multi: %s conf=%.3f class=%s (%.0fms)\n",
               multi_class == FD_CLASS_FAULT ? "FAULT" : "OK", multi_conf,
               result->fault_class_name, result->multi_ms);

    /* Combine results by strategy */
    int n_models = 0, n_fault = 0;
    int votes[3] = {-1, -1, -1};

    if (have_cnn) {
        votes[0] = cnn_class; n_models++;
        if (cnn_class == FD_CLASS_FAULT) n_fault++;
    }
    if (have_proto) {
        votes[1] = proto_class; n_models++;
        if (proto_class == FD_CLASS_FAULT) n_fault++;
    }
    if (run_multi) {
        votes[2] = multi_class; n_models++;
        if (multi_class == FD_CLASS_FAULT) n_fault++;
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
    }

    /* Count agreement */
    result->agreement = 0;
    for (int i = 0; i < 3; i++) {
        if (votes[i] >= 0 && votes[i] == result->result)
            result->agreement++;
    }

    /* Combined confidence */
    float conf_sum = 0.0f;
    int conf_n = 0;
    if (have_cnn) { conf_sum += cnn_conf; conf_n++; }
    if (have_proto) {
        conf_sum += 0.5f + 0.5f * proto_conf;
        conf_n++;
    }
    if (run_multi) {
        conf_sum += multi_conf > 0.5f ? multi_conf : 1.0f - multi_conf;
        conf_n++;
    }

    if (conf_n > 0) {
        if (result->agreement == n_models)
            result->confidence = conf_sum / conf_n;
        else
            result->confidence = 0.5f * conf_sum / conf_n;
    } else {
        result->confidence = 0.5f;
    }

    result->total_ms = (float)(fd_get_time_ms() - t0);
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

    uint8_t *preprocessed = (uint8_t *)malloc(FD_MODEL_INPUT_BYTES);
    if (!preprocessed) {
        fd_err("Failed to allocate preprocess buffer\n");
        fd_set_state(FD_STATUS_ERROR, NULL, "malloc failed");
        return NULL;
    }

    int consecutive_ok = 0;
    int use_verify_interval = 0;

    while (!g_fd.thread_stop) {
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

        /* Check available memory */
        int avail_mb = fd_get_available_memory_mb();
        if (avail_mb > 0 && avail_mb < cfg.min_free_mem_mb) {
            fd_set_state(FD_STATUS_MEM_LOW, NULL, "memory low");
            fd_log("Skipping cycle: %d MB available < %d MB threshold\n",
                   avail_mb, cfg.min_free_mem_mb);
            continue;
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
        pthread_mutex_unlock(&g_fd.frame_mutex);

        if (!jpeg_copy) continue;

        fd_set_state(FD_STATUS_ACTIVE, NULL, NULL);

        /* Decode JPEG */
        fd_image_t img = {0};
        if (fd_decode_jpeg(jpeg_copy, jpeg_size, &img) < 0) {
            free(jpeg_copy);
            fd_set_state(FD_STATUS_ERROR, NULL, "JPEG decode failed");
            continue;
        }
        free(jpeg_copy);

        /* Preprocess */
        if (fd_preprocess(&img, preprocessed) < 0) {
            free(img.data);
            fd_set_state(FD_STATUS_ERROR, NULL, "preprocess failed");
            continue;
        }
        free(img.data);

        /* Run detection */
        fd_result_t result;
        fd_run_detection(preprocessed, &result, &cfg);

        /* Update state */
        pthread_mutex_lock(&g_fd.state_mutex);
        g_fd.state.status = FD_STATUS_ENABLED;
        g_fd.state.last_result = result;
        g_fd.state.last_check_time = (uint64_t)time(NULL);
        g_fd.state.cycle_count++;
        g_fd.state.error_msg[0] = '\0';
        pthread_mutex_unlock(&g_fd.state_mutex);

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
    fd_log("Detection thread stopped\n");
    return NULL;
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
        if (fd_resolve_model_path(FD_MODEL_CNN, cfg.cnn_model,
                                   path, sizeof(path)) < 0) {
            fd_err("CNN model not found: %s\n", cfg.cnn_model);
            fd_set_state(FD_STATUS_ERROR, NULL, "CNN model not found");
            return -1;
        }
    }
    if (cfg.proto_enabled || cfg.strategy == FD_STRATEGY_PROTONET) {
        char path[512];
        if (fd_resolve_model_path(FD_MODEL_PROTONET, cfg.proto_model,
                                   path, sizeof(path)) < 0) {
            fd_err("ProtoNet model not found: %s\n", cfg.proto_model);
            fd_set_state(FD_STATUS_ERROR, NULL, "ProtoNet model not found");
            return -1;
        }
        char proto_path[512];
        snprintf(proto_path, sizeof(proto_path),
                 "%s/protonet/%s/prototypes.bin",
                 g_fd.models_base_dir, cfg.proto_model);
        if (access(proto_path, R_OK) != 0) {
            fd_err("ProtoNet prototypes not found: %s\n", proto_path);
            fd_set_state(FD_STATUS_ERROR, NULL, "prototypes.bin not found");
            return -1;
        }
    }
    if (cfg.multi_enabled || cfg.strategy == FD_STRATEGY_MULTICLASS) {
        char path[512];
        if (fd_resolve_model_path(FD_MODEL_MULTICLASS, cfg.multi_model,
                                   path, sizeof(path)) < 0) {
            fd_err("Multiclass model not found: %s\n", cfg.multi_model);
            fd_set_state(FD_STATUS_ERROR, NULL, "Multiclass model not found");
            return -1;
        }
    }
    fd_log("Model files verified\n");

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
    pthread_cond_destroy(&g_fd.frame_cond);

    g_fd.initialized = 0;
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

    /* Invalidate prototypes cache when model changes */
    g_fd.prototypes_loaded = 0;
}

int fault_detect_scan_models(fd_model_info_t *models, int max_models)
{
    int count = 0;
    const char *class_dirs[] = {"cnn", "protonet", "multiclass"};
    fd_model_class_t class_types[] = {FD_MODEL_CNN, FD_MODEL_PROTONET,
                                       FD_MODEL_MULTICLASS};

    for (int c = 0; c < 3 && count < max_models; c++) {
        char dir_path[512];
        snprintf(dir_path, sizeof(dir_path), "%s/%s",
                 g_fd.models_base_dir, class_dirs[c]);

        DIR *dir = opendir(dir_path);
        if (!dir) continue;

        struct dirent *ent;
        while ((ent = readdir(dir)) != NULL && count < max_models) {
            if (ent->d_name[0] == '.') continue;

            /* Check if it's a directory */
            char sub_path[512];
            snprintf(sub_path, sizeof(sub_path), "%s/%s",
                     dir_path, ent->d_name);
            struct stat st;
            if (stat(sub_path, &st) != 0 || !S_ISDIR(st.st_mode))
                continue;

            /* Verify model file exists */
            char model_path[512];
            if (fd_resolve_model_path(class_types[c], ent->d_name,
                                       model_path, sizeof(model_path)) < 0)
                continue;

            snprintf(models[count].name, sizeof(models[count].name),
                     "%.63s", ent->d_name);
            snprintf(models[count].path, sizeof(models[count].path),
                     "%.255s", sub_path);
            models[count].cls = class_types[c];
            count++;
        }
        closedir(dir);
    }
    return count;
}

int fault_detect_npu_available(void)
{
    return g_rknn.handle != NULL ? 1 : 0;
}

/* ============================================================================
 * Name/enum helpers
 * ============================================================================ */

static const char *g_strategy_names[] = {
    "or", "majority", "all", "verify", "classify",
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
