# h264-streamer

HTTP streaming server with web UI for Rinkhals custom firmware on Anycubic printers.

## Features

- **MJPEG Streaming** - Multipart JPEG stream from USB camera
- **H.264 FLV Streaming** - Hardware-encoded H.264 for Anycubic slicer
- **Display Capture** - Stream the printer's LCD screen remotely
- **Touch Control** - Click on display stream to interact with printer UI
- **Web Control Panel** - Live preview, settings, and monitoring
- **Timelapse Management** - Browse, preview, download, and delete recordings
- **Moonraker Integration** - Compatible webcam endpoints
- **Auto Frame Skipping** - Dynamic frame rate based on CPU load
- **Timelapse Recording** - Layer-by-layer recording to MP4

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
| `/timelapse` | Timelapse management page |
| `/api/stats` | JSON stats (FPS, CPU, settings) |
| `/api/touch` | POST touch events to printer |
| `/api/timelapse/list` | JSON list of recordings |
| `/api/timelapse/thumb/<name>` | Thumbnail image |
| `/api/timelapse/video/<name>` | MP4 video (supports range requests) |
| `/api/timelapse/delete/<name>` | DELETE to remove recording |

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

## Timelapse Recording

h264-streamer supports two timelapse recording modes:

### Native Anycubic Timelapse

Works automatically with Anycubic slicer's built-in timelapse feature.

- **Trigger**: Enable timelapse in Anycubic slicer before slicing
- **Capture**: Slicer inserts layer-change commands that trigger frame capture via RPC
- **Output**: Videos saved to `/useremain/app/gk/Time-lapse-Video/`
- **No configuration needed** - works out of the box in go-klipper mode

### Advanced Timelapse (Moonraker)

Independent timelapse via Moonraker integration (see [Advanced Timelapse](#advanced-timelapse) section below).

- **Trigger**: Enable in h264-streamer control panel
- **Capture**: Layer-based or time-based (hyperlapse) via Moonraker WebSocket
- **Output**: Internal storage or USB drive
- **Configurable**: FPS, quality, variable FPS, flip options

| Feature | Native (Anycubic) | Advanced (Moonraker) |
|---------|-------------------|----------------------|
| Trigger | Slicer setting | Control panel setting |
| Layer detection | Slicer G-code | Moonraker API |
| Hyperlapse mode | ❌ | ✅ |
| USB storage | ❌ | ✅ |
| Variable FPS | ❌ | ✅ |
| Configuration | None | Full control |

**Note**: When Advanced Timelapse is enabled, Native Anycubic timelapse commands are ignored to prevent duplicate recordings.

## Timelapse Management

Access at `http://<printer-ip>:8081/timelapse` or via the "Time Lapse" button on the control page.

### Features

- **Browse Recordings** - View all timelapse videos with thumbnails
- **Storage Selection** - Switch between internal and USB storage
- **Auto Thumbnails** - Generates thumbnails from videos using ffprobe/ffmpeg
- **Metadata Display** - Duration, file size, frame count, creation date
- **Preview** - Play videos directly in browser (HTML5 video player)
- **Download** - Download MP4 files to your computer
- **Delete** - Remove recordings with confirmation dialog
- **Sorting** - Sort by date (newest/oldest), name, or file size

### Storage Locations

| Storage | Path |
|---------|------|
| Internal | `/useremain/app/gk/Time-lapse-Video/` |
| USB | `/mnt/udisk/Time-lapse-Video/` |

| File Type | Naming Pattern |
|-----------|----------------|
| Video | `{gcode_name}_{sequence}.mp4` |
| Thumbnail | `{gcode_name}_{sequence}_{frames}.jpg` |

## Advanced Timelapse

Independent timelapse recording via Moonraker integration, inspired by [moonraker-timelapse](https://github.com/mainsail-crew/moonraker-timelapse).

### Features

- **Independent Recording** - Records regardless of slicer timelapse settings
- **Layer Mode** - Capture frame on each layer change
- **Hyperlapse Mode** - Capture frames at fixed time intervals
- **Moonraker Integration** - Monitors print status via WebSocket
- **USB Storage** - Optional storage to USB drive
- **Configurable Output** - FPS, quality, variable FPS, flip options

### Configuration

Enable in the Timelapse Settings panel on the control page:

#### General Settings

| Setting | Default | Description |
|---------|---------|-------------|
| `timelapse_enabled` | false | Enable advanced timelapse recording |
| `timelapse_mode` | layer | Capture mode: `layer` (on layer change) or `hyperlapse` (time-based) |
| `timelapse_hyperlapse_interval` | 30 | Seconds between captures in hyperlapse mode (5-300) |

#### Storage Settings

| Setting | Default | Description |
|---------|---------|-------------|
| `timelapse_storage` | internal | Storage location: `internal` or `usb` |
| `timelapse_usb_path` | /mnt/udisk/timelapse | USB output directory (click to browse) |

#### Moonraker Connection

| Setting | Default | Description |
|---------|---------|-------------|
| `moonraker_host` | 127.0.0.1 | Moonraker server IP address |
| `moonraker_port` | 7125 | Moonraker WebSocket port |

#### Video Output Settings

| Setting | Default | Description |
|---------|---------|-------------|
| `timelapse_output_fps` | 30 | Video playback framerate (1-120 fps) |
| `timelapse_crf` | 23 | H.264 quality factor (0=best, 51=worst) |
| `timelapse_duplicate_last_frame` | 0 | Repeat final frame N times (pause at end) |

#### Variable FPS Settings

| Setting | Default | Description |
|---------|---------|-------------|
| `timelapse_variable_fps` | false | Auto-calculate FPS based on frame count |
| `timelapse_target_length` | 10 | Target video duration in seconds |
| `timelapse_variable_fps_min` | 5 | Minimum FPS when using variable FPS |
| `timelapse_variable_fps_max` | 60 | Maximum FPS when using variable FPS |

#### Capture Settings

| Setting | Default | Description |
|---------|---------|-------------|
| `timelapse_stream_delay` | 0.05 | Delay (seconds) after layer change before capture |
| `timelapse_flip_x` | false | Horizontal flip (mirror) |
| `timelapse_flip_y` | false | Vertical flip |

### API Endpoints

| Endpoint | Description |
|----------|-------------|
| `/api/timelapse/settings` | POST to save timelapse settings |
| `/api/timelapse/storage` | GET USB/internal storage status |
| `/api/timelapse/moonraker` | GET Moonraker connection status |

### How It Works

1. Enable timelapse in control panel settings
2. Start a print - timelapse automatically begins
3. In layer mode: captures on each `print_stats.info.current_layer` change
4. In hyperlapse mode: captures at configured interval
5. On print complete: assembles frames into MP4 with thumbnail
6. On print cancel: saves partial timelapse (frames captured so far)

### Video Encoding

Videos are encoded using libx264 with memory-optimized settings for the RV1106:
- **Codec**: H.264 (libx264) with ultrafast preset
- **Fallback**: mpeg4 if libx264 fails (OOM)
- **Quality**: Configurable CRF (0-51, default 23)
- **Compatibility**: HTML5 video playback in browsers

### Notes

- When advanced timelapse is enabled, Anycubic slicer timelapse commands are ignored
- USB storage requires a mounted USB drive at `/mnt/udisk`
- Requires rkmpi encoder mode (not gkcam)
- Cancelled prints save partial timelapse video instead of discarding frames

## Configuration

Settings are stored in `app.json`:

| Property | Default | Description |
|----------|---------|-------------|
| `mode` | go-klipper | Operating mode (see below) |
| `encoder_type` | rkmpi-yuyv | Encoder: gkcam, rkmpi, rkmpi-yuyv |
| `autolanmode` | true | Auto-enable LAN mode on start |
| `auto_skip` | false | Auto frame skip based on CPU |
| `target_cpu` | 60 | Target CPU % for auto-skip |
| `bitrate` | 512 | H.264 bitrate (kbps) |
| `mjpeg_fps` | 10 | MJPEG framerate |
| `port` | 8080 | Main HTTP server port |
| `logging` | false | Enable debug logging |

## Operating Modes

### go-klipper (default)

For Anycubic printers with **Rinkhals custom firmware**.

- Manages LAN mode via local binary API (port 18086)
- Kills/restarts gkcam to control camera access
- Enables MQTT/RPC responders for timelapse support
- Full integration with Anycubic firmware services

### vanilla-klipper

For **external Klipper** setups (e.g., Raspberry Pi) or testing without firmware.

- No firmware integration (skips LAN mode, gkcam, MQTT/RPC)
- Pure camera streaming only
- No timelapse support

| Feature | go-klipper | vanilla-klipper |
|---------|------------|-----------------|
| LAN mode management | ✅ | ❌ |
| Timelapse recording | ✅ | ❌ |
| Anycubic slicer | ✅ | ❌ |
| External Klipper | ❌ | ✅ |

## Encoder Modes

### rkmpi-yuyv (Recommended)

YUYV capture from USB camera with full hardware encoding.

- Both MJPEG and H.264 use hardware VENC encoding
- **Lowest CPU usage (~5-10%)**
- **Actual ~4 fps on KS1** (USB bandwidth limited)
- Best for: Low CPU overhead, ideal for print monitoring

### rkmpi

MJPEG capture from USB camera with pass-through for MJPEG streaming.

| Output | Encoding | CPU Usage |
|--------|----------|-----------|
| MJPEG only | Pass-through (no decode) | ~7-8% |
| MJPEG + H.264 | TurboJPEG decode + VENC | ~15-20% |

- Camera delivers MJPEG frames directly to `/stream` endpoint
- H.264 encoding requires software JPEG decode (TurboJPEG) to NV12
- **~10 fps MJPEG, ~2-10 fps H.264 on KS1**
- Best for: Higher frame rate when CPU headroom available

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
| **rkmpi-yuyv** | ~4 | ~4 | 5-10% | Recommended - low CPU |
| rkmpi | ~10 | ~2-10 | 7-20% | Higher CPU with H.264 enabled |
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
