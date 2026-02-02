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

| Option | Description | Default |
|--------|-------------|---------|
| `-d` | Camera device path | auto-detect |
| `-w` | Width | 1280 |
| `-h` | Height | 720 |
| `-f` | Framerate | 15 |
| `-s` | Skip ratio (1=no skip) | 1 |
| `-a` | Enable auto-skip based on CPU | off |
| `-t` | Target CPU % for auto-skip | 60 |
| `-n` | Disable H.264 (MJPEG only) | off |
| `-v` | Verbose output | off |
| `-S` | Server mode (built-in HTTP) | off |
| `--display` | Enable display capture | off |
| `--streaming-port` | HTTP server port | 8080 |

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

```
USB Camera (V4L2 MJPEG)
    │
    ▼
TurboJPEG decode (software)
    │
    ▼
NV12 frame buffer
    │
    ├──► VENC Ch0 (H.264) ──► FLV HTTP :18088
    │
    └──► VENC Ch1 (MJPEG) ──► MJPEG HTTP :8080

Display Capture (--display flag):
    /dev/fb0 ──► RGA rotation ──► RGA BGRX→NV12 ──► VENC Ch2 (MJPEG)
                                                          │
                                                          ▼
                                                   /display :8080
```

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
