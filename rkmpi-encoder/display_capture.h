/*
 * Display Framebuffer Capture for RV1106
 *
 * Captures the LCD framebuffer (/dev/fb0) and encodes to JPEG for streaming.
 * Uses RV1106 hardware VENC for efficient JPEG encoding.
 * Auto-detects printer model and applies correct screen orientation.
 *
 * Display specs (Anycubic printers):
 * - Resolution: 800x480
 * - Format: 32bpp BGRX
 * - Orientation varies by model
 *
 * Pipeline: Framebuffer -> Rotate -> BGRX to NV12 -> VENC MJPEG -> JPEG
 */

#ifndef DISPLAY_CAPTURE_H
#define DISPLAY_CAPTURE_H

#include <stdint.h>
#include <stddef.h>

/* Display resolution (fixed for Anycubic printers) */
#define DISPLAY_WIDTH   800
#define DISPLAY_HEIGHT  480
#define DISPLAY_BPP     4       /* 32bpp BGRX */

/* JPEG encoding quality for display capture (0-100) */
#define DISPLAY_JPEG_QUALITY  80

/* Default capture FPS (display updates are typically slow) */
#define DISPLAY_DEFAULT_FPS   5

/* Screen orientation modes (based on printer model) */
typedef enum {
    DISPLAY_ORIENT_NORMAL = 0,
    DISPLAY_ORIENT_FLIP_180 = 1,    /* KS1, KS1M */
    DISPLAY_ORIENT_ROTATE_90 = 2,   /* K3, K2P, K3V2 */
    DISPLAY_ORIENT_ROTATE_270 = 3   /* K3M */
} DisplayOrientation;

/* Display capture context */
typedef struct {
    int fb_fd;                      /* Framebuffer file descriptor */
    uint32_t *fb_pixels;            /* Mapped framebuffer memory */
    size_t fb_size;                 /* Mapped size */
    int fb_width;                   /* Actual framebuffer width */
    int fb_height;                  /* Actual framebuffer height */
    DisplayOrientation orientation; /* Detected orientation */
    int output_width;               /* Output width after rotation */
    int output_height;              /* Output height after rotation */
    int fps;                        /* Target capture FPS */
    volatile int running;           /* Thread control flag */
} DisplayCapture;

/* Initialize display capture (opens /dev/fb0, detects orientation) */
int display_capture_init(DisplayCapture *ctx, int fps);

/* Cleanup display capture */
void display_capture_cleanup(DisplayCapture *ctx);

/* Capture single frame to JPEG (thread-safe)
 * Returns JPEG size on success, 0 on failure
 * jpeg_buf must be at least DISPLAY_WIDTH * DISPLAY_HEIGHT * 3 bytes
 */
size_t display_capture_frame(DisplayCapture *ctx, uint8_t *jpeg_buf, size_t jpeg_buf_size);

/* Start display capture thread (writes to g_display_buffer) */
int display_capture_start(int fps);

/* Stop display capture thread */
void display_capture_stop(void);

/* Check if display capture is running */
int display_capture_is_running(void);

/* Get current orientation name (for logging) */
const char *display_orientation_name(DisplayOrientation orient);

#endif /* DISPLAY_CAPTURE_H */
