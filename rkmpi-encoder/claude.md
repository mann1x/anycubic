# rkmpi_enc - RV1106 Hardware H.264 Encoder

USB camera capture with hardware H.264 encoding for RV1106-based Anycubic printers.

## RV1106 Hardware Specifications

### CPU
- **Core:** Single-core ARM Cortex-A7 @ 1.2GHz
- **Architecture:** ARMv7-A with NEON SIMD
- **FPU:** VFPv4 hardware floating point
- **Cache:** 32KB L1 I-cache, 32KB L1 D-cache, 256KB L2 cache

### Memory
- **RAM:** 256MB DDR3 (shared with system)
- **Available:** ~100-150MB after system/Klipper overhead
- **Bandwidth:** Limited, shared between CPU, NPU, and multimedia

### Video Engine (VENC/VDEC)
- **Encoder:** H.264/H.265 hardware encoder up to 2304x1296 @ 30fps
- **Decoder:** H.264/H.265/MJPEG hardware decoder
- **Note:** MJPEG decode via VDEC requires specific buffer setup; TurboJPEG software decode is simpler and sufficient for 720p

### RGA (2D Graphics Accelerator)
- **Capabilities:** Scaling, rotation, color space conversion, compositing
- **Max resolution:** 8192x8192
- **Useful for:** NV12/RGB conversion, frame scaling

### NPU (Neural Processing Unit)
- **Performance:** 0.5 TOPS INT8
- **Not used** by this encoder

### USB
- **Controller:** USB 2.0 OTG
- **Bandwidth:** 480 Mbps theoretical, ~35-40 MB/s practical
- **Limitation:** Single USB controller shared between host and device mode

### Practical Limits for Video Encoding
- **720p @ 15fps:** Comfortable, low CPU usage (~15-25%)
- **720p @ 30fps:** Works but higher CPU for JPEG decode (~35-45%)
- **1080p @ 15fps:** Possible but stresses memory bandwidth
- **1080p @ 30fps:** Not recommended, may cause frame drops

## USB Camera Capabilities & Limitations

### Supported Camera Types
- UVC (USB Video Class) compliant cameras
- Must support **MJPEG output format** (raw YUV too bandwidth-heavy)
- Typical cameras: Logitech C270/C920, generic "HD Webcam" models

### Camera Output Formats
| Format | Bandwidth @ 720p30 | CPU Load | Recommended |
|--------|-------------------|----------|-------------|
| MJPEG | ~3-8 MB/s | Medium (decode) | **Yes** |
| YUYV | ~27 MB/s | Low | No (exceeds USB) |
| H.264 | ~1-2 MB/s | None | Camera-dependent |

### Resolution Support
- **Preferred:** 1280x720 (720p) - best balance of quality and performance
- **Supported:** 640x480, 800x600, 1280x720, 1920x1080
- **Default fallback:** 1280x720 if detection fails

### Framerate Considerations
- Camera advertised FPS may not be achievable under USB bandwidth constraints
- MJPEG quality/compression affects actual bandwidth
- Recommended: 10-15 fps for monitoring, 20-30 fps for timelapse

### Known Camera Issues
- Some cameras report capabilities they can't sustain
- Cheap cameras may have unstable USB connections
- Illumination affects MJPEG compression ratio (dark = larger frames)

### Camera Detection
```bash
# List V4L2 devices
ls -la /dev/v4l/by-id/

# Example output:
# usb-SunplusIT_Inc_HD_Camera-video-index0 -> ../../video0

# Check camera capabilities (if v4l2-ctl available)
v4l2-ctl -d /dev/video0 --list-formats-ext
```

## Why MJPEG → Software Decode → Hardware H.264?

The RV1106 has hardware MJPEG decode, but:
1. **Buffer complexity:** VDEC requires specific DMA buffer management
2. **Format mismatch:** VDEC outputs to internal buffers, VENC expects specific input
3. **Simplicity:** TurboJPEG decode to CPU memory, then copy to VENC input buffer
4. **Performance:** At 720p15, software JPEG decode uses only ~15% CPU
5. **Reliability:** Avoids complex Rockit pipeline configuration

For higher resolutions/framerates, a full hardware pipeline (VDEC→RGA→VENC) would be needed.

## Overview

This encoder captures MJPEG frames from a USB camera, decodes them using TurboJPEG, and re-encodes to H.264 using the RV1106's hardware VENC. It also supports capturing the printer's LCD framebuffer for remote display viewing. Output is served via built-in HTTP servers.

## Architecture

```
USB Camera (V4L2 MJPEG)
    ↓
TurboJPEG decode (software)
    ↓
NV12 frame buffer
    ↓
RKMPI VENC Ch0 (hardware H.264) → FLV HTTP server (:18088)
RKMPI VENC Ch1 (hardware MJPEG) → MJPEG HTTP server (:8080)

Display Capture (optional, --display flag):
    Framebuffer (/dev/fb0) → memcpy → RGA rotation → RGA (BGRX→NV12) → VENC Ch2 (MJPEG)
                                                                            ↓
                                                              /display endpoint (:8080)
```

## Build System

### Prerequisites
- RV1106 cross-compilation toolchain: `/shared/dev/rv1106-toolchain`
- Toolchain prefix: `arm-rockchip830-linux-uclibcgnueabihf-`

### SDK Libraries (in lib-printer/)
From printer's `/oem/usr/lib/`:
- librockit_full.so - Rockit multimedia framework
- librockchip_mpp.so - Media Process Platform
- librga.so - 2D graphics acceleration
- libdrm.so - Direct Rendering Manager
- libturbojpeg.so - JPEG decode/encode

### Build Commands

```bash
# Build with dynamic linking (default, ~22KB binary)
make dynamic

# Build with static linking (~5MB+ binary, standalone)
make static

# Clean
make clean

# Deploy to printer for testing
make deploy PRINTER_IP=192.168.x.x

# Copy to h264-streamer build directory
make install-h264
```

### Compiler Flags
- `-march=armv7-a -mfpu=neon -mfloat-abi=hard` - ARMv7 with NEON
- `-fno-stack-protector` - Disabled (not in printer's uClibc)
- `-O2` - Optimization level

## Usage

```bash
# Show help
./rkmpi_enc --help

# Run with verbose output
./rkmpi_enc -v

# Custom resolution and framerate
./rkmpi_enc -w 1280 -h 720 -f 15

# Specify camera device
./rkmpi_enc -d /dev/video0

# Enable auto-skip based on CPU usage (target 60% max)
./rkmpi_enc -a -t 60

# Manual skip ratio (skip every other frame)
./rkmpi_enc -s 2
```

## Command-Line Options

| Option | Description |
|--------|-------------|
| `-d` | Camera device path (default: auto-detect) |
| `-w` | Width (default: 1280) |
| `-h` | Height (default: 720) |
| `-f` | Framerate (default: 15) |
| `-s` | Skip ratio (default: 1 = no skip) |
| `-a` | Enable auto-skip based on CPU usage |
| `-t` | Target CPU % for auto-skip (default: 60) |
| `-n` | Disable H.264 encoding (MJPEG only) |
| `-v` | Verbose output |
| `-S` | Server mode (built-in HTTP servers) |
| `-N` | No camera (server mode without camera capture) |
| `--display` | Enable display capture (LCD framebuffer) |
| `--streaming-port` | HTTP server port (default: 8080) |

## CPU Optimizations

The encoder includes several CPU optimizations:

1. **Pre-allocated YUV buffer**: Avoids per-frame malloc/free overhead
2. **NEON SIMD UV interleaving**: Uses ARM NEON intrinsics for U/V plane interleaving
3. **RGA hardware rotation**: Uses RGA `imflip()`/`imrotate()` for display rotation (0% CPU)
4. **TurboJPEG fast flags**: Uses `TJFLAG_FASTDCT | TJFLAG_FASTUPSAMPLE` for faster decoding
5. **Auto-skip**: Dynamically adjusts skip ratio based on CPU usage
6. **DMA buffers**: Uses RK_MPI_MMZ for hardware-accessible memory
7. **On-demand encoding**: Display capture only encodes when clients are connected

## Display Capture

The `--display` flag enables capturing the printer's LCD framebuffer (/dev/fb0) for remote viewing.

### Pipeline (Full Hardware Acceleration)
```
Framebuffer (800x480 BGRX)
    ↓ memcpy to DMA buffer (fb mmap is slow/uncached)
    ↓ RGA rotation/flip (imflip or imrotate)
    ↓ RGA color conversion (BGRX → NV12)
    ↓ VENC Ch2 (MJPEG encoding)
    ↓
/display endpoint (1-10 fps, configurable)
```

### Features
- **On-demand encoding**: Only captures when clients connected AND enabled
- **Disabled by default**: Enable via control page to save CPU when not needed
- **Configurable FPS**: 1-10 fps selectable via control page
- **Full hardware pipeline**: RGA rotation + RGA color conversion + VENC encoding

### Screen Orientation
Auto-detected from `/userdata/app/gk/config/api.cfg` (JSON format with `modelId`):

| Model | Model ID | Orientation | RGA Operation |
|-------|----------|-------------|---------------|
| KS1, KS1M | 20025, 20029 | 180° flip | `imflip()` FLIP_H_V |
| K3M | 20026 | 270° rotation | `imrotate()` ROT_270 |
| K3, K2P, K3V2 | 20024, 20021, 20027 | 90° rotation | `imrotate()` ROT_90 |

### Endpoints
- `/display` - MJPEG multipart stream (configurable 1-10 fps)
- `/display/snapshot` - Single JPEG frame (on-demand capture)

### Performance
- **CPU usage**: ~7-13% when active at 10fps (full hardware pipeline with RGA DMA copy)
- **Per-frame timing** (measured):
  - RGA framebuffer copy: ~2ms (vs 20-50ms for CPU memcpy)
  - RGA rotation: ~2ms
  - RGA color conversion: ~1.5ms
  - VENC JPEG: ~1ms
  - Total: ~6-11ms per frame
- **Memory**: 3 DMA buffers (~4.5MB total)
  - Source BGRX: 800×480×4 = 1.5MB
  - Rotation BGRX: 800×480×4 = 1.5MB
  - NV12 output: 800×480×1.5 = 0.6MB

### Implementation Details
- RGA DMA copy from framebuffer (10-15x faster than CPU memcpy from uncached mmap)
- Two-step RGA pipeline: rotation/flip first, then color conversion
- DMA buffers allocated via `RK_MPI_MMZ_Alloc` for RGA/VENC access
- Cache flush required after CPU writes, before hardware reads
- Uses VENC channel 2 (separate from camera channels 0, 1)
- CPU fallback available if RGA copy or rotation fails
- Timing instrumentation available via `-DDISPLAY_TIMING` compile flag

## Output Pipes

The encoder creates and writes to:
- `/tmp/mjpeg.pipe` - Raw MJPEG frames (for snapshot/MJPEG stream)
- `/tmp/h264.pipe` - H.264 Annex-B NALUs (for FLV stream)

## Integration with h264-streamer

The h264_server.py spawns this encoder as a subprocess:

```python
cmd = [
    encoder_path,
    '-w', str(width),
    '-h', str(height),
    '-f', str(framerate),
    '-s', str(skip_ratio)
]
if not h264_enabled:
    cmd.append('--no-h264')
```

## Camera Detection

The encoder auto-detects USB cameras by scanning `/dev/v4l/by-id/` for symlinks. It's not tied to a specific camera model and works with any USB camera supporting MJPEG output.

## Operating Modes

The `--mode` flag controls firmware integration behavior.

### go-klipper (default)

For Anycubic printers running **Rinkhals custom firmware**.

**Features:**
- **MQTT Client** - Connects to printer's MQTT broker (port 9883) for camera commands
- **RPC Client** - Connects to local binary API (port 18086) for timelapse commands
- **Timelapse Recording** - Handles openDelayCamera, startLanCapture, stopLanCapture RPC commands
- **Full Firmware Integration** - Works alongside gkapi and other Anycubic services

**Use when:**
- Running on an Anycubic printer with Rinkhals
- You need timelapse recording support
- The camera needs to respond to firmware commands

### vanilla-klipper

For **external Klipper** setups or testing without Anycubic firmware.

**Features:**
- **No MQTT/RPC** - Skips all firmware communication
- **Pure Encoding** - Just captures from camera and serves HTTP streams
- **No Timelapse** - Timelapse commands are not processed

**Use when:**
- Using an external Raspberry Pi with Klipper
- Testing the encoder in isolation
- The printer's native firmware is not involved

### Mode Comparison

| Feature | go-klipper | vanilla-klipper |
|---------|------------|-----------------|
| MQTT client | ✅ | ❌ |
| RPC client | ✅ | ❌ |
| Timelapse recording | ✅ | ❌ |
| Works without firmware | ❌ | ✅ |

### MQTT Keepalive

The MQTT client sends PINGREQ packets every 45 seconds when idle to prevent broker disconnections. This is handled automatically in the MQTT client thread:
- Tracks `last_activity` timestamp for send/receive operations
- Sends PINGREQ (0xC0, 0x00) when no activity for 45 seconds
- Broker responds with PINGRESP (0xD0)
- Prevents "Connection closed by broker" errors

### Setting the Mode

```bash
# Default (go-klipper)
./rkmpi_enc -S -v

# External Klipper
./rkmpi_enc -S -v --mode vanilla-klipper
```

---

## Environment

On the printer, set LD_LIBRARY_PATH before running:
```bash
export LD_LIBRARY_PATH=/oem/usr/lib:$LD_LIBRARY_PATH
./rkmpi_enc -v
```

## File Locations

- Source: `rkmpi_enc.c` (this directory)
- Makefile: `Makefile`
- SDK headers: `include/`
- Printer libs: `lib-printer/`
- Built binary: `rkmpi_enc`
- h264-streamer copy: `../h264-streamer/29-h264-streamer/rkmpi_enc`

### Absolute Paths
- This directory: `/shared/dev/anycubic/rkmpi-encoder/`
- Cross-toolchain: `/shared/dev/rv1106-toolchain/`

## Timelapse Recording

The encoder includes built-in timelapse recording with hardware VENC H.264 encoding.

### Video Encoding Methods

**Hardware VENC (Default)**
- Uses RV1106 hardware H.264 encoder directly
- No external dependencies (no ffmpeg required)
- Frames encoded in real-time as they're captured
- MP4 muxing via minimp4 header-only library
- Works with all capture modes: rkmpi, rkmpi + H.264, rkmpi-yuyv
- Uses VENC channel 2 (separate from live stream on channel 0)

**FFmpeg Fallback**
- Used if VENC initialization fails
- Saves JPEG frames to disk, assembles at finalize
- Tries bundled static ffmpeg first, then stock ffmpeg with LD_LIBRARY_PATH
- Falls back to mpeg4 codec if libx264 fails (OOM)

### VENC Pipeline

```
JPEG frame (from frame buffer)
    │
    ▼
TurboJPEG decode → NV12
    │
    ▼
VENC Channel 2 (H.264, VBR 4Mbps)
    │
    ▼
minimp4 muxer → .mp4 file
```

### Timelapse Modes

**RPC Mode (Legacy)**
- Triggered by Anycubic firmware RPC commands
- `openDelayCamera` initializes timelapse with gcode filepath
- `startLanCapture` captures each frame
- Print completion detected via `print_stats.state` in RPC status

**Custom Mode (Advanced)**
- Triggered by h264_server.py via Moonraker integration
- Timelapse commands received via control file
- Ignores Anycubic RPC timelapse commands when active
- Supports configurable FPS, CRF, variable FPS, and flip options

### Control File Commands

The encoder reads commands from a control file (default: `/tmp/rkmpi.ctrl`):

| Command | Description |
|---------|-------------|
| `timelapse_init:<gcode>:<output_dir>` | Initialize custom timelapse |
| `timelapse_capture` | Capture single JPEG frame |
| `timelapse_finalize` | Assemble frames into MP4 |
| `timelapse_cancel` | Discard frames and cleanup |
| `timelapse_fps:<n>` | Set output FPS (1-60) |
| `timelapse_crf:<n>` | Set H.264 quality (0-51, ffmpeg only) |
| `timelapse_variable_fps:<0\|1>:<min>:<max>:<target>:<dup>` | Configure variable FPS |
| `timelapse_flip:<flip_x>:<flip_y>` | Set flip options (0 or 1) |
| `timelapse_output_dir:<path>` | Set output directory |
| `timelapse_custom_mode:<0\|1>` | Enable/disable custom mode |
| `timelapse_use_venc:<0\|1>` | Use hardware VENC (default: 1) |

### Timelapse Configuration (timelapse.h)

```c
typedef struct {
    int output_fps;           // Video playback FPS (default: 10)
    int crf;                  // H.264 quality 0-51 (default: 23, ffmpeg only)
    int variable_fps;         // Auto-calculate FPS (default: 0)
    int target_length;        // Target video length in seconds
    int variable_fps_min;     // Minimum FPS for variable mode
    int variable_fps_max;     // Maximum FPS for variable mode
    int duplicate_last_frame; // Duplicate count for last frame
    int flip_x;               // Horizontal flip (default: 0)
    int flip_y;               // Vertical flip (default: 0)
    char output_dir[256];     // Output directory path
} TimelapseConfig;

typedef struct {
    // ... other fields ...
    int use_venc;             // 1 = hardware VENC (default), 0 = ffmpeg
    int venc_initialized;     // 1 if VENC encoder is ready
    int frame_width;          // Frame width (auto-detected)
    int frame_height;         // Frame height (auto-detected)
} TimelapseState;
```

### Variable FPS Calculation

When `variable_fps` is enabled, output FPS is calculated as:
```
fps = max(min_fps, min(max_fps, frame_count / target_length))
```

This ensures short prints don't have overly long videos and long prints don't exceed frame rate limits.

### FFmpeg Fallback (when VENC disabled or fails)

The `timelapse_finalize()` function assembles frames using ffmpeg with memory-optimized settings:

**Primary encoder (libx264):**
```bash
ffmpeg -y -framerate <fps> -i frames/frame_%04d.jpg \
    -c:v libx264 -preset ultrafast -tune zerolatency \
    -x264-params keyint=30:min-keyint=10:scenecut=0:bframes=0:ref=1:rc-lookahead=0:threads=1 \
    -crf <crf> -pix_fmt yuv420p output.mp4
```

**Fallback encoder (mpeg4):**
If libx264 fails (e.g., OOM on low-memory systems), falls back to mpeg4:
```bash
ffmpeg -y -framerate <fps> -i frames/frame_%04d.jpg \
    -c:v mpeg4 -q:v 5 -pix_fmt yuv420p output.mp4
```

**Note:** mpeg4 videos may not play in browser preview but will work when downloaded.

Flip filters are applied based on configuration (ffmpeg mode only):
- `flip_x=1, flip_y=0` → `-vf hflip`
- `flip_x=0, flip_y=1` → `-vf vflip`
- `flip_x=1, flip_y=1` → `-vf hflip,vflip`

### Output Files

- **Video**: `{output_dir}/{gcode}_NN.mp4`
- **Thumbnail**: `{output_dir}/{gcode}_NN_{frames}.jpg` (last frame)
- **Temp frames** (ffmpeg mode only): `/tmp/timelapse_frames/frame_NNNN.jpg`

Default output directory: `/useremain/app/gk/Time-lapse-Video/`

### Implementation Files

- `timelapse.h` - Configuration struct and function prototypes
- `timelapse.c` - Frame capture, finalization, VENC/ffmpeg routing
- `timelapse_venc.h` - Hardware VENC encoding API
- `timelapse_venc.c` - VENC encoder with minimp4 muxer
- `minimp4.h` - Header-only MP4 muxer library (public domain)
- `rpc_client.c` - RPC command handlers (legacy mode)
- `rkmpi_enc.c` - Control file parsing for custom mode

---

## Troubleshooting

### "cannot open shared object file"
Ensure LD_LIBRARY_PATH includes `/oem/usr/lib`

### Camera busy
- Check if gkcam is running: `ps | grep gkcam`
- Kill it: use rinkhals `kill_by_name gkcam` (pkill not available)
- Ensure LAN mode is enabled via local binary API

### No video device
- Check USB camera connected: `ls /dev/video*`
- Check V4L2 devices: `ls /dev/v4l/by-id/`

---

## USB Camera V4L2 Controls

The encoder supports real-time control of USB camera settings via V4L2.

### Supported Controls

| Control | Range | Default | Notes |
|---------|-------|---------|-------|
| Brightness | 0-255 | 0 | |
| Contrast | 0-255 | 32 | |
| Saturation | 0-132 | 85 | |
| Hue | -180 to 180 | 0 | |
| Gamma | 90-150 | 100 | |
| Sharpness | 0-30 | 3 | |
| Gain | 0-1 | 1 | On/Off |
| Backlight Compensation | 0-7 | 0 | |
| White Balance Auto | 0-1 | 1 | |
| White Balance Temperature | 2800-6500 | 4000 | Only when auto=0 |
| Exposure Auto | 1,3 | 3 | 1=Manual, 3=Auto |
| Exposure Absolute | 10-2500 | 156 | Only when auto=1 |
| Exposure Auto Priority | 0-1 | 0 | Constant vs variable FPS |
| Power Line Frequency | 0-2 | 1 | 0=Off, 1=50Hz, 2=60Hz |

### Control File Interface

Camera controls are read from `/tmp/h264_ctrl`:

```
cam_brightness=0
cam_contrast=32
cam_saturation=85
cam_wb_auto=1
cam_exposure_auto=3
...
```

The encoder reads these values and applies them to the camera via `VIDIOC_S_CTRL`.

### JPEG Quality

USB cameras typically don't support V4L2 JPEG quality control. In YUYV mode, JPEG quality is controlled by the `--jpeg-quality` parameter (1-100, default 85) which sets the hardware VENC quality.

### JPEG Frame Validation

To prevent corrupt/white frames, the encoder validates JPEG output:
- Checks for valid 0xFFD8 header
- Requires minimum 100 byte size
- Logs and skips invalid frames: `[JPEG] Bad frame detected`

See also: `knowledge/USB_CAMERA_CONTROLS.md`
