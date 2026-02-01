/*
 * Display Framebuffer Capture Implementation
 * Uses RGA for hardware color conversion/rotation + VENC for JPEG encoding
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

/* RGA headers for hardware color conversion/rotation */
#include "librga/im2d.h"
#include "librga/rga.h"

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

/* DMA buffers for RGA and VENC */
static MB_BLK g_display_src_mb = MB_INVALID_HANDLE;  /* Source BGRX buffer */
static MB_BLK g_display_rot_mb = MB_INVALID_HANDLE;  /* Rotated BGRX buffer */
static MB_BLK g_display_dst_mb = MB_INVALID_HANDLE;  /* Destination NV12 buffer */
static void *g_display_src_vaddr = NULL;
static void *g_display_rot_vaddr = NULL;
static void *g_display_dst_vaddr = NULL;

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

    /* api.cfg is JSON format, look for "modelId": "20025" */
    while (fgets(line, sizeof(line), f)) {
        char *pos = strstr(line, "\"modelId\"");
        if (pos) {
            /* Find the colon and then the value */
            pos = strchr(pos, ':');
            if (pos) {
                pos++;
                /* Skip whitespace and find opening quote */
                while (*pos == ' ' || *pos == '\t') pos++;
                if (*pos == '"') {
                    pos++;
                    /* Copy model ID until closing quote */
                    int i = 0;
                    while (*pos && *pos != '"' && i < (int)sizeof(model_id) - 1) {
                        model_id[i++] = *pos++;
                    }
                    model_id[i] = '\0';
                    break;
                }
            }
        }
    }
    fclose(f);

    if (model_id[0] == '\0') {
        log_info("Model ID not found, using default orientation\n");
        return DISPLAY_ORIENT_NORMAL;
    }

    log_info("Detected model ID: %s\n", model_id);

    /* Map model ID to orientation
     * KS1/KS1M: 180 degree flip needed
     * K3M: 270 degree rotation needed
     * K3/K2P/K3V2: 90 degree rotation needed
     */
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
 * Rotate BGRX pixels 180 degrees using NEON SIMD
 * Processes 4 pixels at a time for better performance
 */
#include <arm_neon.h>

static void rotate_bgrx_180(const uint32_t *src, uint32_t *dst, int width, int height) {
    int total = width * height;
    int i = 0;
    int j = total - 4;

    /* Process 4 pixels at a time with NEON */
    while (i <= total - 4 && j >= 0) {
        /* Load 4 pixels */
        uint32x4_t pixels = vld1q_u32(&src[i]);
        /* Reverse the 4 pixels */
        uint32x4_t reversed = vrev64q_u32(pixels);
        reversed = vcombine_u32(vget_high_u32(reversed), vget_low_u32(reversed));
        /* Store at reversed position */
        vst1q_u32(&dst[j], reversed);
        i += 4;
        j -= 4;
    }

    /* Handle remaining pixels */
    while (i < total) {
        dst[total - 1 - i] = src[i];
        i++;
    }
}

static void rotate_bgrx_90(const uint32_t *src, uint32_t *dst, int width, int height) {
    /* 90 CW: output is height x width */
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            dst[x * height + (height - 1 - y)] = src[y * width + x];
        }
    }
}

static void rotate_bgrx_270(const uint32_t *src, uint32_t *dst, int width, int height) {
    /* 270 CW: output is height x width */
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            dst[(width - 1 - x) * height + y] = src[y * width + x];
        }
    }
}

/*
 * Convert BGRX to NV12 using RGA hardware acceleration (color conversion only)
 */
static int rga_convert_bgrx_to_nv12(void *src_bgrx, void *dst_nv12,
                                     int width, int height) {
    rga_buffer_t src_buf, dst_buf;
    IM_STATUS status;

    /* Wrap source buffer (BGRX) */
    src_buf = wrapbuffer_virtualaddr(src_bgrx, width, height, RK_FORMAT_BGRX_8888);
    if (src_buf.width == 0) {
        log_error("Failed to wrap source buffer\n");
        return -1;
    }

    /* Wrap destination buffer (NV12) */
    dst_buf = wrapbuffer_virtualaddr(dst_nv12, width, height, RK_FORMAT_YCbCr_420_SP);
    if (dst_buf.width == 0) {
        log_error("Failed to wrap destination buffer\n");
        return -1;
    }

    /* RGA color conversion only */
    status = imcvtcolor(src_buf, dst_buf, RK_FORMAT_BGRX_8888, RK_FORMAT_YCbCr_420_SP,
                       IM_RGB_TO_YUV_BT601_LIMIT, 1);

    if (status != IM_STATUS_SUCCESS) {
        log_error("RGA color conversion failed: %s\n", imStrError(status));
        return -1;
    }

    return 0;
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

    /* Allocate DMA buffer for source BGRX (copy from framebuffer) */
    size_t src_size = ctx->fb_size;
    RK_S32 ret = RK_MPI_MMZ_Alloc(&g_display_src_mb, src_size, RK_MMZ_ALLOC_CACHEABLE);
    if (ret != RK_SUCCESS || g_display_src_mb == MB_INVALID_HANDLE) {
        log_error("RK_MPI_MMZ_Alloc(src) failed: 0x%x\n", ret);
        munmap(ctx->fb_pixels, ctx->fb_size);
        close(ctx->fb_fd);
        return -1;
    }
    g_display_src_vaddr = RK_MPI_MMZ_Handle2VirAddr(g_display_src_mb);
    log_info("Allocated source DMA buffer: %zu bytes\n", src_size);

    /* Allocate DMA buffer for rotation (CPU rotation output, RGA input) */
    ret = RK_MPI_MMZ_Alloc(&g_display_rot_mb, src_size, RK_MMZ_ALLOC_CACHEABLE);
    if (ret != RK_SUCCESS || g_display_rot_mb == MB_INVALID_HANDLE) {
        log_error("RK_MPI_MMZ_Alloc(rot) failed: 0x%x\n", ret);
        RK_MPI_MMZ_Free(g_display_src_mb);
        g_display_src_mb = MB_INVALID_HANDLE;
        munmap(ctx->fb_pixels, ctx->fb_size);
        close(ctx->fb_fd);
        return -1;
    }
    g_display_rot_vaddr = RK_MPI_MMZ_Handle2VirAddr(g_display_rot_mb);
    log_info("Allocated rotation DMA buffer: %zu bytes\n", src_size);

    /* Allocate DMA buffer for destination NV12 */
    size_t nv12_size = ctx->output_width * ctx->output_height * 3 / 2;
    ret = RK_MPI_MMZ_Alloc(&g_display_dst_mb, nv12_size, RK_MMZ_ALLOC_CACHEABLE);
    if (ret != RK_SUCCESS || g_display_dst_mb == MB_INVALID_HANDLE) {
        log_error("RK_MPI_MMZ_Alloc(dst) failed: 0x%x\n", ret);
        RK_MPI_MMZ_Free(g_display_rot_mb);
        g_display_rot_mb = MB_INVALID_HANDLE;
        RK_MPI_MMZ_Free(g_display_src_mb);
        g_display_src_mb = MB_INVALID_HANDLE;
        munmap(ctx->fb_pixels, ctx->fb_size);
        close(ctx->fb_fd);
        return -1;
    }
    g_display_dst_vaddr = RK_MPI_MMZ_Handle2VirAddr(g_display_dst_mb);
    log_info("Allocated destination DMA buffer: %zu bytes\n", nv12_size);

    /* Initialize VENC for display JPEG encoding */
    if (init_display_venc(ctx->output_width, ctx->output_height, ctx->fps, DISPLAY_JPEG_QUALITY) != 0) {
        RK_MPI_MMZ_Free(g_display_dst_mb);
        RK_MPI_MMZ_Free(g_display_rot_mb);
        RK_MPI_MMZ_Free(g_display_src_mb);
        g_display_dst_mb = MB_INVALID_HANDLE;
        g_display_rot_mb = MB_INVALID_HANDLE;
        g_display_src_mb = MB_INVALID_HANDLE;
        munmap(ctx->fb_pixels, ctx->fb_size);
        close(ctx->fb_fd);
        return -1;
    }

    ctx->running = 1;
    log_info("Display capture initialized (CPU rotation + RGA color conversion)\n");
    return 0;
}

void display_capture_cleanup(DisplayCapture *ctx) {
    ctx->running = 0;

    /* Cleanup VENC */
    cleanup_display_venc();

    /* Free DMA buffers */
    if (g_display_dst_mb != MB_INVALID_HANDLE) {
        RK_MPI_MMZ_Free(g_display_dst_mb);
        g_display_dst_mb = MB_INVALID_HANDLE;
        g_display_dst_vaddr = NULL;
    }

    if (g_display_rot_mb != MB_INVALID_HANDLE) {
        RK_MPI_MMZ_Free(g_display_rot_mb);
        g_display_rot_mb = MB_INVALID_HANDLE;
        g_display_rot_vaddr = NULL;
    }

    if (g_display_src_mb != MB_INVALID_HANDLE) {
        RK_MPI_MMZ_Free(g_display_src_mb);
        g_display_src_mb = MB_INVALID_HANDLE;
        g_display_src_vaddr = NULL;
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
    if (!ctx->running || !ctx->fb_pixels || !g_display_src_vaddr || !g_display_rot_vaddr || !g_display_dst_vaddr) {
        return 0;
    }

    void *rga_src;
    int rga_width, rga_height;

    /* Copy framebuffer to cached DMA buffer first (fb mmap is slow/uncached) */
    memcpy(g_display_src_vaddr, ctx->fb_pixels, ctx->fb_size);

    /* Apply CPU rotation if needed, reading from cached source buffer */
    if (ctx->orientation == DISPLAY_ORIENT_FLIP_180) {
        rotate_bgrx_180((uint32_t *)g_display_src_vaddr, (uint32_t *)g_display_rot_vaddr, ctx->fb_width, ctx->fb_height);
        rga_src = g_display_rot_vaddr;
        rga_width = ctx->fb_width;
        rga_height = ctx->fb_height;
        /* Flush rotation buffer cache so RGA can see it */
        RK_MPI_MMZ_FlushCacheStart(g_display_rot_mb, 0, ctx->fb_size, RK_MMZ_SYNC_WRITEONLY);
        RK_MPI_MMZ_FlushCacheEnd(g_display_rot_mb, 0, ctx->fb_size, RK_MMZ_SYNC_WRITEONLY);
    } else if (ctx->orientation == DISPLAY_ORIENT_ROTATE_90) {
        rotate_bgrx_90((uint32_t *)g_display_src_vaddr, (uint32_t *)g_display_rot_vaddr, ctx->fb_width, ctx->fb_height);
        rga_src = g_display_rot_vaddr;
        rga_width = ctx->fb_height;  /* Swapped */
        rga_height = ctx->fb_width;
        /* Flush rotation buffer cache so RGA can see it */
        RK_MPI_MMZ_FlushCacheStart(g_display_rot_mb, 0, ctx->fb_size, RK_MMZ_SYNC_WRITEONLY);
        RK_MPI_MMZ_FlushCacheEnd(g_display_rot_mb, 0, ctx->fb_size, RK_MMZ_SYNC_WRITEONLY);
    } else if (ctx->orientation == DISPLAY_ORIENT_ROTATE_270) {
        rotate_bgrx_270((uint32_t *)g_display_src_vaddr, (uint32_t *)g_display_rot_vaddr, ctx->fb_width, ctx->fb_height);
        rga_src = g_display_rot_vaddr;
        rga_width = ctx->fb_height;  /* Swapped */
        rga_height = ctx->fb_width;
        /* Flush rotation buffer cache so RGA can see it */
        RK_MPI_MMZ_FlushCacheStart(g_display_rot_mb, 0, ctx->fb_size, RK_MMZ_SYNC_WRITEONLY);
        RK_MPI_MMZ_FlushCacheEnd(g_display_rot_mb, 0, ctx->fb_size, RK_MMZ_SYNC_WRITEONLY);
    } else {
        /* No rotation needed, use source buffer directly */
        rga_src = g_display_src_vaddr;
        rga_width = ctx->fb_width;
        rga_height = ctx->fb_height;
        /* Flush source buffer cache so RGA can see it */
        RK_MPI_MMZ_FlushCacheStart(g_display_src_mb, 0, ctx->fb_size, RK_MMZ_SYNC_WRITEONLY);
        RK_MPI_MMZ_FlushCacheEnd(g_display_src_mb, 0, ctx->fb_size, RK_MMZ_SYNC_WRITEONLY);
    }

    /* RGA color conversion (BGRX to NV12) */
    if (rga_convert_bgrx_to_nv12(rga_src, g_display_dst_vaddr, rga_width, rga_height) != 0) {
        log_error("RGA conversion failed\n");
        return 0;
    }

    /* Flush destination cache for VENC */
    size_t nv12_size = ctx->output_width * ctx->output_height * 3 / 2;
    RK_MPI_MMZ_FlushCacheStart(g_display_dst_mb, 0, nv12_size, RK_MMZ_SYNC_WRITEONLY);
    RK_MPI_MMZ_FlushCacheEnd(g_display_dst_mb, 0, nv12_size, RK_MMZ_SYNC_WRITEONLY);

    /* Setup frame info for VENC */
    VIDEO_FRAME_INFO_S stFrame;
    memset(&stFrame, 0, sizeof(stFrame));
    stFrame.stVFrame.pMbBlk = g_display_dst_mb;
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

    log_info("Display capture started at %d fps (RGA + VENC hardware)\n", fps);
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
