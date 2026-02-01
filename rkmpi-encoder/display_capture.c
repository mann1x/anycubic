/*
 * Display Framebuffer Capture Implementation
 */

#include "display_capture.h"
#include "frame_buffer.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <linux/fb.h>

#include "turbojpeg.h"

/* Model IDs for orientation detection */
#define MODEL_ID_K2P   "20021"
#define MODEL_ID_K3    "20024"
#define MODEL_ID_KS1   "20025"
#define MODEL_ID_K3M   "20026"
#define MODEL_ID_K3V2  "20027"
#define MODEL_ID_KS1M  "20029"

#define API_CFG_PATH "/userdata/app/gk/config/api.cfg"

/* External verbose flag */
extern int g_verbose;

/* Global display capture thread */
static pthread_t g_display_thread;
static DisplayCapture g_display_ctx;
static volatile int g_display_running = 0;

static void log_info(const char *fmt, ...) {
    if (g_verbose) {
        va_list args;
        va_start(args, fmt);
        fprintf(stderr, "[DISPLAY] ");
        vfprintf(stderr, fmt, args);
        va_end(args);
        fflush(stderr);
    }
}

static void log_error(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    fprintf(stderr, "[DISPLAY] ERROR: ");
    vfprintf(stderr, fmt, args);
    va_end(args);
    fflush(stderr);
}

const char *display_orientation_name(DisplayOrientation orient) {
    switch (orient) {
        case DISPLAY_ORIENT_NORMAL:     return "NORMAL";
        case DISPLAY_ORIENT_FLIP_180:   return "FLIP_180";
        case DISPLAY_ORIENT_ROTATE_90:  return "ROTATE_90";
        case DISPLAY_ORIENT_ROTATE_270: return "ROTATE_270";
        default:                        return "UNKNOWN";
    }
}

/*
 * Detect printer model and return appropriate screen orientation
 */
static DisplayOrientation detect_orientation(void) {
    FILE *f = fopen(API_CFG_PATH, "r");
    if (!f) {
        log_info("Cannot open %s, using default orientation\n", API_CFG_PATH);
        return DISPLAY_ORIENT_NORMAL;
    }

    char line[256];
    char model_id[32] = {0};

    while (fgets(line, sizeof(line), f)) {
        /* Look for model_id line */
        if (strncmp(line, "model_id", 8) == 0) {
            char *eq = strchr(line, '=');
            if (eq) {
                eq++;
                /* Skip whitespace */
                while (*eq == ' ' || *eq == '\t') eq++;
                /* Copy model ID, strip newline */
                int i = 0;
                while (*eq && *eq != '\n' && *eq != '\r' && i < (int)sizeof(model_id) - 1) {
                    model_id[i++] = *eq++;
                }
                model_id[i] = '\0';
                break;
            }
        }
    }
    fclose(f);

    if (model_id[0] == '\0') {
        log_info("Model ID not found, using default orientation\n");
        return DISPLAY_ORIENT_NORMAL;
    }

    log_info("Detected model ID: %s\n", model_id);

    /* Map model ID to orientation */
    if (strcmp(model_id, MODEL_ID_KS1) == 0 || strcmp(model_id, MODEL_ID_KS1M) == 0) {
        return DISPLAY_ORIENT_FLIP_180;
    }
    if (strcmp(model_id, MODEL_ID_K3M) == 0) {
        return DISPLAY_ORIENT_ROTATE_270;
    }
    if (strcmp(model_id, MODEL_ID_K3) == 0 || strcmp(model_id, MODEL_ID_K2P) == 0 ||
        strcmp(model_id, MODEL_ID_K3V2) == 0) {
        return DISPLAY_ORIENT_ROTATE_90;
    }

    return DISPLAY_ORIENT_NORMAL;
}

/*
 * Rotate BGRX pixels according to orientation
 * Input: src (fb_width x fb_height)
 * Output: dst (output_width x output_height)
 */
static void rotate_pixels(const uint32_t *src, uint32_t *dst,
                         int fb_width, int fb_height,
                         DisplayOrientation orient) {
    int x, y;

    switch (orient) {
        case DISPLAY_ORIENT_NORMAL:
            /* No rotation needed, just copy */
            memcpy(dst, src, fb_width * fb_height * sizeof(uint32_t));
            break;

        case DISPLAY_ORIENT_FLIP_180:
            /* 180 degree rotation */
            for (y = 0; y < fb_height; y++) {
                for (x = 0; x < fb_width; x++) {
                    dst[(fb_height - 1 - y) * fb_width + (fb_width - 1 - x)] =
                        src[y * fb_width + x];
                }
            }
            break;

        case DISPLAY_ORIENT_ROTATE_90:
            /* 90 degree clockwise: output is height x width */
            for (y = 0; y < fb_height; y++) {
                for (x = 0; x < fb_width; x++) {
                    dst[x * fb_height + (fb_height - 1 - y)] =
                        src[y * fb_width + x];
                }
            }
            break;

        case DISPLAY_ORIENT_ROTATE_270:
            /* 270 degree clockwise (90 counter-clockwise): output is height x width */
            for (y = 0; y < fb_height; y++) {
                for (x = 0; x < fb_width; x++) {
                    dst[(fb_width - 1 - x) * fb_height + y] =
                        src[y * fb_width + x];
                }
            }
            break;
    }
}

int display_capture_init(DisplayCapture *ctx, int fps) {
    memset(ctx, 0, sizeof(*ctx));
    ctx->fps = fps > 0 ? fps : DISPLAY_DEFAULT_FPS;

    /* Detect orientation */
    ctx->orientation = detect_orientation();
    log_info("Screen orientation: %s\n", display_orientation_name(ctx->orientation));

    /* Open framebuffer */
    ctx->fb_fd = open("/dev/fb0", O_RDONLY);
    if (ctx->fb_fd < 0) {
        log_error("Cannot open /dev/fb0: %s\n", strerror(errno));
        return -1;
    }

    /* Get framebuffer info */
    struct fb_var_screeninfo vinfo;
    if (ioctl(ctx->fb_fd, FBIOGET_VSCREENINFO, &vinfo) < 0) {
        log_error("FBIOGET_VSCREENINFO failed: %s\n", strerror(errno));
        close(ctx->fb_fd);
        return -1;
    }

    ctx->fb_width = vinfo.xres;
    ctx->fb_height = vinfo.yres;
    ctx->fb_size = ctx->fb_width * ctx->fb_height * sizeof(uint32_t);

    log_info("Framebuffer: %dx%d, %d bpp\n", ctx->fb_width, ctx->fb_height, vinfo.bits_per_pixel);

    if (vinfo.bits_per_pixel != 32) {
        log_error("Unsupported bpp: %d (expected 32)\n", vinfo.bits_per_pixel);
        close(ctx->fb_fd);
        return -1;
    }

    /* Map framebuffer (read-only) */
    ctx->fb_pixels = mmap(NULL, ctx->fb_size, PROT_READ, MAP_SHARED, ctx->fb_fd, 0);
    if (ctx->fb_pixels == MAP_FAILED) {
        log_error("mmap failed: %s\n", strerror(errno));
        close(ctx->fb_fd);
        return -1;
    }

    /* Calculate output dimensions based on orientation */
    if (ctx->orientation == DISPLAY_ORIENT_ROTATE_90 ||
        ctx->orientation == DISPLAY_ORIENT_ROTATE_270) {
        ctx->output_width = ctx->fb_height;
        ctx->output_height = ctx->fb_width;
    } else {
        ctx->output_width = ctx->fb_width;
        ctx->output_height = ctx->fb_height;
    }

    log_info("Output dimensions: %dx%d\n", ctx->output_width, ctx->output_height);

    /* Allocate rotation buffer if needed */
    if (ctx->orientation != DISPLAY_ORIENT_NORMAL) {
        ctx->rotate_buf = malloc(ctx->fb_size);
        if (!ctx->rotate_buf) {
            log_error("Failed to allocate rotation buffer\n");
            munmap(ctx->fb_pixels, ctx->fb_size);
            close(ctx->fb_fd);
            return -1;
        }
    }

    /* Initialize TurboJPEG compressor */
    ctx->tj_handle = tjInitCompress();
    if (!ctx->tj_handle) {
        log_error("tjInitCompress failed\n");
        if (ctx->rotate_buf) free(ctx->rotate_buf);
        munmap(ctx->fb_pixels, ctx->fb_size);
        close(ctx->fb_fd);
        return -1;
    }

    ctx->running = 1;
    return 0;
}

void display_capture_cleanup(DisplayCapture *ctx) {
    ctx->running = 0;

    if (ctx->tj_handle) {
        tjDestroy(ctx->tj_handle);
        ctx->tj_handle = NULL;
    }

    if (ctx->rotate_buf) {
        free(ctx->rotate_buf);
        ctx->rotate_buf = NULL;
    }

    if (ctx->fb_pixels && ctx->fb_pixels != MAP_FAILED) {
        munmap(ctx->fb_pixels, ctx->fb_size);
        ctx->fb_pixels = NULL;
    }

    if (ctx->fb_fd >= 0) {
        close(ctx->fb_fd);
        ctx->fb_fd = -1;
    }
}

size_t display_capture_frame(DisplayCapture *ctx, uint8_t *jpeg_buf, size_t jpeg_buf_size) {
    if (!ctx->running || !ctx->fb_pixels || !ctx->tj_handle) {
        return 0;
    }

    const uint32_t *pixels;

    /* Apply rotation if needed */
    if (ctx->orientation != DISPLAY_ORIENT_NORMAL && ctx->rotate_buf) {
        rotate_pixels(ctx->fb_pixels, (uint32_t *)ctx->rotate_buf,
                     ctx->fb_width, ctx->fb_height, ctx->orientation);
        pixels = (const uint32_t *)ctx->rotate_buf;
    } else {
        pixels = ctx->fb_pixels;
    }

    /* Compress to JPEG using TurboJPEG
     * TJPF_BGRX matches the framebuffer format (B G R X) */
    unsigned char *jpeg_out = jpeg_buf;
    unsigned long jpeg_size = 0;

    int ret = tjCompress2(ctx->tj_handle,
                         (const unsigned char *)pixels,
                         ctx->output_width,
                         0,  /* pitch (0 = default) */
                         ctx->output_height,
                         TJPF_BGRX,
                         &jpeg_out,
                         &jpeg_size,
                         TJSAMP_420,
                         DISPLAY_JPEG_QUALITY,
                         TJFLAG_FASTDCT | TJFLAG_NOREALLOC);

    if (ret != 0) {
        log_error("tjCompress2 failed: %s\n", tjGetErrorStr());
        return 0;
    }

    return (size_t)jpeg_size;
}

/*
 * Display capture thread function
 */
static void *display_capture_thread(void *arg) {
    DisplayCapture *ctx = (DisplayCapture *)arg;

    /* Allocate JPEG output buffer */
    size_t jpeg_buf_size = ctx->output_width * ctx->output_height * 3;
    uint8_t *jpeg_buf = malloc(jpeg_buf_size);
    if (!jpeg_buf) {
        log_error("Failed to allocate JPEG buffer\n");
        return NULL;
    }

    /* Calculate frame interval */
    uint64_t frame_interval_us = 1000000 / ctx->fps;
    struct timespec ts;

    log_info("Capture thread started: %d fps (interval %lu us)\n",
             ctx->fps, (unsigned long)frame_interval_us);

    while (ctx->running && g_display_running) {
        clock_gettime(CLOCK_MONOTONIC, &ts);
        uint64_t start_us = (uint64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;

        /* Capture frame */
        size_t jpeg_size = display_capture_frame(ctx, jpeg_buf, jpeg_buf_size);

        if (jpeg_size > 0) {
            /* Write to display frame buffer */
            frame_buffer_write(&g_display_buffer, jpeg_buf, jpeg_size, 0, 1);
        }

        /* Sleep until next frame */
        clock_gettime(CLOCK_MONOTONIC, &ts);
        uint64_t elapsed_us = ((uint64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000) - start_us;

        if (elapsed_us < frame_interval_us) {
            usleep(frame_interval_us - elapsed_us);
        }
    }

    free(jpeg_buf);
    log_info("Capture thread stopped\n");
    return NULL;
}

int display_capture_start(int fps) {
    if (g_display_running) {
        log_info("Display capture already running\n");
        return 0;
    }

    /* Initialize display capture context */
    if (display_capture_init(&g_display_ctx, fps) != 0) {
        return -1;
    }

    g_display_running = 1;

    /* Start capture thread */
    if (pthread_create(&g_display_thread, NULL, display_capture_thread, &g_display_ctx) != 0) {
        log_error("Failed to create display capture thread\n");
        display_capture_cleanup(&g_display_ctx);
        g_display_running = 0;
        return -1;
    }

    log_info("Display capture started at %d fps\n", fps);
    return 0;
}

void display_capture_stop(void) {
    if (!g_display_running) {
        return;
    }

    g_display_running = 0;
    g_display_ctx.running = 0;

    /* Wake up any waiting threads */
    frame_buffer_broadcast(&g_display_buffer);

    pthread_join(g_display_thread, NULL);
    display_capture_cleanup(&g_display_ctx);

    log_info("Display capture stopped\n");
}

int display_capture_is_running(void) {
    return g_display_running;
}
