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
    Framebuffer (/dev/fb0) → memcpy → CPU rotation → RGA (BGRX→NV12) → VENC Ch2 (MJPEG)
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
3. **NEON SIMD rotation**: Uses NEON intrinsics for 180° display rotation
4. **TurboJPEG fast flags**: Uses `TJFLAG_FASTDCT | TJFLAG_FASTUPSAMPLE` for faster decoding
5. **Auto-skip**: Dynamically adjusts skip ratio based on CPU usage
6. **DMA buffers**: Uses RK_MPI_MMZ for hardware-accessible memory

## Display Capture

The `--display` flag enables capturing the printer's LCD framebuffer (/dev/fb0) for remote viewing.

### Pipeline
```
Framebuffer (800x480 BGRX)
    ↓ memcpy to DMA buffer (fb mmap is slow/uncached)
    ↓ CPU rotation (NEON SIMD for 180°)
    ↓ RGA color conversion (BGRX → NV12)
    ↓ VENC Ch2 (MJPEG encoding)
    ↓
/display endpoint (5 fps)
```

### Screen Orientation
Auto-detected from `/userdata/app/gk/config/api.cfg` (JSON format with `modelId`):

| Model | Model ID | Orientation |
|-------|----------|-------------|
| KS1, KS1M | 20025, 20029 | 180° flip |
| K3M | 20026 | 270° rotation |
| K3, K2P, K3V2 | 20024, 20021, 20027 | 90° rotation |

### Endpoints
- `/display` - MJPEG multipart stream (5 fps)
- `/display/snapshot` - Single JPEG frame

### Performance
- **CPU usage**: ~15% at 5 fps (800x480)
- **Memory**: 3 DMA buffers (~4.5MB total)
  - Source BGRX: 800×480×4 = 1.5MB
  - Rotation BGRX: 800×480×4 = 1.5MB
  - NV12 output: 800×480×1.5 = 0.6MB

### Implementation Details
- Rotation done in CPU (RGA combined rotation+color conversion unreliable)
- DMA buffers allocated via `RK_MPI_MMZ_Alloc` for RGA/VENC access
- Cache flush required after CPU writes, before hardware reads
- Uses VENC channel 2 (separate from camera channels 0, 1)

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
