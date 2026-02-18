/*
 * USB Camera Detection
 *
 * Enumerates USB cameras via /dev/v4l/by-path, resolves device paths,
 * detects supported formats, resolutions, and frame rates via V4L2 ioctls.
 */

#include "camera_detect.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <linux/videodev2.h>

/* V4L2 directories */
#define V4L2_BY_PATH_DIR  "/dev/v4l/by-path"
#define V4L2_BY_ID_DIR    "/dev/v4l/by-id"

/* Safe string copy: copies src into dst of size dst_size, always null-terminates. */
static inline void safe_strcpy(char *dst, size_t dst_size, const char *src) {
    if (dst_size == 0) return;
    size_t len = strlen(src);
    if (len >= dst_size) len = dst_size - 1;
    memcpy(dst, src, len);
    dst[len] = '\0';
}

/* Multi-camera port allocation: CAM#1=8080, CAM#2=8082, CAM#3=8083, CAM#4=8084 */
static int camera_port_for_id(int camera_id) {
    if (camera_id == 1) return 8080;
    return 8080 + camera_id;  /* 2→8082, 3→8083, 4→8084 */
}

/*
 * Parse USB port from by-path entry name.
 * Example: "platform-xhci_0-usb-0:1.3:1.0-video-index0" → "1.3"
 * Returns: 1 on success (port filled), 0 on failure
 */
static int parse_usb_port(const char *by_path_name, char *port, size_t port_size) {
    /* Look for "usb-0:" prefix pattern */
    const char *usb = strstr(by_path_name, "usb-0:");
    if (!usb) {
        /* Try alternative pattern "usb-" followed by digits */
        usb = strstr(by_path_name, "usb-");
        if (!usb) return 0;
        usb += 4;  /* skip "usb-" */
        /* Find the ':' after bus number */
        const char *colon = strchr(usb, ':');
        if (!colon) return 0;
        usb = colon + 1;
    } else {
        usb += 6;  /* skip "usb-0:" */
    }

    /* Copy port until next ':' or '-' */
    size_t i = 0;
    while (usb[i] && usb[i] != ':' && usb[i] != '-' && i < port_size - 1) {
        port[i] = usb[i];
        i++;
    }
    port[i] = '\0';
    return i > 0;
}

/*
 * Check if USB port matches the internal camera port.
 * Handles both exact match ("1.3" == "1.3") and suffix match
 * (e.g., "1-1.3" ends with "1.3").
 */
static int usb_port_matches(const char *detected_port, const char *internal_port) {
    if (!detected_port[0] || !internal_port[0]) return 0;

    /* Exact match */
    if (strcmp(detected_port, internal_port) == 0) return 1;

    /* Suffix match (internal_port at end of detected_port, after a '.') */
    size_t dlen = strlen(detected_port);
    size_t ilen = strlen(internal_port);
    if (dlen > ilen && detected_port[dlen - ilen - 1] == '.' &&
        strcmp(detected_port + dlen - ilen, internal_port) == 0) {
        return 1;
    }

    /* Also check by-path name contains the port */
    return 0;
}

int camera_detect_formats(const char *device, int *has_mjpeg, int *has_yuyv) {
    int fd = open(device, O_RDWR);
    if (fd < 0) return -1;

    *has_mjpeg = 0;
    *has_yuyv = 0;

    struct v4l2_fmtdesc fmtdesc;
    memset(&fmtdesc, 0, sizeof(fmtdesc));
    fmtdesc.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

    while (ioctl(fd, VIDIOC_ENUM_FMT, &fmtdesc) == 0) {
        if (fmtdesc.pixelformat == V4L2_PIX_FMT_MJPEG)
            *has_mjpeg = 1;
        else if (fmtdesc.pixelformat == V4L2_PIX_FMT_YUYV)
            *has_yuyv = 1;
        fmtdesc.index++;
    }

    close(fd);
    return 0;
}

int camera_detect_resolution(const char *device, int *width, int *height) {
    int fd = open(device, O_RDWR);
    if (fd < 0) return -1;

    *width = 0;
    *height = 0;

    /* Try MJPEG first, then YUYV */
    unsigned int formats[] = { V4L2_PIX_FMT_MJPEG, V4L2_PIX_FMT_YUYV };
    int nformats = 2;

    for (int f = 0; f < nformats; f++) {
        struct v4l2_frmsizeenum frmsize;
        memset(&frmsize, 0, sizeof(frmsize));
        frmsize.pixel_format = formats[f];

        int best_pixels = 0;
        while (ioctl(fd, VIDIOC_ENUM_FRAMESIZES, &frmsize) == 0) {
            if (frmsize.type == V4L2_FRMSIZE_TYPE_DISCRETE) {
                int pixels = frmsize.discrete.width * frmsize.discrete.height;
                if (pixels > best_pixels) {
                    best_pixels = pixels;
                    *width = frmsize.discrete.width;
                    *height = frmsize.discrete.height;
                }
            }
            frmsize.index++;
        }

        if (best_pixels > 0) {
            close(fd);
            return 0;
        }
    }

    /* Fallback: query current format */
    struct v4l2_format fmt;
    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(fd, VIDIOC_G_FMT, &fmt) == 0) {
        *width = fmt.fmt.pix.width;
        *height = fmt.fmt.pix.height;
        close(fd);
        return 0;
    }

    close(fd);
    return -1;
}

int camera_detect_all_resolutions(const char *device,
                                   CameraResolution *out, int max_res,
                                   int has_mjpeg) {
    int fd = open(device, O_RDWR);
    if (fd < 0) return 0;

    int count = 0;

    /* Query resolutions for the preferred format (MJPEG if available, else YUYV) */
    unsigned int fmt = has_mjpeg ? V4L2_PIX_FMT_MJPEG : V4L2_PIX_FMT_YUYV;
    struct v4l2_frmsizeenum frmsize;
    memset(&frmsize, 0, sizeof(frmsize));
    frmsize.pixel_format = fmt;

    while (ioctl(fd, VIDIOC_ENUM_FRAMESIZES, &frmsize) == 0 && count < max_res) {
        if (frmsize.type == V4L2_FRMSIZE_TYPE_DISCRETE) {
            out[count].width = frmsize.discrete.width;
            out[count].height = frmsize.discrete.height;
            count++;
        }
        frmsize.index++;
    }

    /* If MJPEG had no results and camera also supports YUYV, try YUYV */
    if (count == 0 && has_mjpeg) {
        memset(&frmsize, 0, sizeof(frmsize));
        frmsize.pixel_format = V4L2_PIX_FMT_YUYV;
        while (ioctl(fd, VIDIOC_ENUM_FRAMESIZES, &frmsize) == 0 && count < max_res) {
            if (frmsize.type == V4L2_FRMSIZE_TYPE_DISCRETE) {
                out[count].width = frmsize.discrete.width;
                out[count].height = frmsize.discrete.height;
                count++;
            }
            frmsize.index++;
        }
    }

    close(fd);

    /* Sort by pixel count descending (largest first) */
    for (int i = 0; i < count - 1; i++) {
        for (int j = i + 1; j < count; j++) {
            int pi = out[i].width * out[i].height;
            int pj = out[j].width * out[j].height;
            if (pj > pi) {
                CameraResolution tmp = out[i];
                out[i] = out[j];
                out[j] = tmp;
            }
        }
    }

    return count;
}

int camera_detect_max_fps(const char *device, int width, int height) {
    int fd = open(device, O_RDWR);
    if (fd < 0) return 0;

    int max_fps = 0;

    /* Try MJPEG first, then YUYV */
    unsigned int formats[] = { V4L2_PIX_FMT_MJPEG, V4L2_PIX_FMT_YUYV };
    int nformats = 2;

    for (int f = 0; f < nformats; f++) {
        struct v4l2_frmivalenum frmival;
        memset(&frmival, 0, sizeof(frmival));
        frmival.pixel_format = formats[f];
        frmival.width = width;
        frmival.height = height;

        while (ioctl(fd, VIDIOC_ENUM_FRAMEINTERVALS, &frmival) == 0) {
            if (frmival.type == V4L2_FRMIVAL_TYPE_DISCRETE) {
                int fps = 0;
                if (frmival.discrete.numerator > 0) {
                    fps = frmival.discrete.denominator / frmival.discrete.numerator;
                }
                if (fps > max_fps) max_fps = fps;
            }
            frmival.index++;
        }

        if (max_fps > 0) break;
    }

    close(fd);
    return max_fps;
}

/*
 * Query camera name from VIDIOC_QUERYCAP.
 */
static void query_camera_name(const char *device, char *name, size_t name_size) {
    int fd = open(device, O_RDWR);
    if (fd < 0) {
        strncpy(name, "Unknown Camera", name_size - 1);
        return;
    }

    struct v4l2_capability cap;
    memset(&cap, 0, sizeof(cap));
    if (ioctl(fd, VIDIOC_QUERYCAP, &cap) == 0) {
        strncpy(name, (const char *)cap.card, name_size - 1);
        name[name_size - 1] = '\0';
    } else {
        strncpy(name, "USB Camera", name_size - 1);
    }

    close(fd);
}

/*
 * Look up the /dev/v4l/by-id/ entry name for a given /dev/videoN device.
 * This provides a stable, human-readable unique ID like:
 *   "usb-SunplusIT_Inc_Integrated_Camera-video-index0"
 * Falls back to empty string if by-id directory doesn't exist.
 */
static void lookup_by_id(const char *device, char *unique_id, size_t id_size) {
    unique_id[0] = '\0';

    DIR *dir = opendir(V4L2_BY_ID_DIR);
    if (!dir) return;

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') continue;
        /* Only index0 entries (one per camera) */
        if (!strstr(entry->d_name, "-video-index0")) continue;

        char symlink_path[512];
        snprintf(symlink_path, sizeof(symlink_path), "%s/%s",
                 V4L2_BY_ID_DIR, entry->d_name);

        char real_path[64];
        if (!realpath(symlink_path, real_path)) continue;

        if (strcmp(real_path, device) == 0) {
            safe_strcpy(unique_id, id_size, entry->d_name);
            break;
        }
    }

    closedir(dir);
}

int camera_detect_all(CameraInfo *cameras, int max_cameras,
                      const char *internal_usb_port) {
    if (max_cameras > CAMERA_MAX) max_cameras = CAMERA_MAX;
    memset(cameras, 0, sizeof(CameraInfo) * max_cameras);

    DIR *dir = opendir(V4L2_BY_PATH_DIR);
    if (!dir) {
        fprintf(stderr, "CamDetect: Cannot open %s\n", V4L2_BY_PATH_DIR);
        return 0;
    }

    int count = 0;
    struct dirent *entry;

    while ((entry = readdir(dir)) != NULL && count < max_cameras) {
        /* Only look at USB video-index0 entries (skip ISP, metadata devices) */
        if (!strstr(entry->d_name, "video-index0"))
            continue;
        if (!strstr(entry->d_name, "usb"))
            continue;

        /* Resolve symlink to /dev/videoN */
        char symlink_path[512];
        snprintf(symlink_path, sizeof(symlink_path), "%s/%s",
                 V4L2_BY_PATH_DIR, entry->d_name);

        char real_path[64];
        if (!realpath(symlink_path, real_path)) continue;

        /* Skip if not a /dev/video device */
        if (strncmp(real_path, "/dev/video", 10) != 0) continue;

        /* Check if device is accessible */
        if (access(real_path, R_OK | W_OK) != 0) continue;

        CameraInfo *cam = &cameras[count];

        /* Device path */
        safe_strcpy(cam->device, sizeof(cam->device), real_path);

        /* By-path entry name */
        safe_strcpy(cam->by_path, sizeof(cam->by_path), entry->d_name);

        /* Generate unique_id from /dev/v4l/by-id/ (matches Python behavior),
         * fall back to by-path name if by-id not available */
        lookup_by_id(real_path, cam->unique_id, sizeof(cam->unique_id));
        if (!cam->unique_id[0]) {
            safe_strcpy(cam->unique_id, sizeof(cam->unique_id), entry->d_name);
        }

        /* Parse USB port */
        parse_usb_port(entry->d_name, cam->usb_port, sizeof(cam->usb_port));

        /* Check if this is the primary (internal) camera */
        if (internal_usb_port && internal_usb_port[0]) {
            cam->is_primary = usb_port_matches(cam->usb_port, internal_usb_port);
        }

        /* Query camera name */
        query_camera_name(cam->device, cam->name, sizeof(cam->name));

        /* Detect formats */
        camera_detect_formats(cam->device, &cam->has_mjpeg, &cam->has_yuyv);

        /* Detect all supported resolutions */
        cam->num_resolutions = camera_detect_all_resolutions(
            cam->device, cam->resolutions, CAMERA_MAX_RESOLUTIONS,
            cam->has_mjpeg);

        /* Detect resolution */
        camera_detect_resolution(cam->device, &cam->width, &cam->height);

        /* Detect max FPS */
        if (cam->width > 0 && cam->height > 0) {
            cam->max_fps = camera_detect_max_fps(cam->device,
                                                  cam->width, cam->height);
        }

        count++;
    }

    closedir(dir);

    /* Sort: primary camera first, then by device path */
    for (int i = 0; i < count - 1; i++) {
        for (int j = i + 1; j < count; j++) {
            int swap = 0;
            if (cameras[j].is_primary && !cameras[i].is_primary) {
                swap = 1;
            } else if (cameras[i].is_primary == cameras[j].is_primary) {
                if (strcmp(cameras[i].device, cameras[j].device) > 0)
                    swap = 1;
            }
            if (swap) {
                CameraInfo tmp = cameras[i];
                cameras[i] = cameras[j];
                cameras[j] = tmp;
            }
        }
    }

    /* Assign camera IDs, ports, and default enable state */
    for (int i = 0; i < count; i++) {
        cameras[i].camera_id = i + 1;
        cameras[i].streaming_port = camera_port_for_id(i + 1);
        cameras[i].enabled = (i == 0);  /* Only primary enabled by default */
    }

    if (count > 0) {
        fprintf(stderr, "CamDetect: Found %d camera(s):\n", count);
        for (int i = 0; i < count; i++) {
            fprintf(stderr, "  CAM#%d: %s (%s) %dx%d@%dfps USB=%s %s%s\n",
                    cameras[i].camera_id,
                    cameras[i].device,
                    cameras[i].name,
                    cameras[i].width, cameras[i].height,
                    cameras[i].max_fps,
                    cameras[i].usb_port,
                    cameras[i].is_primary ? "[PRIMARY]" : "",
                    cameras[i].has_mjpeg ? " MJPEG" : "");
        }
    } else {
        fprintf(stderr, "CamDetect: No cameras found\n");
    }

    return count;
}
