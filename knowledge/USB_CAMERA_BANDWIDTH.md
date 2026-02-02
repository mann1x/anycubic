# USB Camera Bandwidth Investigation - RV1106

## Summary

Investigation into USB bandwidth optimization for V4L2 camera capture on RV1106 ARM platform (Anycubic printers).

## Key Findings

### USB 2.0 Physical Limits

| Transfer Mode | Max Bandwidth | Notes |
|---------------|---------------|-------|
| Isochronous | 24 MB/s | Hard limit for USB 2.0 High-Speed |
| Bulk | Higher potential | Not guaranteed, cameras use isochronous |

### Camera Data Rates

| Format | Frame Size | Max FPS @ 720p | Notes |
|--------|------------|----------------|-------|
| YUYV raw | 1.8 MB | ~13 fps | Limited by USB bandwidth |
| MJPEG | 50-150 KB | 30+ fps | Compression enables high FPS |

**YUYV mode is limited to ~4fps** because of USB isochronous bandwidth ceiling (24 MB/s).

## V4L2 Access Methods

| Method | Status | Reason |
|--------|--------|--------|
| **MMAP** | Optimal | Zero-copy kernel→userspace, well supported |
| USERPTR | Not supported | ARM dma_contig allocator requires contiguous memory |
| DMABUF | Broken on RV1106 | Non-standard CMA implementation, VIDIOC_EXPBUF fails |

### Why DMABUF Doesn't Work

According to Luckfox Forums, on RV1103/RV1106 platforms, memory allocation for the camera uses Rockchip's MMZ which differs from standard Linux CMA. VIDIOC_EXPBUF fails, making zero-copy from USB camera to VENC impossible.

## Bypassing V4L2

| Approach | Verdict | Reason |
|----------|---------|--------|
| libuvc | Not recommended | Kernel UVC driver is mature and optimized |
| libusb direct | Not recommended | Adds complexity without performance benefit |

## Recommended Optimizations

### 1. Increase usbfs Memory Pool

The `usbfs_memory_mb` parameter limits total USB transfer buffer memory.

```bash
# Check current value
cat /sys/module/usbcore/parameters/usbfs_memory_mb

# Increase to 32MB (default is often 8-16MB)
echo 32 > /sys/module/usbcore/parameters/usbfs_memory_mb
```

**Note:** This is a maximum limit, not pre-allocation. Memory is only used when USB transfers request it.

### 2. Disable USB Autosuspend for Camera

```bash
# Find camera device
lsusb -t

# Disable autosuspend (replace X-Y with device path)
echo 'on' > /sys/bus/usb/devices/X-Y/power/control
```

### 3. UVC Quirks

Apply FIX_BANDWIDTH quirk if camera misreports bandwidth:

```bash
echo 0x80 > /sys/module/uvcvideo/parameters/quirks
```

### 4. V4L2 Buffer Count

Increase buffer count for smoother frame delivery:

```c
#define V4L2_BUFFER_COUNT  5  // Default was 4
```

## Diagnostic Commands

```bash
# USB device info
lsusb -t
cat /sys/bus/usb/devices/*/speed        # 480 = High-Speed

# USB core parameters
cat /sys/module/usbcore/parameters/usbfs_memory_mb
cat /sys/module/usbcore/parameters/autosuspend

# UVC parameters
cat /sys/module/uvcvideo/parameters/quirks
cat /sys/module/uvcvideo/parameters/trace

# Enable UVC bandwidth tracing
echo 0x400 > /sys/module/uvcvideo/parameters/trace
dmesg | grep uvcvideo

# Camera power settings
cat /sys/bus/usb/devices/X-Y/power/control
cat /sys/bus/usb/devices/X-Y/power/autosuspend_delay_ms
```

## Test Results (KS1 Printer)

### System Configuration

| Parameter | Original | Optimized |
|-----------|----------|-----------|
| usbfs_memory_mb | 8 MB | 32 MB |
| V4L2 buffers | 4 | 5 |
| Camera autosuspend | auto | on |
| Camera | SunplusIT Integrated Camera | - |
| USB Speed | 480 Mbps (High-Speed) | - |

### CPU Usage (MJPEG Mode)

| Scenario | CPU Usage | Notes |
|----------|-----------|-------|
| Idle (no clients) | 0% | Pre-DQBUF sleep skips V4L2 entirely |
| MJPEG client only | 4-8% | Pass-through, no TurboJPEG decode |
| FLV client (H.264) | 35-50% | TurboJPEG decode is CPU-intensive |
| Connection spike | <15% | Mitigated with ramp-up throttling |

### CPU Usage (YUYV Mode)

| Scenario | CPU Usage | Notes |
|----------|-----------|-------|
| Idle (no clients) | 0% | Same idle logic |
| Active stream | 5-6% | Hardware JPEG encode, YUYV→NV12 only |

## Adaptive Frame Rate Detection

Cameras may advertise higher frame rates than they actually deliver (e.g., 30fps advertised but only 10fps actual due to USB bandwidth or camera limitations).

The encoder detects actual camera frame rate and adjusts accordingly:

```c
/* Measure inter-frame timing over 30 frames */
camera_interval = (camera_interval * 3 + measured_interval) / 4;  /* EMA */

/* After detection, only rate-limit if camera is faster than target */
rate_limit_needed = (camera_fps > target_fps + 2);
```

On KS1 printer with Integrated Camera:
- Advertised: 30fps MJPEG
- Actual: 9fps (100ms interval)
- Rate limiting: disabled (camera already at target)

## Connection Spike Mitigation

The CPU spike on client connection is mitigated with gradual ramp-up:

```c
/* Ramp-up phases: 25% → 50% → 75% → 100% over 4 seconds */
switch (ramp_phase) {
    case 0: process = (frame_counter % 4) == 1; break;  /* 25% */
    case 1: process = (frame_counter % 2) == 1; break;  /* 50% */
    case 2: process = (frame_counter % 4) != 0; break;  /* 75% */
    default: process = 1; break;                        /* 100% */
}
```

## References

- [Luckfox Forums - Dmabuf mode for v4l2](https://forums.luckfox.com/viewtopic.php?t=924)
- [Linux Kernel - UVC Driver](https://www.kernel.org/doc/html/v5.0/media/v4l-drivers/uvcvideo.html)
- [Linux Kernel - VIDIOC_EXPBUF](https://www.kernel.org/doc/html/v5.4/media/uapi/v4l/vidioc-expbuf.html)
- [Teledyne Vision - USBFS on Linux](https://www.teledynevisionsolutions.com/support/support-center/application-note/iis/understanding-usbfs-on-linux/)

## Diagnostic Script

See `rkmpi-encoder/scripts/usb_diag.sh` for automated diagnostics.
