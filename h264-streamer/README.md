# h264-streamer

HTTP streaming server with web UI for Rinkhals custom firmware on Anycubic printers.

## Features

- **MJPEG Streaming** - Multipart JPEG stream from USB camera
- **H.264 FLV Streaming** - Hardware-encoded H.264 for Anycubic slicer
- **Display Capture** - Stream the printer's LCD screen remotely
- **Touch Control** - Click on display stream to interact with printer UI
- **Web Control Panel** - Live preview, settings, and monitoring
- **Moonraker Integration** - Compatible webcam endpoints
- **Auto Frame Skipping** - Dynamic frame rate based on CPU load
- **Timelapse Support** - Layer-by-layer recording to MP4

## Installation

Download the SWU package for your printer model from [Releases](https://github.com/mann1x/anycubic/releases).

| Printer | SWU File |
|---------|----------|
| Kobra 2 Pro | h264-streamer-K2P.swu |
| Kobra 3 | h264-streamer-K3.swu |
| Kobra 3 V2 | h264-streamer-K3V2.swu |
| Kobra S1 | h264-streamer-KS1.swu |
| Kobra S1 Max | h264-streamer-KS1M.swu |
| Kobra 3 Max | h264-streamer-K3M.swu |

### Install via USB

1. Rename the SWU file to `update.swu`
2. Copy to `aGVscF9zb3Nf` folder on a FAT32 USB drive
3. Insert USB into printer and wait for installation

## HTTP Endpoints

### Streaming (Port 8080)

| Endpoint | Description |
|----------|-------------|
| `/stream` | MJPEG multipart stream (USB camera) |
| `/snapshot` | Single JPEG frame |
| `/display` | MJPEG stream of printer LCD |
| `/display/snapshot` | Single LCD frame |
| `/status` | JSON status |

### Control (Port 8081)

| Endpoint | Description |
|----------|-------------|
| `/control` | Web UI with settings and preview |
| `/api/stats` | JSON stats (FPS, CPU, settings) |
| `/api/touch` | POST touch events to printer |

### FLV (Port 18088)

| Endpoint | Description |
|----------|-------------|
| `/flv` | H.264 in FLV container (for slicer) |

## Web Control Panel

Access at `http://<printer-ip>:8081/control`

### Features

- **Tab-based Preview** - Snapshot, MJPEG, H.264, Display tabs
- **Real-time Stats** - MJPEG FPS, H.264 FPS, CPU usage
- **Display Stream** - Click to send touch events
- **Settings**:
  - Auto LAN mode toggle
  - H.264 encoding on/off
  - Frame rate slider (0-100%)
  - Auto-skip with CPU target
  - Display capture on/off
  - Display FPS selection

## Touch Control

Click anywhere on the display stream to send touch events to the printer.

The server automatically transforms coordinates based on printer model orientation:

| Model | Display Size | Touch Transform |
|-------|--------------|-----------------|
| KS1, KS1M | 800x480 | `(800-x, 480-y)` |
| K3, K2P, K3V2 | 480x800 | `(y, 480-x)` |
| K3M | 480x800 | `(800-y, x)` |

## Configuration

Settings are stored in `app.json`:

| Property | Default | Description |
|----------|---------|-------------|
| `encoder_type` | rkmpi-yuyv | Encoder: gkcam, rkmpi, rkmpi-yuyv |
| `autolanmode` | true | Auto-enable LAN mode on start |
| `auto_skip` | false | Auto frame skip based on CPU |
| `target_cpu` | 60 | Target CPU % for auto-skip |
| `bitrate` | 512 | H.264 bitrate (kbps) |
| `mjpeg_fps` | 10 | MJPEG framerate |
| `port` | 8080 | Main HTTP server port |
| `logging` | false | Enable debug logging |

## Encoder Modes

### rkmpi (Recommended)

MJPEG capture from USB camera with pass-through for MJPEG streaming.

| Output | Encoding | CPU Usage |
|--------|----------|-----------|
| MJPEG only | Pass-through (no decode) | ~7-8% |
| MJPEG + H.264 | TurboJPEG decode + VENC | ~15-20% |

- Camera delivers MJPEG frames directly to `/stream` endpoint
- H.264 encoding requires software JPEG decode (TurboJPEG) to NV12
- Higher FPS possible (~30 fps depending on camera)
- Best balance of quality and flexibility

### rkmpi-yuyv

YUYV capture from USB camera with full hardware encoding.

- Both MJPEG and H.264 use hardware VENC encoding
- Lower CPU usage (~5-10%)
- **Nominal 10 fps, actual ~4 fps on KS1** (USB bandwidth limited)
- Best for: Minimum CPU overhead when low FPS is acceptable

### gkcam

Uses the native gkcam camera service (no rkmpi_enc).

- Proxies gkcam's FLV stream at `:18088/flv`
- **H.264 stream runs at ~4 fps on KS1**
- MJPEG extracted from H.264 keyframes (~1 fps)
- Lowest resource usage
- Does not kill gkcam (coexists with native firmware)

### Mode Comparison

| Mode | MJPEG FPS | H.264 FPS | CPU Usage | Notes |
|------|-----------|-----------|-----------|-------|
| rkmpi | up to 30 | up to 30 | 7-20% | Higher CPU only with H.264 enabled |
| rkmpi-yuyv | ~4 | ~4 | 5-10% | USB bandwidth limited |
| gkcam | ~1 | ~4 | <5% | Uses native firmware stream |

*FPS values measured on Kobra S1. Other models may vary.*

## Process Management

```bash
# Stop
/useremain/home/rinkhals/apps/29-h264-streamer/app.sh stop

# Start
/useremain/home/rinkhals/apps/29-h264-streamer/app.sh start

# Status
/useremain/home/rinkhals/apps/29-h264-streamer/app.sh status
```

**Warning**: Never use `killall python` or `pkill python` - this will break Moonraker/Klipper!

## Files

```
29-h264-streamer/
├── app.json          # Configuration
├── app.sh            # Start/stop script
├── h264_server.py    # Main Python server
├── h264_monitor.sh   # Monitor/restart script
└── rkmpi_enc         # Hardware encoder binary
```

## Documentation

- [claude.md](claude.md) - Detailed technical documentation
