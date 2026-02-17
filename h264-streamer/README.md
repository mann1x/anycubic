# h264-streamer

All-in-one USB camera streaming app for Rinkhals custom firmware on Anycubic printers. Pure C implementation — no Python interpreter required.

## Features

- **MJPEG Streaming** - Multipart JPEG stream from USB camera
- **H.264 FLV Streaming** - Hardware-encoded H.264 for Anycubic slicer
- **Multi-Camera Support** - Up to 4 USB cameras with individual settings
- **Display Capture** - Stream the printer's LCD screen remotely
- **Touch Control** - Click on display stream to interact with printer UI
- **Web Control Panel** - Live preview, settings, and monitoring
- **Camera Controls** - Real-time V4L2 camera adjustments (brightness, contrast, etc.)
- **Timelapse Recording** - Layer-by-layer or interval-based recording to MP4
- **Timelapse Management** - Browse, preview, download, and delete recordings
- **Moonraker Integration** - WebSocket timelapse + webcam provisioning
- **Auto Frame Skipping** - Dynamic frame rate based on CPU load
- **ACProxyCam FLV Proxy** - Offload H.264 encoding to external host
- **Config Persistence** - JSON config file for all settings

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
| `/api/config` | JSON full running configuration |
| `/api/touch` | POST touch events to printer |
| `/api/camera/controls` | GET camera controls with values/ranges |
| `/api/camera/set` | POST to set a camera control |
| `/api/camera/reset` | POST to reset camera to defaults |
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

## Multi-Camera Support

h264-streamer supports up to 4 USB cameras with individual settings.

### Port Allocation

| Camera | Port | Stream URL | Description |
|--------|------|------------|-------------|
| CAM#1 | 8080 | `:8080/stream` | Primary (H.264 + MJPEG) |
| CAM#2 | 8082 | `:8082/stream` | Secondary (MJPEG only) |
| CAM#3 | 8083 | `:8083/stream` | Secondary (MJPEG only) |
| CAM#4 | 8084 | `:8084/stream` | Secondary (MJPEG only) |

### Features

- **Automatic Discovery** - Detects USB cameras via `/dev/v4l/by-id/`
- **Dynamic Resolution Detection** - Queries supported resolutions per camera
- **Per-Camera Settings** - Resolution, FPS, and V4L2 controls per camera
- **Persistent Configuration** - Settings saved by camera unique ID
- **Camera Selector** - Switch between cameras in the control panel

### USB Bandwidth Notes

USB 2.0 has limited bandwidth (~35-40 MB/s). With multiple cameras:
- Primary camera can run at 720p MJPEG
- Secondary cameras typically need 640x480 or lower
- YUYV mode uses less bandwidth than MJPEG at same resolution

### Control Panel

- **Camera Selector** - CAM#1, CAM#2, etc. buttons next to Live Preview
- **Additional Cameras Settings** - Panel for enabling and configuring secondary cameras
- **Camera Controls** - Applies to currently selected camera

### API Endpoints

| Endpoint | Method | Description |
|----------|--------|-------------|
| `/api/cameras` | GET | List all cameras with ports and status |
| `/api/camera/enable` | POST | Enable a camera `{"id": 2}` |
| `/api/camera/disable` | POST | Disable a camera `{"id": 2}` |
| `/api/camera/settings` | POST | Update resolution/FPS for a camera |

---

## Moonraker Camera Settings

Configure which cameras are provisioned to Moonraker for the dashboard and web UI.

### Features

- **Per-camera provisioning** - Enable/disable Moonraker registration independently
- **Custom names** - Set custom names for each camera (e.g., "Bed Camera")
- **Dashboard default** - Select which camera is the Mainsail/Fluidd default
- **Immediate apply** - Changes provision to Moonraker instantly

### Control Panel

The "Moonraker Camera Settings" panel shows each camera with:
- **Name in Moonraker** - Editable custom name
- **Provision** - Checkbox to enable/disable
- **Default** - Radio button for dashboard default

### API Endpoints

| Endpoint | Method | Description |
|----------|--------|-------------|
| `/api/moonraker/cameras` | GET | Get cameras and Moonraker settings |
| `/api/moonraker/cameras` | POST | Save and provision to Moonraker |

---

## Camera Controls

Real-time USB camera V4L2 control via a collapsible panel on the control page.

### Features

- **Real-time adjustment** - Changes apply immediately without "Apply" button
- **Persistent settings** - Camera settings saved and restored on restart
- **Reset to Defaults** - One-click restore of all camera defaults
- **Auto-disable controls** - White balance temp disabled when auto is on, exposure disabled when auto is on

### Available Controls

| Control | Range | Default | Notes |
|---------|-------|---------|-------|
| Brightness | 0-255 | 0 | |
| Contrast | 0-255 | 32 | |
| Saturation | 0-132 | 85 | |
| Hue | -180 to 180 | 0 | |
| Gamma | 90-150 | 100 | |
| Sharpness | 0-30 | 3 | |
| Gain | On/Off | On | |
| Backlight Compensation | 0-7 | 0 | |
| Auto White Balance | On/Off | On | |
| White Balance Temperature | 2800-6500K | 4000K | Only when auto is off |
| Auto Exposure | Manual/Auto | Auto | |
| Exposure | 10-2500 | 156 | Only when manual mode |
| Exposure Priority | Constant FPS/Variable | Constant | |
| Power Line Frequency | Off/50Hz/60Hz | 50Hz | Anti-flicker |

### API Endpoints

| Endpoint | Method | Description |
|----------|--------|-------------|
| `/api/camera/controls` | GET | Get all controls with current values and ranges |
| `/api/camera/set` | POST | Set a single control: `{"control": "contrast", "value": 50}` |
| `/api/camera/reset` | POST | Reset all controls to defaults |

### JPEG Quality

**Note**: USB cameras don't support V4L2 JPEG quality control. In rkmpi-yuyv mode, JPEG quality is controlled via the "JPEG Quality" slider in the main settings panel (1-100, default 85).

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

Videos are encoded using the RV1106 hardware VENC encoder:
- **Codec**: H.264 via hardware VENC (no ffmpeg dependency)
- **Muxer**: minimp4 header-only library for MP4 container
- **Fallback**: ffmpeg with libx264 if VENC fails
- **Compatibility**: HTML5 video playback in browsers

### Notes

- When advanced timelapse is enabled, Anycubic slicer timelapse commands are ignored
- USB storage requires a mounted USB drive at `/mnt/udisk`
- Requires rkmpi encoder mode (not gkcam)
- Cancelled prints save partial timelapse video instead of discarding frames

## Configuration

Basic settings in `app.json` (Rinkhals app properties):

| Property | Default | Description |
|----------|---------|-------------|
| `mode` | go-klipper | Operating mode (see below) |
| `streaming_port` | 8080 | Streaming server port |
| `control_port` | 8081 | Control server port |
| `logging` | false | Enable debug logging |

All other settings are persisted in `29-h264-streamer.config` (JSON) and managed via the control page:

| Setting | Default | Description |
|---------|---------|-------------|
| `encoder_type` | rkmpi | Encoder mode: rkmpi, rkmpi-yuyv |
| `autolanmode` | true | Auto-enable LAN mode on start |
| `h264_enabled` | true | Enable H.264 encoding |
| `auto_skip` | false | Auto frame skip based on CPU |
| `target_cpu` | 60 | Target CPU % for auto-skip |
| `bitrate` | 512 | H.264 bitrate (kbps) |
| `mjpeg_fps` | 10 | MJPEG framerate |
| `display_enabled` | false | Enable display capture |
| `timelapse_enabled` | false | Enable Moonraker timelapse |
| `acproxycam_flv_proxy` | false | Enable ACProxyCam FLV proxy |

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

### Setting the Mode

To switch to vanilla-klipper mode, use the Rinkhals `set_app_property` command:

```bash
set_app_property 29-h264-streamer mode vanilla-klipper
```

To switch back to go-klipper mode:

```bash
set_app_property 29-h264-streamer mode go-klipper
```

After changing mode, restart the app:

```bash
cd home/rinkhals/apps/29-h264-streamer/ && ./app.sh stop && sleep 2 && ./app.sh start
```

### Troubleshooting

Enable logging in the control panel settings, then view logs with:

```bash
tail -F /tmp/rinkhals/app-h264-streamer.log -n 100
```

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

### Mode Comparison

| Mode | MJPEG FPS | H.264 FPS | CPU Usage | Notes |
|------|-----------|-----------|-----------|-------|
| **rkmpi-yuyv** | ~4 | ~4 | 5-10% | Recommended - low CPU |
| rkmpi | ~10 | ~2-10 | 7-20% | Higher CPU with H.264 enabled |

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

## Files

```
29-h264-streamer/
├── app.json          # Rinkhals app metadata
├── app.sh            # Start/stop script
├── h264_monitor.sh   # Config reader, launches primary encoder
├── rkmpi_enc         # All-in-one binary (encoder + control server + APIs)
├── control.html      # Control page template
├── index.html        # Homepage template
└── timelapse.html    # Timelapse manager template
```

## Documentation

- [claude.md](claude.md) - Detailed technical documentation
