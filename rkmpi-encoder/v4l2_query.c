/*
 * Simple V4L2 camera format query tool
 * Lists supported formats, resolutions, and framerates
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/videodev2.h>

static const char *pixfmt_to_string(unsigned int pixfmt)
{
    static char buf[8];
    buf[0] = pixfmt & 0xFF;
    buf[1] = (pixfmt >> 8) & 0xFF;
    buf[2] = (pixfmt >> 16) & 0xFF;
    buf[3] = (pixfmt >> 24) & 0xFF;
    buf[4] = 0;
    return buf;
}

int main(int argc, char *argv[])
{
    const char *device = "/dev/video10";
    int fd;
    struct v4l2_capability cap;
    struct v4l2_fmtdesc fmtdesc;
    struct v4l2_frmsizeenum frmsize;
    struct v4l2_frmivalenum frmival;

    if (argc > 1)
        device = argv[1];

    fd = open(device, O_RDWR);
    if (fd < 0) {
        perror("Failed to open device");
        return 1;
    }

    /* Query capabilities */
    if (ioctl(fd, VIDIOC_QUERYCAP, &cap) == 0) {
        printf("Device: %s\n", cap.card);
        printf("Driver: %s\n", cap.driver);
        printf("Bus: %s\n", cap.bus_info);
        printf("Capabilities: 0x%08x\n\n", cap.capabilities);
    }

    /* Enumerate formats */
    printf("Supported formats:\n");
    memset(&fmtdesc, 0, sizeof(fmtdesc));
    fmtdesc.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

    while (ioctl(fd, VIDIOC_ENUM_FMT, &fmtdesc) == 0) {
        printf("\n  Format: %s (%s)\n", fmtdesc.description,
               pixfmt_to_string(fmtdesc.pixelformat));

        /* Enumerate frame sizes for this format */
        memset(&frmsize, 0, sizeof(frmsize));
        frmsize.pixel_format = fmtdesc.pixelformat;

        while (ioctl(fd, VIDIOC_ENUM_FRAMESIZES, &frmsize) == 0) {
            if (frmsize.type == V4L2_FRMSIZE_TYPE_DISCRETE) {
                printf("    %ux%u", frmsize.discrete.width,
                       frmsize.discrete.height);

                /* Enumerate frame intervals (framerates) */
                memset(&frmival, 0, sizeof(frmival));
                frmival.pixel_format = fmtdesc.pixelformat;
                frmival.width = frmsize.discrete.width;
                frmival.height = frmsize.discrete.height;

                printf(" @ ");
                while (ioctl(fd, VIDIOC_ENUM_FRAMEINTERVALS, &frmival) == 0) {
                    if (frmival.type == V4L2_FRMIVAL_TYPE_DISCRETE) {
                        double fps = (double)frmival.discrete.denominator /
                                    frmival.discrete.numerator;
                        printf("%.1ffps ", fps);
                    }
                    frmival.index++;
                }
                printf("\n");
            } else if (frmsize.type == V4L2_FRMSIZE_TYPE_STEPWISE) {
                printf("    %u-%u x %u-%u (step %u x %u)\n",
                       frmsize.stepwise.min_width, frmsize.stepwise.max_width,
                       frmsize.stepwise.min_height, frmsize.stepwise.max_height,
                       frmsize.stepwise.step_width, frmsize.stepwise.step_height);
            }
            frmsize.index++;
        }

        fmtdesc.index++;
    }

    /* Show current format */
    struct v4l2_format fmt;
    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(fd, VIDIOC_G_FMT, &fmt) == 0) {
        printf("\nCurrent format: %ux%u %s\n",
               fmt.fmt.pix.width, fmt.fmt.pix.height,
               pixfmt_to_string(fmt.fmt.pix.pixelformat));
    }

    close(fd);
    return 0;
}
