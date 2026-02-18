/*
 * Frame Buffer Management for Multi-threaded Streaming
 *
 * Provides thread-safe double-buffered frame storage for JPEG and H.264 data.
 * Server threads can wait efficiently for new frames using condition variables.
 */

#ifndef FRAME_BUFFER_H
#define FRAME_BUFFER_H

#include <stdint.h>
#include <stddef.h>
#include <pthread.h>

/* Maximum frame sizes */
#define FRAME_BUFFER_MAX_JPEG    (512 * 1024)   /* 512KB for camera JPEG */
#define FRAME_BUFFER_MAX_H264    (256 * 1024)   /* 256KB for H.264 */
#define FRAME_BUFFER_MAX_DISPLAY (512 * 1024)   /* 512KB for display JPEG */

/* Frame data structure */
typedef struct {
    uint8_t *data;          /* Frame data */
    size_t size;            /* Current data size */
    size_t capacity;        /* Buffer capacity */
    uint64_t timestamp;     /* Frame timestamp (microseconds) */
    uint64_t sequence;      /* Frame sequence number */
    int is_keyframe;        /* For H.264: 1 if IDR frame */
} FrameData;

/* Double-buffered frame storage */
typedef struct {
    FrameData frames[2];    /* Double buffer */
    int write_idx;          /* Current write buffer index */
    int read_idx;           /* Current read buffer index */
    uint64_t frame_count;   /* Total frames written */
    pthread_mutex_t mutex;
    pthread_cond_t cond;
} FrameBuffer;

/* Global frame buffers */
extern FrameBuffer g_jpeg_buffer;
extern FrameBuffer g_h264_buffer;
extern FrameBuffer g_display_buffer;

/* Initialization and cleanup */
int frame_buffer_init(FrameBuffer *fb, size_t capacity);
void frame_buffer_cleanup(FrameBuffer *fb);
int frame_buffers_init(void);
void frame_buffers_cleanup(void);

/* Producer API (encoder thread) */
int frame_buffer_write(FrameBuffer *fb, const uint8_t *data, size_t size,
                       uint64_t timestamp, int is_keyframe);

/* Consumer API (server threads) */
/* Wait for new frame, returns 0 on success, -1 on timeout */
int frame_buffer_wait(FrameBuffer *fb, uint64_t last_sequence, int timeout_ms);

/* Get current frame data (caller must hold lock or call between wait/release) */
const FrameData *frame_buffer_get_current(FrameBuffer *fb);

/* Copy frame data to user buffer, returns bytes copied
 * sequence_out: frame sequence number (optional)
 * timestamp_out: frame timestamp in microseconds (optional)
 * is_keyframe_out: 1 if H.264 keyframe (optional)
 */
size_t frame_buffer_copy(FrameBuffer *fb, uint8_t *dst, size_t dst_size,
                         uint64_t *sequence_out, uint64_t *timestamp_out,
                         int *is_keyframe_out);

/* Zero-copy frame access: returns pointer to internal buffer data.
 * Pointer is valid for at least one frame period (~50ms at 20fps).
 * Returns NULL if no frame available. */
const uint8_t *frame_buffer_ref(FrameBuffer *fb, size_t *size_out,
                                 uint64_t *sequence_out);

/* Get current sequence number without waiting */
uint64_t frame_buffer_get_sequence(FrameBuffer *fb);

/* Broadcast wake-up to all waiting threads (for shutdown) */
void frame_buffer_broadcast(FrameBuffer *fb);

#endif /* FRAME_BUFFER_H */
