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

### rkmpi-yuyv (Default)

- YUYV capture from USB camera
- Hardware JPEG + H.264 encoding
- Lowest CPU usage (~5-10%)
- Max ~10 fps at 720p

### rkmpi

- MJPEG capture from USB camera
- Software JPEG decode, hardware H.264
- Higher FPS possible (~30 fps)
- Higher CPU usage (~15-20%)

### gkcam

- Uses native gkcam FLV stream
- Lowest resource usage
- Limited to ~1-2 fps MJPEG

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
