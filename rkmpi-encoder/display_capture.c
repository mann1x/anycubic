/*
 * Display Framebuffer Capture Implementation
 * Uses RV1106 hardware VENC for JPEG encoding (not TurboJPEG)
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

/* RKMPI headers for hardware JPEG encoding */
#include "rk_mpi_sys.h"
#include "rk_mpi_venc.h"
#include "rk_mpi_mb.h"
#include "rk_mpi_mmz.h"
#include "rk_comm_venc.h"
#include "rk_comm_video.h"

/* Model IDs for orientation detection */
#define MODEL_ID_K2P   "20021"
#define MODEL_ID_K3    "20024"
#define MODEL_ID_KS1   "20025"
#define MODEL_ID_K3M   "20026"
#define MODEL_ID_K3V2  "20027"
#define MODEL_ID_KS1M  "20029"

#define API_CFG_PATH "/userdata/app/gk/config/api.cfg"

/* VENC channel for display JPEG encoding (separate from camera channels 0,1) */
#define VENC_CHN_DISPLAY  2

/* External verbose flag */
extern int g_verbose;

/* Global display capture thread */
static pthread_t g_display_thread;
static DisplayCapture g_display_ctx;
static volatile int g_display_running = 0;

/* DMA buffer for NV12 data */
static MB_BLK g_display_mb = MB_INVALID_HANDLE;
static void *g_display_mb_vaddr = NULL;

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
 * Convert BGRX (32-bit) to NV12 (YUV420SP)
 * BGRX: B G R X B G R X ... (4 bytes per pixel)
 * NV12: Y plane, then interleaved UV plane (half resolution)
 *
 * RGB to YUV conversion (BT.601):
 *   Y  =  0.299*R + 0.587*G + 0.114*B
 *   U  = -0.169*R - 0.331*G + 0.500*B + 128
 *   V  =  0.500*R - 0.419*G - 0.081*B + 128
 */
static void bgrx_to_nv12(const uint32_t *bgrx, uint8_t *nv12_y, uint8_t *nv12_uv,
                         int width, int height) {
    /* Process 2x2 blocks for UV subsampling */
    for (int y = 0; y < height; y += 2) {
        const uint32_t *row0 = bgrx + y * width;
        const uint32_t *row1 = row0 + width;
        uint8_t *y_row0 = nv12_y + y * width;
        uint8_t *y_row1 = y_row0 + width;
        uint8_t *uv_row = nv12_uv + (y / 2) * width;

        for (int x = 0; x < width; x += 2) {
            /* Extract BGRX pixels (2x2 block) */
            uint32_t p00 = row0[x];
            uint32_t p01 = row0[x + 1];
            uint32_t p10 = row1[x];
            uint32_t p11 = row1[x + 1];

            /* Extract RGB components (BGRX format: B=byte0, G=byte1, R=byte2, X=byte3) */
            int b00 = (p00 >> 0) & 0xFF, g00 = (p00 >> 8) & 0xFF, r00 = (p00 >> 16) & 0xFF;
            int b01 = (p01 >> 0) & 0xFF, g01 = (p01 >> 8) & 0xFF, r01 = (p01 >> 16) & 0xFF;
            int b10 = (p10 >> 0) & 0xFF, g10 = (p10 >> 8) & 0xFF, r10 = (p10 >> 16) & 0xFF;
            int b11 = (p11 >> 0) & 0xFF, g11 = (p11 >> 8) & 0xFF, r11 = (p11 >> 16) & 0xFF;

            /* Calculate Y for each pixel (fixed-point: *256 then >>8) */
            y_row0[x]     = (uint8_t)((66 * r00 + 129 * g00 + 25 * b00 + 128) >> 8) + 16;
            y_row0[x + 1] = (uint8_t)((66 * r01 + 129 * g01 + 25 * b01 + 128) >> 8) + 16;
            y_row1[x]     = (uint8_t)((66 * r10 + 129 * g10 + 25 * b10 + 128) >> 8) + 16;
            y_row1[x + 1] = (uint8_t)((66 * r11 + 129 * g11 + 25 * b11 + 128) >> 8) + 16;

            /* Average RGB for UV calculation (2x2 block -> 1 UV pair) */
            int r_avg = (r00 + r01 + r10 + r11) >> 2;
            int g_avg = (g00 + g01 + g10 + g11) >> 2;
            int b_avg = (b00 + b01 + b10 + b11) >> 2;

            /* Calculate U and V */
            int u = ((-38 * r_avg - 74 * g_avg + 112 * b_avg + 128) >> 8) + 128;
            int v = ((112 * r_avg - 94 * g_avg - 18 * b_avg + 128) >> 8) + 128;

            /* Clamp to [0, 255] */
            uv_row[x]     = (uint8_t)(u < 0 ? 0 : (u > 255 ? 255 : u));  /* U */
            uv_row[x + 1] = (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));  /* V */
        }
    }
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

/*
 * Initialize VENC for display JPEG encoding
 */
static int init_display_venc(int width, int height, int fps, int quality) {
    RK_S32 ret;
    VENC_CHN_ATTR_S stAttr;
    VENC_RECV_PIC_PARAM_S stRecvParam;

    memset(&stAttr, 0, sizeof(stAttr));
    memset(&stRecvParam, 0, sizeof(stRecvParam));

    /* MJPEG encoder for display frames */
    stAttr.stVencAttr.enType = RK_VIDEO_ID_MJPEG;
    stAttr.stVencAttr.enPixelFormat = RK_FMT_YUV420SP;  /* NV12 input */
    stAttr.stVencAttr.u32PicWidth = width;
    stAttr.stVencAttr.u32PicHeight = height;
    stAttr.stVencAttr.u32VirWidth = width;
    stAttr.stVencAttr.u32VirHeight = height;
    stAttr.stVencAttr.u32StreamBufCnt = 2;
    stAttr.stVencAttr.u32BufSize = width * height * 3 / 2;
    stAttr.stVencAttr.enMirror = MIRROR_NONE;

    /* Fixed quality for JPEG */
    stAttr.stRcAttr.enRcMode = VENC_RC_MODE_MJPEGFIXQP;
    stAttr.stRcAttr.stMjpegFixQp.u32Qfactor = quality;
    stAttr.stRcAttr.stMjpegFixQp.u32SrcFrameRateNum = fps;
    stAttr.stRcAttr.stMjpegFixQp.u32SrcFrameRateDen = 1;
    stAttr.stRcAttr.stMjpegFixQp.fr32DstFrameRateNum = fps;
    stAttr.stRcAttr.stMjpegFixQp.fr32DstFrameRateDen = 1;

    ret = RK_MPI_VENC_CreateChn(VENC_CHN_DISPLAY, &stAttr);
    if (ret != RK_SUCCESS) {
        log_error("RK_MPI_VENC_CreateChn(DISPLAY) failed: 0x%x\n", ret);
        return -1;
    }

    stRecvParam.s32RecvPicNum = -1;  /* Continuous */
    ret = RK_MPI_VENC_StartRecvFrame(VENC_CHN_DISPLAY, &stRecvParam);
    if (ret != RK_SUCCESS) {
        log_error("RK_MPI_VENC_StartRecvFrame(DISPLAY) failed: 0x%x\n", ret);
        RK_MPI_VENC_DestroyChn(VENC_CHN_DISPLAY);
        return -1;
    }

    log_info("VENC DISPLAY initialized: %dx%d, quality=%d, fps=%d\n",
             width, height, quality, fps);
    return 0;
}

static void cleanup_display_venc(void) {
    RK_MPI_VENC_StopRecvFrame(VENC_CHN_DISPLAY);
    RK_MPI_VENC_DestroyChn(VENC_CHN_DISPLAY);
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

    /* Allocate rotation buffer (always needed for BGRX storage before NV12 conversion) */
    ctx->rotate_buf = malloc(ctx->fb_size);
    if (!ctx->rotate_buf) {
        log_error("Failed to allocate rotation buffer\n");
        munmap(ctx->fb_pixels, ctx->fb_size);
        close(ctx->fb_fd);
        return -1;
    }

    /* Initialize VENC for display JPEG encoding */
    if (init_display_venc(ctx->output_width, ctx->output_height, ctx->fps, DISPLAY_JPEG_QUALITY) != 0) {
        free(ctx->rotate_buf);
        munmap(ctx->fb_pixels, ctx->fb_size);
        close(ctx->fb_fd);
        return -1;
    }

    /* Allocate DMA buffer for NV12 data */
    size_t nv12_size = ctx->output_width * ctx->output_height * 3 / 2;
    RK_S32 ret = RK_MPI_MMZ_Alloc(&g_display_mb, nv12_size, RK_MMZ_ALLOC_CACHEABLE);
    if (ret != RK_SUCCESS || g_display_mb == MB_INVALID_HANDLE) {
        log_error("RK_MPI_MMZ_Alloc failed: 0x%x, trying uncacheable\n", ret);
        ret = RK_MPI_MMZ_Alloc(&g_display_mb, nv12_size, RK_MMZ_ALLOC_UNCACHEABLE);
        if (ret != RK_SUCCESS || g_display_mb == MB_INVALID_HANDLE) {
            log_error("RK_MPI_MMZ_Alloc failed completely: 0x%x\n", ret);
            cleanup_display_venc();
            free(ctx->rotate_buf);
            munmap(ctx->fb_pixels, ctx->fb_size);
            close(ctx->fb_fd);
            return -1;
        }
    }
    g_display_mb_vaddr = RK_MPI_MMZ_Handle2VirAddr(g_display_mb);
    log_info("Allocated DMA buffer: %zu bytes\n", nv12_size);

    ctx->running = 1;
    return 0;
}

void display_capture_cleanup(DisplayCapture *ctx) {
    ctx->running = 0;

    /* Free DMA buffer */
    if (g_display_mb != MB_INVALID_HANDLE) {
        RK_MPI_MMZ_Free(g_display_mb);
        g_display_mb = MB_INVALID_HANDLE;
        g_display_mb_vaddr = NULL;
    }

    /* Cleanup VENC */
    cleanup_display_venc();

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
    if (!ctx->running || !ctx->fb_pixels || !g_display_mb_vaddr) {
        return 0;
    }

    const uint32_t *pixels;

    /* Apply rotation if needed */
    if (ctx->orientation != DISPLAY_ORIENT_NORMAL) {
        rotate_pixels(ctx->fb_pixels, (uint32_t *)ctx->rotate_buf,
                     ctx->fb_width, ctx->fb_height, ctx->orientation);
        pixels = (const uint32_t *)ctx->rotate_buf;
    } else {
        /* Copy to rotate_buf even if no rotation (needed for consistent pointer) */
        memcpy(ctx->rotate_buf, ctx->fb_pixels, ctx->fb_size);
        pixels = (const uint32_t *)ctx->rotate_buf;
    }

    /* Convert BGRX to NV12 */
    size_t y_size = ctx->output_width * ctx->output_height;
    uint8_t *nv12_y = (uint8_t *)g_display_mb_vaddr;
    uint8_t *nv12_uv = nv12_y + y_size;
    bgrx_to_nv12(pixels, nv12_y, nv12_uv, ctx->output_width, ctx->output_height);

    /* Flush cache for DMA */
    RK_MPI_MMZ_FlushCacheStart(g_display_mb, 0, y_size * 3 / 2, RK_MMZ_SYNC_WRITEONLY);
    RK_MPI_MMZ_FlushCacheEnd(g_display_mb, 0, y_size * 3 / 2, RK_MMZ_SYNC_WRITEONLY);

    /* Setup frame info for VENC */
    VIDEO_FRAME_INFO_S stFrame;
    memset(&stFrame, 0, sizeof(stFrame));
    stFrame.stVFrame.pMbBlk = g_display_mb;
    stFrame.stVFrame.u32Width = ctx->output_width;
    stFrame.stVFrame.u32Height = ctx->output_height;
    stFrame.stVFrame.u32VirWidth = ctx->output_width;
    stFrame.stVFrame.u32VirHeight = ctx->output_height;
    stFrame.stVFrame.enPixelFormat = RK_FMT_YUV420SP;
    stFrame.stVFrame.enCompressMode = COMPRESS_MODE_NONE;

    /* Send frame to VENC */
    RK_S32 ret = RK_MPI_VENC_SendFrame(VENC_CHN_DISPLAY, &stFrame, 1000);
    if (ret != RK_SUCCESS) {
        log_error("RK_MPI_VENC_SendFrame failed: 0x%x\n", ret);
        return 0;
    }

    /* Get encoded JPEG stream */
    VENC_STREAM_S stStream;
    memset(&stStream, 0, sizeof(stStream));
    stStream.pstPack = malloc(sizeof(VENC_PACK_S));
    if (!stStream.pstPack) {
        log_error("Failed to allocate stream pack\n");
        return 0;
    }

    ret = RK_MPI_VENC_GetStream(VENC_CHN_DISPLAY, &stStream, 1000);
    if (ret != RK_SUCCESS) {
        log_error("RK_MPI_VENC_GetStream failed: 0x%x\n", ret);
        free(stStream.pstPack);
        return 0;
    }

    /* Copy JPEG data to output buffer */
    size_t jpeg_size = 0;
    if (stStream.u32PackCount > 0 && stStream.pstPack) {
        void *pData = RK_MPI_MB_Handle2VirAddr(stStream.pstPack->pMbBlk);
        jpeg_size = stStream.pstPack->u32Len;
        if (pData && jpeg_size > 0) {
            if (jpeg_size > jpeg_buf_size) {
                jpeg_size = jpeg_buf_size;
            }
            memcpy(jpeg_buf, pData, jpeg_size);
        }
    }

    /* Release stream */
    RK_MPI_VENC_ReleaseStream(VENC_CHN_DISPLAY, &stStream);
    free(stStream.pstPack);

    return jpeg_size;
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

    log_info("Display capture started at %d fps (hardware VENC)\n", fps);
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
