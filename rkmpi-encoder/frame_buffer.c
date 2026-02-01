/*
 * Frame Buffer Management Implementation
 */

#include "frame_buffer.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>

/* Global frame buffers */
FrameBuffer g_jpeg_buffer;
FrameBuffer g_h264_buffer;
FrameBuffer g_display_buffer;

static uint64_t get_timestamp_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000 + (uint64_t)ts.tv_nsec / 1000;
}

int frame_buffer_init(FrameBuffer *fb, size_t capacity) {
    memset(fb, 0, sizeof(*fb));

    /* Initialize mutex and condition variable */
    pthread_mutexattr_t mutex_attr;
    pthread_mutexattr_init(&mutex_attr);
    pthread_mutex_init(&fb->mutex, &mutex_attr);
    pthread_mutexattr_destroy(&mutex_attr);

    pthread_condattr_t cond_attr;
    pthread_condattr_init(&cond_attr);
    pthread_condattr_setclock(&cond_attr, CLOCK_MONOTONIC);
    pthread_cond_init(&fb->cond, &cond_attr);
    pthread_condattr_destroy(&cond_attr);

    /* Allocate double buffers */
    for (int i = 0; i < 2; i++) {
        fb->frames[i].data = malloc(capacity);
        if (!fb->frames[i].data) {
            /* Cleanup on failure */
            for (int j = 0; j < i; j++) {
                free(fb->frames[j].data);
            }
            pthread_mutex_destroy(&fb->mutex);
            pthread_cond_destroy(&fb->cond);
            return -1;
        }
        fb->frames[i].capacity = capacity;
        fb->frames[i].size = 0;
        fb->frames[i].timestamp = 0;
        fb->frames[i].sequence = 0;
        fb->frames[i].is_keyframe = 0;
    }

    fb->write_idx = 0;
    fb->read_idx = 0;
    fb->frame_count = 0;

    return 0;
}

void frame_buffer_cleanup(FrameBuffer *fb) {
    pthread_mutex_lock(&fb->mutex);

    for (int i = 0; i < 2; i++) {
        if (fb->frames[i].data) {
            free(fb->frames[i].data);
            fb->frames[i].data = NULL;
        }
    }

    pthread_mutex_unlock(&fb->mutex);
    pthread_cond_destroy(&fb->cond);
    pthread_mutex_destroy(&fb->mutex);
}

int frame_buffers_init(void) {
    if (frame_buffer_init(&g_jpeg_buffer, FRAME_BUFFER_MAX_JPEG) != 0) {
        return -1;
    }
    if (frame_buffer_init(&g_h264_buffer, FRAME_BUFFER_MAX_H264) != 0) {
        frame_buffer_cleanup(&g_jpeg_buffer);
        return -1;
    }
    if (frame_buffer_init(&g_display_buffer, FRAME_BUFFER_MAX_DISPLAY) != 0) {
        frame_buffer_cleanup(&g_h264_buffer);
        frame_buffer_cleanup(&g_jpeg_buffer);
        return -1;
    }
    return 0;
}

void frame_buffers_cleanup(void) {
    frame_buffer_cleanup(&g_display_buffer);
    frame_buffer_cleanup(&g_jpeg_buffer);
    frame_buffer_cleanup(&g_h264_buffer);
}

int frame_buffer_write(FrameBuffer *fb, const uint8_t *data, size_t size,
                       uint64_t timestamp, int is_keyframe) {
    if (!data || size == 0) {
        return -1;
    }

    pthread_mutex_lock(&fb->mutex);

    /* Write to current write buffer */
    FrameData *frame = &fb->frames[fb->write_idx];

    if (size > frame->capacity) {
        /* Frame too large, truncate (shouldn't happen with proper capacity) */
        size = frame->capacity;
    }

    memcpy(frame->data, data, size);
    frame->size = size;
    frame->timestamp = timestamp ? timestamp : get_timestamp_us();
    frame->is_keyframe = is_keyframe;

    /* Update sequence and swap buffers */
    fb->frame_count++;
    frame->sequence = fb->frame_count;

    /* Swap read/write indices */
    fb->read_idx = fb->write_idx;
    fb->write_idx = (fb->write_idx + 1) % 2;

    /* Wake up all waiting threads */
    pthread_cond_broadcast(&fb->cond);

    pthread_mutex_unlock(&fb->mutex);

    return 0;
}

int frame_buffer_wait(FrameBuffer *fb, uint64_t last_sequence, int timeout_ms) {
    struct timespec ts;
    int ret = 0;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    ts.tv_sec += timeout_ms / 1000;
    ts.tv_nsec += (timeout_ms % 1000) * 1000000;
    if (ts.tv_nsec >= 1000000000) {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000;
    }

    pthread_mutex_lock(&fb->mutex);

    /* Wait until we have a newer frame */
    while (fb->frame_count <= last_sequence) {
        ret = pthread_cond_timedwait(&fb->cond, &fb->mutex, &ts);
        if (ret == ETIMEDOUT) {
            pthread_mutex_unlock(&fb->mutex);
            return -1;
        }
    }

    pthread_mutex_unlock(&fb->mutex);
    return 0;
}

const FrameData *frame_buffer_get_current(FrameBuffer *fb) {
    /* Caller should hold mutex or use between wait/release */
    return &fb->frames[fb->read_idx];
}

size_t frame_buffer_copy(FrameBuffer *fb, uint8_t *dst, size_t dst_size,
                         uint64_t *sequence_out, int *is_keyframe_out) {
    size_t copied = 0;

    pthread_mutex_lock(&fb->mutex);

    const FrameData *frame = &fb->frames[fb->read_idx];

    if (frame->size > 0 && dst && dst_size > 0) {
        copied = (frame->size < dst_size) ? frame->size : dst_size;
        memcpy(dst, frame->data, copied);

        if (sequence_out) {
            *sequence_out = frame->sequence;
        }
        if (is_keyframe_out) {
            *is_keyframe_out = frame->is_keyframe;
        }
    }

    pthread_mutex_unlock(&fb->mutex);

    return copied;
}

uint64_t frame_buffer_get_sequence(FrameBuffer *fb) {
    uint64_t seq;
    pthread_mutex_lock(&fb->mutex);
    seq = fb->frame_count;
    pthread_mutex_unlock(&fb->mutex);
    return seq;
}

void frame_buffer_broadcast(FrameBuffer *fb) {
    pthread_mutex_lock(&fb->mutex);
    pthread_cond_broadcast(&fb->cond);
    pthread_mutex_unlock(&fb->mutex);
}
