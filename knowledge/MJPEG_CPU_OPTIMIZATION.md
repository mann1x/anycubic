# MJPEG Mode CPU Optimization - RV1106

## Summary

Investigation into reducing CPU usage for MJPEG camera capture mode on RV1106 ARM platform. Target: ≤5% CPU for MJPEG streaming.

## Architecture Overview

```
USB Camera (MJPEG 720p)
       │
       ▼
   V4L2 DQBUF ──────────────────────────────┐
       │                                     │
       ▼                                     │
   MJPEG Pass-through ──► HTTP :8080         │ No decode needed
       │                   (MJPEG clients)   │
       │                                     │
       ▼                                     │
   TurboJPEG Decode ──► NV12 ──► VENC ──► HTTP :18088
   (CPU-intensive)              (H.264)     (FLV clients)
```

## Key Insight: Camera Frame Rate Detection

Cameras often advertise higher frame rates than they deliver:

| Advertised | Actual | Reason |
|------------|--------|--------|
| 30 fps | 9-10 fps | USB bandwidth limits, camera hardware |

### Adaptive Detection Implementation

```c
/* Measure inter-frame timing using exponential moving average */
if (g_mjpeg_ctrl.camera_interval == 0) {
    g_mjpeg_ctrl.camera_interval = interval;
} else {
    g_mjpeg_ctrl.camera_interval = (g_mjpeg_ctrl.camera_interval * 3 + interval) / 4;
}

/* After 30 frames, finalize detection */
if (captured_count >= 30) {
    int camera_fps = 1000000 / g_mjpeg_ctrl.camera_interval;
    g_mjpeg_ctrl.rate_limit_needed = (camera_fps > target_fps + 2);
}
```

**Result on KS1:** Camera delivers 9fps, target is 10fps → rate limiting disabled.

## Optimization #1: Pre-DQBUF Sleep

### Problem
Original code called `VIDIOC_DQBUF` for every frame (30/sec advertised), then decided whether to process. This caused unnecessary syscall overhead.

### Solution
Move rate control BEFORE the DQBUF call:

```c
/* BEFORE DQBUF - only if rate limiting needed */
if (g_mjpeg_ctrl.rate_limit_needed) {
    RK_U64 now = get_timestamp_us();
    RK_U64 next_frame_time = last_output_time + target_interval;
    if (now < next_frame_time) {
        usleep(next_frame_time - now);  /* Sleep without touching V4L2 */
    }
}

/* Now call DQBUF */
ioctl(v4l2_fd, VIDIOC_DQBUF, &buf);
```

**Result:** Syscall rate reduced from 30/sec to 10/sec when camera is faster than target.

## Optimization #2: Client-Based Idle

### Problem
Encoder was processing frames even with no clients connected.

### Solution
Skip all processing when no clients:

```c
if (cfg.server_mode && !cfg.mjpeg_stdout && total_clients == 0) {
    usleep(500000);  /* 500ms sleep when fully idle */
    continue;        /* Skip DQBUF entirely */
}
```

**Result:** 0% CPU when idle.

## Optimization #3: Skip TurboJPEG When No FLV Clients

### Problem
TurboJPEG decode (~25ms/frame) ran even when only MJPEG clients were connected.

### Solution
Only decode when FLV clients need H.264:

```c
if (cfg.server_mode) {
    int flv_clients = flv_server_client_count();
    if (flv_clients > 0) {
        do_h264 = g_ctrl.h264_enabled && ...;
    }
    /* else: no FLV clients, skip decode entirely */
}
```

**Result:** MJPEG-only streaming uses 4-8% CPU (pass-through, no decode).

## Optimization #4: Connection Ramp-Up

### Problem
Sudden client connection caused CPU spikes from 0% to 40%+.

### Solution
Gradual frame processing ramp-up over 4 seconds:

```c
/* Ramp phases: 25% → 50% → 75% → 100% */
switch (ramp_phase) {
    case 0: process = (frame_counter % 4) == 1; break;  /* 25% */
    case 1: process = (frame_counter % 2) == 1; break;  /* 50% */
    case 2: process = (frame_counter % 4) != 0; break;  /* 75% */
    default: process = 1; break;                        /* 100% */
}
```

**Result:** Connection spikes reduced to <15%.

## CPU Usage Results

### MJPEG Mode (Camera MJPEG → Pass-through)

| Scenario | CPU | Notes |
|----------|-----|-------|
| Idle (no clients) | 0% | Pre-DQBUF sleep skips V4L2 |
| MJPEG client only | 4-8% | Pass-through, no decode |
| FLV client (H.264) | 35-50% | TurboJPEG decode required |
| Connection spike | <15% | Mitigated by ramp-up |

### With H.264 Disabled (h264=0)

| Scenario | CPU | Notes |
|----------|-----|-------|
| Idle | 0% | Same as enabled |
| MJPEG client | 0-8% | Same as enabled |
| FLV client | 0% | No video sent, just FLV header |

### YUYV Mode (Camera YUYV → HW JPEG)

| Scenario | CPU | Notes |
|----------|-----|-------|
| Idle | 0% | Same idle logic |
| Active stream | 5-6% | YUYV→NV12 conversion only |

## Configuration Options

### Runtime Control File (`/tmp/h264_ctrl`)

```
h264=0          # Disable H.264 encoding entirely
h264=1          # Enable H.264 encoding (default)
skip=2          # Encode every 2nd frame (reduces CPU)
auto_skip=1     # Auto-adjust skip based on CPU load
target_cpu=60   # Target CPU % for auto-skip
```

### Command Line

```bash
# MJPEG-only server (lowest CPU)
rkmpi_enc --server -N --mode vanilla-klipper

# With H.264 disabled at startup
rkmpi_enc --server -N -n  # -n = --no-h264
```

## Diagnostic Commands

```bash
# Check camera actual frame rate (look for "Camera rate detected")
grep "Camera rate" /tmp/rkmpi.log

# Monitor CPU usage
top -b -n 5 | grep rkmpi_enc

# Check client connections
cat /tmp/h264_ctrl | grep clients

# Check H.264 status
cat /tmp/h264_ctrl | grep h264
```

## Key Takeaways

1. **MJPEG pass-through is cheap** - Just memory copy, no decode needed
2. **TurboJPEG decode is expensive** - ~25ms/frame, 35-50% CPU at 10fps
3. **Client-based idle is essential** - Don't process when no one is watching
4. **Adaptive rate detection** - Don't assume camera delivers advertised fps
5. **Pre-DQBUF sleep** - Don't touch V4L2 until frame is needed

## Files Modified

- `rkmpi-encoder/rkmpi_enc.c` - Main encoder with all optimizations
- `rkmpi-encoder/http_server.c` - Client connection tracking
- `rkmpi-encoder/http_server.h` - Warmup constants

## Related Documentation

- `USB_CAMERA_BANDWIDTH.md` - USB bandwidth investigation
- `rkmpi-encoder/claude.md` - Encoder component documentation
