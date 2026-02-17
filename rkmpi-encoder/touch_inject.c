/*
 * Touch Event Injection
 *
 * Injects Linux input events via /dev/input/event0 for LCD touch.
 * Uses MT Protocol B (multi-touch slots).
 * Transforms web display coordinates to touch panel coordinates
 * based on the printer's display orientation.
 */

#include "touch_inject.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <errno.h>
#include <linux/input.h>

#define TOUCH_DEVICE "/dev/input/event0"

/* Framebuffer native resolution */
#define FB_WIDTH  800
#define FB_HEIGHT 480

/* Model IDs */
#define MODEL_ID_K2P   "20021"
#define MODEL_ID_K3    "20024"
#define MODEL_ID_KS1   "20025"
#define MODEL_ID_K3M   "20026"
#define MODEL_ID_K3V2  "20027"
#define MODEL_ID_KS1M  "20029"

/* Orientation enum (matches display_capture.h) */
#define ORIENT_NORMAL   0
#define ORIENT_FLIP_180 1
#define ORIENT_ROTATE_90  2
#define ORIENT_ROTATE_270 3

/* Detect display orientation from api.cfg */
static int detect_orientation(void) {
    FILE *f = fopen("/userdata/app/gk/config/api.cfg", "r");
    if (!f) return ORIENT_NORMAL;

    char line[256];
    char model_id[32] = {0};

    while (fgets(line, sizeof(line), f)) {
        char *pos = strstr(line, "\"modelId\"");
        if (pos) {
            pos = strchr(pos + 9, '"');
            if (pos) {
                pos++; /* skip opening quote */
                char *end = strchr(pos, '"');
                if (end) {
                    size_t len = end - pos;
                    if (len < sizeof(model_id)) {
                        memcpy(model_id, pos, len);
                        model_id[len] = '\0';
                    }
                }
            }
            break;
        }
    }
    fclose(f);

    if (strcmp(model_id, MODEL_ID_KS1) == 0 || strcmp(model_id, MODEL_ID_KS1M) == 0) {
        return ORIENT_FLIP_180;
    } else if (strcmp(model_id, MODEL_ID_K3M) == 0) {
        return ORIENT_ROTATE_270;
    } else if (strcmp(model_id, MODEL_ID_K3) == 0 ||
               strcmp(model_id, MODEL_ID_K2P) == 0 ||
               strcmp(model_id, MODEL_ID_K3V2) == 0) {
        return ORIENT_ROTATE_90;
    }
    return ORIENT_NORMAL;
}

/* Transform web display coordinates to touch panel coordinates */
static void transform_coordinates(int web_x, int web_y, int orient,
                                   int *touch_x, int *touch_y) {
    switch (orient) {
    case ORIENT_FLIP_180:
        *touch_x = FB_WIDTH - web_x;
        *touch_y = FB_HEIGHT - web_y;
        break;
    case ORIENT_ROTATE_90:
        *touch_x = web_y;
        *touch_y = FB_HEIGHT - web_x;
        break;
    case ORIENT_ROTATE_270:
        *touch_x = FB_WIDTH - web_y;
        *touch_y = web_x;
        break;
    default:
        *touch_x = web_x;
        *touch_y = web_y;
        break;
    }
}

/* Write a single input_event */
static int emit_event(int fd, __u16 type, __u16 code, __s32 value) {
    struct input_event ev;
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ev.time.tv_sec = ts.tv_sec;
    ev.time.tv_usec = ts.tv_nsec / 1000;
    ev.type = type;
    ev.code = code;
    ev.value = value;

    ssize_t n = write(fd, &ev, sizeof(ev));
    return (n == sizeof(ev)) ? 0 : -1;
}

int touch_inject(int x, int y, int duration_ms) {
    if (duration_ms <= 0) duration_ms = 50;

    /* Transform coordinates based on display orientation */
    int orient = detect_orientation();
    int touch_x, touch_y;
    transform_coordinates(x, y, orient, &touch_x, &touch_y);

    fprintf(stderr, "Touch: web(%d,%d) -> panel(%d,%d) orient=%d duration=%dms\n",
            x, y, touch_x, touch_y, orient, duration_ms);

    int fd = open(TOUCH_DEVICE, O_WRONLY);
    if (fd < 0) {
        fprintf(stderr, "Touch: Cannot open %s: %s\n", TOUCH_DEVICE, strerror(errno));
        return -1;
    }

    /* Touch down - MT Protocol B */
    emit_event(fd, EV_ABS, ABS_MT_SLOT, 0);
    emit_event(fd, EV_ABS, ABS_MT_TRACKING_ID, 1);
    emit_event(fd, EV_ABS, ABS_MT_POSITION_X, touch_x);
    emit_event(fd, EV_ABS, ABS_MT_POSITION_Y, touch_y);
    emit_event(fd, EV_ABS, ABS_MT_TOUCH_MAJOR, 50);
    emit_event(fd, EV_ABS, ABS_MT_PRESSURE, 100);
    emit_event(fd, EV_KEY, BTN_TOUCH, 1);
    emit_event(fd, EV_SYN, SYN_REPORT, 0);

    /* Hold */
    usleep(duration_ms * 1000);

    /* Touch up */
    emit_event(fd, EV_ABS, ABS_MT_TRACKING_ID, -1);
    emit_event(fd, EV_KEY, BTN_TOUCH, 0);
    emit_event(fd, EV_SYN, SYN_REPORT, 0);

    close(fd);
    return 0;
}
