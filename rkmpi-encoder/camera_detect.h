/*
 * USB Camera Detection
 *
 * Enumerates USB cameras via /dev/v4l/by-path, resolves device paths,
 * detects supported formats, resolutions, and frame rates via V4L2 ioctls.
 */

#ifndef CAMERA_DETECT_H
#define CAMERA_DETECT_H

#define CAMERA_MAX  4

typedef struct {
    char device[32];            /* "/dev/video10" */
    char by_path[128];          /* "/dev/v4l/by-path/..." entry name */
    char name[64];              /* "USB Camera" (from VIDIOC_QUERYCAP) */
    char unique_id[128];        /* "usb-xhci_0-1.3-video-index0" */
    char usb_port[32];          /* "1.3" (parsed from by-path name) */
    int width, height;          /* Native resolution (MJPEG preferred) */
    int max_fps;                /* Max FPS at native resolution */
    int has_mjpeg;              /* Camera supports MJPEG format */
    int has_yuyv;               /* Camera supports YUYV format */
    int is_primary;             /* Matched internal USB port */
    int camera_id;              /* 1-based ID assigned during detection */
    int enabled;                /* Whether this camera should be started */
    int streaming_port;         /* Port assigned (8080, 8082, 8083, 8084) */
} CameraInfo;

/*
 * Detect all USB cameras.
 * Scans /dev/v4l/by-path/ for *-video-index0 entries, resolves to /dev/videoN,
 * queries capabilities. Primary camera (matching internal_usb_port) is listed first.
 *
 * cameras: array of CameraInfo to fill (at least CAMERA_MAX entries)
 * max_cameras: maximum cameras to detect
 * internal_usb_port: USB port string for primary camera (e.g. "1.3")
 *
 * Returns: number of cameras detected (0 if none)
 */
int camera_detect_all(CameraInfo *cameras, int max_cameras,
                      const char *internal_usb_port);

/*
 * Detect native resolution for a camera device.
 * Tries MJPEG first, falls back to YUYV. Returns highest resolution.
 *
 * Returns: 0 on success, -1 on error
 */
int camera_detect_resolution(const char *device, int *width, int *height);

/*
 * Detect max FPS for a given resolution and format.
 * Uses VIDIOC_ENUM_FRAMEINTERVALS.
 *
 * Returns: max FPS (0 on error)
 */
int camera_detect_max_fps(const char *device, int width, int height);

/*
 * Detect supported pixel formats (MJPEG, YUYV).
 *
 * Returns: 0 on success, -1 on error
 */
int camera_detect_formats(const char *device, int *has_mjpeg, int *has_yuyv);

#endif /* CAMERA_DETECT_H */
