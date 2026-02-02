# rkmpi-encoder

Hardware H.264/JPEG encoder for RV1106-based Anycubic 3D printers.

## Features

- **USB Camera Capture** - V4L2 MJPEG or YUYV capture from USB cameras
- **Hardware H.264 Encoding** - RV1106 VENC for efficient video encoding
- **Hardware MJPEG Encoding** - For snapshots and MJPEG streaming
- **Display Capture** - Stream the printer's LCD via RGA hardware rotation
- **Built-in Servers** - HTTP, MQTT, and RPC servers for streaming and control
- **Timelapse Recording** - Capture frames per layer, assemble to MP4

## Usage

```bash
# Show help
./rkmpi_enc --help

# Basic streaming with verbose output
./rkmpi_enc -v

# Custom resolution and framerate
./rkmpi_enc -w 1280 -h 720 -f 15

# Enable display capture
./rkmpi_enc --display

# Specify camera device
./rkmpi_enc -d /dev/video0

# Auto frame skip based on CPU (target 60%)
./rkmpi_enc -a -t 60
```

## Command-Line Options

### Camera & Capture

| Option | Description | Default |
|--------|-------------|---------|
| `-d, --device <path>` | Camera device path | auto-detect |
| `-w, --width <n>` | Capture width | 1280 |
| `-h, --height <n>` | Capture height | 720 |
| `-f, --fps <n>` | Target output FPS | 15 |
| `-y, --yuyv` | Use YUYV capture mode (lower CPU) | off |

### H.264 Encoding

| Option | Description | Default |
|--------|-------------|---------|
| `-o, --h264 <path>` | H.264 output file/pipe | none |
| `-b, --bitrate <n>` | H.264 bitrate (kbps) | 512 |
| `-g, --gop <n>` | H.264 GOP size | 30 |
| `-s, --skip <n>` | Skip ratio (1=all, 2=half) | 2 |
| `-n, --no-h264` | Start with H.264 disabled | off |
| `--h264-resolution <WxH>` | H.264 encode resolution | camera res |

### JPEG Encoding

| Option | Description | Default |
|--------|-------------|---------|
| `-j, --jpeg-quality <n>` | JPEG quality (1-99, YUYV mode) | 85 |

### CPU Management

| Option | Description | Default |
|--------|-------------|---------|
| `-a, --auto-skip` | Auto-adjust skip based on CPU | off |
| `-t, --target-cpu <n>` | Target max CPU % (20-90) | 60 |

### Server Mode

| Option | Description | Default |
|--------|-------------|---------|
| `-S, --server` | Enable HTTP/MQTT/RPC servers | off |
| `-N, --no-stdout` | Disable stdout (use with -S) | off |
| `--streaming-port <n>` | MJPEG HTTP port | 8080 |
| `--mode <mode>` | Operating mode (see below) | go-klipper |

## Operating Modes

### go-klipper (default)

For Anycubic printers with **Rinkhals custom firmware**.

- Enables MQTT client for camera commands (port 9883)
- Enables RPC client for timelapse commands (port 18086)
- Handles timelapse recording (openDelayCamera, startLanCapture, etc.)
- Full integration with Anycubic firmware services

### vanilla-klipper

For **external Klipper** setups (e.g., Raspberry Pi) or testing.

- Disables MQTT/RPC clients
- Pure camera capture and HTTP streaming only
- No timelapse support

| Feature | go-klipper | vanilla-klipper |
|---------|------------|-----------------|
| MQTT/RPC clients | ✅ | ❌ |
| Timelapse recording | ✅ | ❌ |
| Works without firmware | ❌ | ✅ |

### Display Capture

| Option | Description | Default |
|--------|-------------|---------|
| `--display` | Enable display capture | off |
| `--display-fps <n>` | Display capture FPS | 5 |

### General

| Option | Description |
|--------|-------------|
| `-v, --verbose` | Verbose output to stderr |
| `-V, --version` | Show version and exit |
| `--help` | Show help message |

### Mode Comparison

| Feature | MJPEG Mode | YUYV Mode |
|---------|------------|-----------|
| Camera Format | MJPEG | YUYV |
| JPEG Output | Pass-through | Hardware encode |
| H.264 Encoding | Hardware | Hardware |
| CPU Usage | ~15-20% | ~5-10% |
| Max FPS (720p) | 30 fps | 5-10 fps |
| Best For | High FPS | Low CPU |

## HTTP Endpoints

When running in server mode (`-S`):

| Port | Endpoint | Description |
|------|----------|-------------|
| 8080 | `/stream` | MJPEG multipart stream |
| 8080 | `/snapshot` | Single JPEG frame |
| 8080 | `/display` | LCD framebuffer stream |
| 8080 | `/display/snapshot` | Single LCD frame |
| 18088 | `/flv` | H.264 in FLV container |

## Building

### Prerequisites

- RV1106 cross-compilation toolchain
- SDK headers (included in `include/`)

### Build Commands

```bash
# Dynamic linking (requires libs on printer)
make dynamic

# Static linking (standalone binary)
make static

# Deploy to printer
make deploy PRINTER_IP=192.168.x.x

# Copy to h264-streamer
make install-h264
```

### Required Libraries on Printer

Located in `/oem/usr/lib/`:
- librockit_full.so
- librockchip_mpp.so
- librga.so
- libdrm.so
- libturbojpeg.so

## Architecture

### MJPEG Capture Mode (default)

```
USB Camera (V4L2 MJPEG)
    │
    ▼
TurboJPEG decode (software, ~15% CPU)
    │
    ▼
NV12 frame buffer
    │
    ├──► VENC Ch0 (H.264) ──► FLV HTTP :18088
    │
    └──► VENC Ch1 (MJPEG) ──► MJPEG HTTP :8080
```

- Higher FPS possible (up to 30 fps)
- Software JPEG decode adds CPU overhead
- Best for: High frame rate streaming

### YUYV Capture Mode (`--yuyv`)

```
USB Camera (V4L2 YUYV)
    │
    ▼
YUYV → NV12 conversion (CPU, ~5% CPU)
    │
    ▼
NV12 frame buffer
    │
    ├──► VENC Ch0 (H.264) ──► FLV HTTP :18088
    │
    └──► VENC Ch1 (MJPEG) ──► MJPEG HTTP :8080  ← Hardware JPEG encoding
```

- Lower CPU usage (no TurboJPEG decode)
- Limited to ~5-10 fps at 720p (USB bandwidth constraint)
- Both H.264 and MJPEG use hardware encoding
- Best for: Low CPU overhead

### Display Capture (`--display`)

```
/dev/fb0 (800x480 BGRX)
    │
    ▼
RGA DMA copy (10-15x faster than CPU memcpy)
    │
    ▼
RGA rotation/flip (model-specific)
    │
    ▼
RGA BGRX → NV12 conversion
    │
    ▼
VENC Ch2 (MJPEG) ──► /display :8080
```

- Full hardware acceleration (RGA + VENC)
- CPU usage: ~7-13% at 10fps
- On-demand: Only encodes when clients connected

## Display Capture

Captures the printer's LCD framebuffer for remote viewing.

### Screen Orientation by Model

| Model | Model ID | Orientation | RGA Operation |
|-------|----------|-------------|---------------|
| KS1, KS1M | 20025, 20029 | 180° flip | imflip() FLIP_H_V |
| K3M | 20026 | 270° rotation | imrotate() ROT_270 |
| K3, K2P, K3V2 | 20024, 20021, 20027 | 90° rotation | imrotate() ROT_90 |

### Performance

- CPU usage: ~7-13% at 10fps (full hardware pipeline)
- Per-frame: ~6-11ms total (RGA copy + rotation + color convert + VENC)

## Environment Setup

On the printer:
```bash
export LD_LIBRARY_PATH=/oem/usr/lib:$LD_LIBRARY_PATH
./rkmpi_enc -v
```

## Files

| File | Description |
|------|-------------|
| `rkmpi_enc.c` | Main encoder source |
| `display_capture.c` | Display capture implementation |
| `http_server.c` | Built-in HTTP server |
| `mqtt_client.c` | MQTT client for camera control |
| `rpc_client.c` | RPC client for timelapse |
| `timelapse.c` | Timelapse recording logic |
| `flv_mux.c` | FLV container muxer |

## Documentation

- [claude.md](claude.md) - Detailed technical documentation
