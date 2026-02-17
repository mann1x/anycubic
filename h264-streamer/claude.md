# h264-streamer App

USB camera streaming app for Rinkhals with hardware H.264 encoding using the RV1106 VENC. Pure C implementation — no Python interpreter required.

---

## Overview

This app provides HTTP endpoints for streaming video from USB cameras, with a built-in control web UI, REST API, multi-camera management, timelapse recording, and Moonraker integration.

**Streaming server (port 8080, configurable via `streaming_port` property):**
- `/stream` - MJPEG multipart stream (USB camera)
- `/snapshot` - Single JPEG frame (USB camera)
- `/display` - MJPEG stream of printer LCD framebuffer (configurable fps)
- `/display/snapshot` - Single JPEG of printer LCD
- `/flv` - H.264 in FLV container

**Control server (port 8081, configurable via `control_port` property):**
- `/control` - Web UI with settings, FPS/CPU monitoring, and live stream preview
- `/status` - JSON status
- `/api/stats` - JSON stats (FPS, CPU, settings)
- `/api/config` - Current configuration as JSON
- `/api/camera/controls` - Camera V4L2 controls
- `/api/cameras` - Multi-camera list with status
- `/api/moonraker/cameras` - Moonraker camera provisioning
- `/api/timelapse/*` - Timelapse management API
- `/api/acproxycam/flv` - ACProxyCam FLV proxy status
- `/api/touch` - Touch injection
- `/api/restart` - Restart the application
- `/timelapse` - Timelapse management web UI

**FLV server (port 18088, fixed for Anycubic slicer):**
- `/flv` - H.264 in FLV container (legacy endpoint, same stream as :8080/flv)

## Architecture

The app runs as a single `rkmpi_enc --primary` process that handles everything:

```
USB Camera (V4L2 MJPEG/YUYV)
    ↓
rkmpi_enc --primary
    ├── TurboJPEG decode → NV12 → VENC Ch0 → H.264 → FLV server (:8080/flv, :18088/flv)
    ├── MJPEG passthrough/encode → HTTP server (:8080/stream, /snapshot)
    ├── Display capture → RGA → VENC Ch2 → MJPEG (:8080/display)
    ├── Control server (:8081) — web UI, REST API, config persistence
    ├── MQTT client — camera commands from Anycubic firmware
    ├── RPC client — timelapse commands from Anycubic slicer
    ├── Moonraker WebSocket client — advanced timelapse via Moonraker
    ├── Multi-camera management — detect, spawn, manage secondary cameras
    └── Config management — JSON config persistence, live config updates
```

### Process Hierarchy

```
h264_monitor.sh          # Reads config, builds CLI args, launches primary encoder
  └── rkmpi_enc --primary  # Single process handling everything
        ├── Secondary rkmpi_enc instances (--no-flv, per camera)
        └── Moonraker WebSocket thread (if timelapse enabled)
```

## Components

### rkmpi_enc (--primary mode)

The encoder binary in primary mode is the sole application process:

- **HTTP Streaming** — MJPEG, H.264/FLV, display capture on port 8080
- **Control Server** — Web UI, REST API, config management on port 8081
- **LAN Mode Management** — Queries/enables LAN mode via local binary API (port 18086)
- **gkcam Integration** — Kills gkcam to free the USB camera
- **MQTT Client** — Handles camera commands from firmware (port 9883 TLS)
- **RPC Client** — Handles timelapse commands from Anycubic slicer (port 18086)
- **Moonraker Client** — WebSocket client for advanced timelapse via Moonraker
- **Multi-Camera Manager** — Auto-detects cameras, spawns secondary encoder instances
- **Camera Detection** — Discovers cameras via `/dev/v4l/by-id/`, resolves USB ports
- **Config Persistence** — Reads/writes JSON config file with all settings
- **CPU Monitoring** — Tracks system and per-process CPU usage

Source: `/shared/dev/anycubic/rkmpi-encoder/`

### app.sh

App lifecycle management:
- Reads `mode` and `streaming_port`/`control_port` properties from app.json
- Delegates to h264_monitor.sh for process management
- Stops/starts using `kill_by_name rkmpi_enc` and `kill_by_name h264_monitor`

### h264_monitor.sh

Startup script that reads config and launches the primary encoder:
- Reads encoder settings from JSON config file (`29-h264-streamer.config`)
- Detects primary camera device via `/dev/v4l/by-path/`
- Builds rkmpi_enc CLI arguments
- Launches `rkmpi_enc --primary --config <config_file> --template-dir <app_dir>`

## Local Binary API (Port 18086)

The printer's local binary API uses TCP with ETX (0x03) message terminator.

### Query LAN Mode Status
```json
{"id": 2016, "method": "Printer/QueryLanPrintStatus", "params": null}
```
Response: `{"id": 2016, "result": {"open": 0|1}}`

### Enable LAN Mode
```json
{"id": 2016, "method": "Printer/OpenLanPrint", "params": null}
```

### Disable LAN Mode
```json
{"id": 2016, "method": "Printer/CloseLanPrint", "params": null}
```

**Note:** After enabling LAN mode, gkcam must be killed to free the camera device.

## MQTT Camera Control (Port 9883)

gkcam requires an MQTT `startCapture` command to begin streaming. The printer runs a Mochi MQTT broker on port 9883 (TLS).

### Topic Format
```
anycubic/anycubicCloud/v1/web/printer/{modelId}/{deviceId}/video
```

### startCapture Command
```json
{
  "type": "video",
  "action": "startCapture",
  "timestamp": 1700000000000,
  "msgid": "uuid-string",
  "data": null
}
```

### Model IDs (from `/userdata/app/gk/config/api.cfg`)
| Model Code | Model ID | Printer |
|------------|----------|---------|
| K2P | 20021 | Kobra 2 Pro |
| K3 | 20024 | Kobra 3 |
| KS1 | 20025 | Kobra S1 |
| K3M | 20026 | Kobra 3 Max |
| K3V2 | 20027 | Kobra 3 V2 |
| KS1M | 20029 | Kobra S1 Max |

### Credentials
- **Path:** `/userdata/app/gk/config/device_account.json`
- **Fields:** `deviceId`, `username`, `password`

**Important:** Uses QoS 1 (AtLeastOnce) for reliable delivery. The model ID must be read from `api.cfg`, not guessed.

## Control Page Features

The `/control` endpoint (on port 8081) provides a web UI served from `control.html`:
- **Tab-based preview**: Snapshot, MJPEG Stream, H.264 Stream, Display Stream
- **H.264 player**: Uses flv.js from CDN for live FLV playback in browser
- **Display preview**: Shows printer LCD framebuffer (MJPEG)
- **Performance stats**: Real-time MJPEG FPS, H.264 FPS, Total CPU, Encoder CPU
- **Settings**:
  - Auto LAN mode toggle
  - H.264 encoding toggle
  - Encoder type selection (rkmpi/rkmpi-yuyv)
  - Frame Rate slider with text input
  - Auto Skip toggle with target CPU %
  - Display capture toggle with FPS
  - ACProxyCam FLV proxy toggle
  - Camera V4L2 controls (brightness, contrast, etc.)
- **Camera management**: Enable/disable secondary cameras, set resolution/FPS
- **Moonraker Camera Settings**: Provision cameras to Moonraker with custom names
- **Timelapse Settings**: Mode, storage, Moonraker connection, video output options
- **Timelapse Manager**: Browse, preview, download, delete recordings

### Port Layout
- **Port 8080**: Streaming endpoints (`/stream`, `/snapshot`, `/display`, `/display/snapshot`, `/flv`)
- **Port 8081**: Control page (`/control`, `/api/*`, `/timelapse`)
- **Port 18088**: FLV H.264 stream (`/flv`, legacy endpoint for Anycubic slicer)

### API Stats Response (`/api/stats`)
```json
{
  "cpu": {"total": 47.4, "processes": {"2673": 18.6, "2591": 11.3}},
  "fps": {"mjpeg": 10.0, "h264": 5.0},
  "h264_enabled": true,
  "skip_ratio": 2,
  "auto_skip": true,
  "target_cpu": 60,
  "autolanmode": true,
  "display_enabled": true,
  "display_fps": 5,
  "display_clients": 0,
  "encoder_type": "rkmpi",
  "session_id": "abc123"
}
```

---

## Multi-Camera Support

h264-streamer supports up to 4 USB cameras simultaneously, with per-camera settings and control. The primary rkmpi_enc process manages secondary encoder instances.

### Port Allocation

| Camera | Streaming Port | Description |
|--------|---------------|-------------|
| CAM#1 | 8080 | Primary camera (full H.264 + MJPEG) |
| CAM#2 | 8082 | Secondary camera (MJPEG only) |
| CAM#3 | 8083 | Secondary camera (MJPEG only) |
| CAM#4 | 8084 | Secondary camera (MJPEG only) |

**Note:** Secondary cameras use `--no-flv` mode which disables H.264/FLV to avoid VENC resource conflicts.

### Camera Discovery

Cameras are discovered via `/dev/v4l/by-id/` symlinks and sorted with the internal camera first (USB port 1.3). Each camera is assigned:
- **unique_id**: Stable identifier from by-id symlink (persists across reboots)
- **id**: Camera number (1-4, assigned after sorting)
- **streaming_port**: Port for this camera's encoder

### Per-Camera Settings

Each camera has its own settings stored by unique_id in the config file:
```json
{
  "cameras": {
    "usb-SunplusIT_Inc_HD_Camera-video-index0": {
      "enabled": true,
      "mjpeg_fps": 10,
      "resolution": "640x480",
      "cam_brightness": 0,
      "cam_contrast": 32
    }
  }
}
```

### Secondary Camera Modes

Secondary cameras (CAM#2-4) support:
- **YUYV mode**: Lower USB bandwidth, hardware JPEG encoding
- **Lower resolutions**: 640x480, 800x600 to fit USB bandwidth constraints

**USB Bandwidth Limitations:**
- Total USB 2.0 bandwidth: ~35-40 MB/s
- Multiple 720p MJPEG cameras can exhaust bandwidth
- Secondary cameras typically need 640x480 or lower

### Camera API Endpoints

| Endpoint | Method | Description |
|----------|--------|-------------|
| `/api/cameras` | GET | List all cameras with status, ports, settings |
| `/api/camera/enable` | POST | Enable camera, start its encoder |
| `/api/camera/disable` | POST | Disable camera, stop its encoder |
| `/api/camera/settings` | POST | Update camera resolution/FPS (requires restart) |
| `/api/camera/controls` | GET | Get V4L2 controls for current camera |
| `/api/camera/set` | POST | Apply V4L2 control change |

### Camera List API Response (`/api/cameras`)
```json
{
  "cameras": [
    {
      "id": 1,
      "unique_id": "usb-SunplusIT_Inc_HD_Camera-video-index0",
      "name": "SunplusIT Inc HD Camera",
      "device": "/dev/video10",
      "usb_port": "1.3",
      "enabled": true,
      "streaming_port": 8080,
      "is_primary": true,
      "resolution": "1280x720",
      "fps": 10,
      "resolutions": [[1280, 720], [640, 480], [320, 240]]
    }
  ],
  "active_camera_id": 1
}
```

### USB Bandwidth Troubleshooting

**Error: `VIDIOC_STREAMON: No space left on device`**

This indicates USB bandwidth exhaustion. Solutions:
1. Reduce secondary camera resolution (try 640x480 or lower)
2. Reduce frame rate on all cameras
3. Disable cameras not in use
4. Some cameras work better with YUYV mode at lower resolutions

---

## Moonraker Camera Settings

The control page includes a panel to configure which cameras are provisioned to Moonraker for the dashboard and web UI.

### Features

- **Per-camera Moonraker provisioning**: Enable/disable provisioning for each camera independently
- **Custom names**: Set custom names for each camera in Moonraker (e.g., "Bed Camera", "Nozzle Cam")
- **Dashboard default**: Select which camera appears by default on the Mainsail/Fluidd dashboard
- **Immediate provisioning**: Changes apply immediately to Moonraker without restart
- **Persistent settings**: Settings saved to config and restored on restart

### API Endpoints

| Endpoint | Method | Description |
|----------|--------|-------------|
| `/api/moonraker/cameras` | GET | Get cameras with Moonraker settings |
| `/api/moonraker/cameras` | POST | Save settings and provision to Moonraker |

### Config Storage

Settings are stored per-camera in the config file:
```json
{
  "cameras": {
    "usb-SunplusIT_Inc_HD_Camera-video-index0": {
      "enabled": true,
      "moonraker_enabled": true,
      "moonraker_name": "Bed Camera",
      "moonraker_default": true
    }
  }
}
```

### Moonraker Webcam API

The following Moonraker API endpoints are used:
- `POST /server/webcams/item` - Create/update webcam
- `DELETE /server/webcams/item?name=...` - Delete webcam
- `POST /server/database/item` - Set default webcam for Mainsail

---

## ACProxyCam FLV Proxy

When `acproxycam_flv_proxy` is enabled, h264-streamer proxies FLV streams from ACProxyCam instead of encoding H.264 locally. This offloads the CPU-intensive H.264 encoding from the resource-constrained printer (RV1106, 1 core, 256MB) to a more powerful host running ACProxyCam.

### Architecture
```
USB Camera ──MJPEG──> rkmpi_enc :8080/stream ──MJPEG──> ACProxyCam (Pi 5/x64)
                      (--no-flv, no H.264)              MJPEG→H.264 encoding
                                                         /flv endpoint
                                                              │
Slicer ──> :18088/flv ──> control_server ─────HTTP GET────────┘
           (C proxy)      transparent byte proxy
```

### Configuration
- **Control page**: Checkbox in settings (visible only in rkmpi mode)
- **Config key**: `acproxycam_flv_proxy` (true/false)
- **API**: `GET/POST /api/acproxycam/flv` for status and announcement

### How It Works
1. When enabled, rkmpi_enc starts with `--no-flv` (disables VENC H.264, saves CPU)
2. The control server listens on port 18088 for FLV connections
3. ACProxyCam announces its `/flv` URL via `POST /api/acproxycam/flv` (periodic re-announcement every 30s)
4. When slicer connects to `:18088/flv`, the proxy opens upstream connection to ACProxyCam
5. Raw FLV bytes are forwarded transparently (no parsing)
6. RPC/MQTT responders handle slicer protocol integration

### Edge Cases
- ACProxyCam not ready: proxy retries every 2s for ~30s, then closes client (slicer reconnects)
- ACProxyCam goes down: upstream read returns empty → client closed (slicer reconnects)
- Proxy toggled off: encoder restarts with native FLV
- Multiple slicers: each gets own upstream connection to ACProxyCam

---

## Operating Modes

The `--mode` flag controls how h264-streamer integrates with the printer's firmware.

### go-klipper (default)

For Anycubic printers running **Rinkhals custom firmware**. This is the standard configuration.

**Features:**
- **LAN Mode Management** - Queries and auto-enables LAN mode via local binary API (port 18086)
- **gkcam Integration** - Kills gkcam to free the USB camera, restarts it on shutdown
- **MQTT Client** - Handles camera commands from Anycubic firmware
- **RPC Client** - Handles timelapse commands from Anycubic slicer
- **Moonraker Client** - WebSocket timelapse integration (when enabled)
- **Full Firmware Integration** - Works alongside Anycubic's native services

### vanilla-klipper

For setups using **external Klipper** (e.g., on a Raspberry Pi) connected to the printer via USB, or for testing without Anycubic firmware dependencies.

**Features:**
- **No Firmware Integration** - Skips all Anycubic-specific initialization
- **No LAN Mode** - Does not query or enable LAN mode
- **No gkcam Management** - Does not kill or restart gkcam
- **No MQTT/RPC** - Disables MQTT client and RPC responders
- **Pure Camera Streaming** - Just captures from USB camera and serves HTTP streams

### Mode Comparison

| Feature | go-klipper | vanilla-klipper |
|---------|------------|-----------------|
| LAN mode management | Yes | No |
| gkcam kill/restart | Yes | No |
| MQTT client | Yes | No |
| RPC client (timelapse) | Yes | No |
| Moonraker client | Yes | No |
| Anycubic slicer support | Yes | No |
| Works without firmware | No | Yes |
| External Klipper | No | Yes |

### Setting the Mode

**Via app.json property:**
```json
{
    "mode": "vanilla-klipper"
}
```

---

## Properties (app.json)

- `mode` (string, default: "go-klipper") - Operating mode: "go-klipper" or "vanilla-klipper"
- `streaming_port` (int, default: 8080) - Streaming server port
- `control_port` (int, default: 8081) - Control server port
- `logging` (bool, default: false) - Enable debug logging

**Note:** All other settings (encoder_type, bitrate, mjpeg_fps, h264_resolution, auto_skip, target_cpu, skip_ratio, jpeg_quality, autolanmode, display, timelapse, etc.) are stored in the JSON config file (`29-h264-streamer.config`) and managed via the control page API.

## Encoder Modes

### rkmpi mode (default)
- Uses rkmpi_enc binary for direct USB camera capture (MJPEG format)
- Hardware H.264 encoding via RV1106 VENC
- TurboJPEG software JPEG decoding (~15% CPU at 720p)
- Higher quality, more configurable
- Kills gkcam to access camera
- Maximum FPS: 30 fps (camera MJPEG limit)

### rkmpi-yuyv mode
- Uses rkmpi_enc binary with YUYV capture from USB camera
- **Both H.264 and JPEG use hardware encoding** via RV1106 VENC
- Lower CPU usage (~5-10%) - only YUYV→NV12 conversion in software
- No TurboJPEG dependency
- Kills gkcam to access camera
- Maximum FPS: ~5-10 fps (YUYV bandwidth limit at 720p)

**Comparison:**
| Feature | rkmpi (MJPEG) | rkmpi-yuyv |
|---------|---------------|------------|
| Camera Format | MJPEG | YUYV |
| JPEG Output | Pass-through | HW Encode |
| H.264 Encoding | HW (VENC) | HW (VENC) |
| CPU Usage | ~15-20% | ~5-10% |
| Max FPS (720p) | 30 fps | 5-10 fps |
| Best For | High FPS | Low CPU |

## Building rkmpi_enc

### Prerequisites
- RV1106 cross-compilation toolchain at `/shared/dev/rv1106-toolchain`
- RKMPI SDK libraries (rockit, mpp, rga, turbojpeg)

### Build Commands
```bash
cd /shared/dev/anycubic/rkmpi-encoder

# Build with dynamic linking (requires libs on target)
make dynamic

# Copy to h264-streamer directory
make install-h264
```

### Required Libraries on Printer
Located in `/oem/usr/lib/`:
- librockit_full.so
- librockchip_mpp.so
- librga.so
- libdrm.so
- libturbojpeg.so

## Building SWU Package

### Build Scripts
- **Standalone build:** `./scripts/build-h264-swu.sh KS1 ./dist`
- **Rinkhals build:** `/shared/dev/Rinkhals/build/swu-tools/h264-streamer/build-local.sh`

### Build All Models (Local)
```bash
/shared/dev/Rinkhals/build/swu-tools/h264-streamer/build-local.sh
```

### Build Single Model
```bash
/shared/dev/Rinkhals/build/swu-tools/h264-streamer/build-local.sh KS1
```

### SWU Package Contents
```
h264-streamer-{MODEL}.swu    # Password-protected zip
└── update_swu/
    ├── setup.tar.gz         # Compressed tarball
    ├── setup.tar.gz.md5     # Checksum
    └── [contents]:
        ├── update.sh        # Installer script
        └── app/
            ├── app.json     # Rinkhals app metadata
            ├── app.sh       # Start/stop script
            ├── h264_monitor.sh
            ├── rkmpi_enc    # Encoder binary (all-in-one)
            ├── control.html # Control page template
            ├── index.html   # Homepage template
            └── timelapse.html # Timelapse manager template
```

### SWU Passwords by Model (Reference)
- K2P, K3, K3V2: `U2FsdGVkX19deTfqpXHZnB5GeyQ/dtlbHjkUnwgCi+w=`
- KS1, KS1M: `U2FsdGVkX1+lG6cHmshPLI/LaQr9cZCjA8HZt6Y8qmbB7riY`
- K3M: `4DKXtEGStWHpPgZm8Xna9qluzAI8VJzpOsEIgd8brTLiXs8fLSu3vRx8o7fMf4h6`

### Installation
1. Rename SWU to `update.swu`
2. Copy to `aGVscF9zb3Nf` folder on FAT32 USB drive (MBR, not GPT)
3. Plug USB into printer

## File Locations

### Development Workflow
All development is done in the **anycubic** repository. Rinkhals uses a symlink to reference it.

```
# Source (edit and commit here)
/shared/dev/anycubic/h264-streamer/29-h264-streamer/

# Rinkhals symlink (DO NOT edit directly)
/shared/dev/Rinkhals/files/4-apps/home/rinkhals/apps/29-h264-streamer/
  -> /srv/dev-disk-by-label-opt/dev/anycubic/h264-streamer/29-h264-streamer/
```

### File Locations
- **App source (primary):** `/shared/dev/anycubic/h264-streamer/29-h264-streamer/`
- **Encoder source:** `/shared/dev/anycubic/rkmpi-encoder/`
- **SWU build tools:** `/shared/dev/Rinkhals/build/swu-tools/h264-streamer/`
- **Built SWU:** `/shared/dev/Rinkhals/build/dist/h264-streamer-KS1.swu`

### On Printer
- App install path: `/useremain/home/rinkhals/apps/29-h264-streamer/`
- Config file: `/useremain/home/rinkhals/apps/29-h264-streamer.config`
- RKMPI libraries: `/oem/usr/lib/`
- Application log: `/tmp/rinkhals/app-h264-streamer.log`

## Testing on Printer

### Printer Connection
- **IP:** 192.168.178.43
- **User:** root
- **Password:** rockchip
- **App path:** /useremain/home/rinkhals/apps/29-h264-streamer

### Deploy and test manually
```bash
PRINTER_IP=192.168.178.43

# IMPORTANT: Stop app before deploying (or encoder binary won't update)
sshpass -p 'rockchip' ssh root@$PRINTER_IP '/useremain/home/rinkhals/apps/29-h264-streamer/app.sh stop'

# Copy encoder binary (from rkmpi-encoder)
sshpass -p 'rockchip' scp /shared/dev/anycubic/rkmpi-encoder/rkmpi_enc root@$PRINTER_IP:/useremain/home/rinkhals/apps/29-h264-streamer/

# Copy HTML templates (if changed)
sshpass -p 'rockchip' scp /shared/dev/anycubic/h264-streamer/29-h264-streamer/*.html root@$PRINTER_IP:/useremain/home/rinkhals/apps/29-h264-streamer/

# Start app
sshpass -p 'rockchip' ssh root@$PRINTER_IP '/useremain/home/rinkhals/apps/29-h264-streamer/app.sh start'

# Test endpoints
curl http://$PRINTER_IP:8080/status
curl http://$PRINTER_IP:8080/snapshot -o snapshot.jpg
curl http://$PRINTER_IP:8080/display/snapshot -o display.jpg
curl http://$PRINTER_IP:8081/api/stats
```

### Check logs
```bash
# Main application log
sshpass -p 'rockchip' ssh root@$PRINTER_IP 'tail -50 /tmp/rinkhals/app-h264-streamer.log'
```

### Process Management

Use app.sh for all process management:
```bash
sshpass -p 'rockchip' ssh root@$PRINTER_IP '/useremain/home/rinkhals/apps/29-h264-streamer/app.sh stop'
sshpass -p 'rockchip' ssh root@$PRINTER_IP '/useremain/home/rinkhals/apps/29-h264-streamer/app.sh start'
sshpass -p 'rockchip' ssh root@$PRINTER_IP '/useremain/home/rinkhals/apps/29-h264-streamer/app.sh status'
```

## Camera Compatibility

The encoder auto-detects USB cameras via `/dev/v4l/by-id/` symlinks. It works with any USB camera that:
- Supports MJPEG output format
- Provides at least 1280x720 resolution

Resolution detection falls back to 1280x720 if v4l2-ctl is not available.

## Known Issues

- `VideoCapture/StartFluency` API method is not registered on some printer models - not needed, camera works after enabling LAN mode and killing gkcam
- `pkill` not available on printer - use rinkhals `kill_by_name` function instead
- `timeout` command not available on printer - removed from commands
- `v4l2-ctl` may not be available - resolution detection has fallback

## Development Notes

### Modifying the Control Page

The control page (`/control`) is served from `control.html` by the C control server. The HTML is loaded as a template with `{{variable}}` substitutions for injecting server state into the page.

**Template Variables:**
The control server replaces `{{variable_name}}` patterns in control.html with current values before serving. See `serve_control_page()` in `control_server.c` for the full list.

**Adding New Settings:**
1. Add HTML input/select in `control.html` inside the settings form
2. Add JavaScript form handler to include the field in POST data
3. Add POST handler parsing in `handle_control_post()` in `control_server.c`
4. Add config field in `config.h` (`AppConfig` struct)
5. Add JSON read/write in `config.c`
6. Apply setting in `on_config_changed()` callback if needed

### Control Server Implementation

The control server (`control_server.c`) handles:
- HTML template loading and variable substitution
- REST API endpoints for stats, config, cameras, timelapse
- Form POST parsing for settings changes
- Timelapse file management (list, serve, delete, thumbnails)
- ACProxyCam FLV proxy coordination
- Touch injection
- Moonraker camera provisioning

### Config File Format

Settings are persisted in `/useremain/home/rinkhals/apps/29-h264-streamer.config` as JSON:
```json
{
  "encoder_type": "rkmpi",
  "h264_enabled": "true",
  "mjpeg_fps": "10",
  "bitrate": "512",
  "auto_skip": "false",
  "target_cpu": "60",
  "autolanmode": "true",
  "display_enabled": "false",
  "display_fps": "5",
  "acproxycam_flv_proxy": "false",
  "timelapse_enabled": "false",
  "timelapse_mode": "layer",
  "moonraker_host": "127.0.0.1",
  "moonraker_port": "7125",
  "cameras": { ... }
}
```

## RV1106 Hardware Constraints

### CPU & Memory
- **CPU:** Single-core ARM Cortex-A7 @ 1.2GHz (ARMv7-A + NEON)
- **RAM:** 256MB DDR3 shared with system (~100-150MB available)
- **Implication:** Limited concurrent processing, memory-constrained

### Video Encoding Limits
| Resolution | Framerate | CPU Usage | Recommended |
|------------|-----------|-----------|-------------|
| 720p | 10-15 fps | 15-25% | **Yes** |
| 720p | 30 fps | 35-45% | Acceptable |
| 1080p | 15 fps | 40-50% | Marginal |
| 1080p | 30 fps | 60%+ | Not recommended |

### Hardware Acceleration
- **VENC:** H.264/H.265 hardware encoder (up to 2304x1296 @ 30fps)
- **VDEC:** H.264/H.265/MJPEG hardware decoder (not used - TurboJPEG simpler)
- **RGA:** 2D graphics accelerator for scaling/conversion

## Performance Tuning

### skip_ratio Setting
Controls frame skipping to reduce CPU/bandwidth:
- `1` = All frames (no skip)
- `2` = Skip every other frame (half framerate)
- `3` = Skip 2 of 3 frames (1/3 framerate)

### auto_skip Setting (CPU-based Dynamic Skip)
When enabled, the encoder automatically adjusts skip_ratio based on CPU usage:
- **target_cpu**: Maximum CPU usage target (default: 60%)
- If CPU > target+5%: increases skip_ratio (fewer frames)
- If CPU < target-10%: decreases skip_ratio (more frames)
- Range: 1 (all frames) to 10 (max skip)

### Recommended Settings
- **Monitoring:** 720p, 10-15 fps, skip_ratio=1, h264_enabled=true
- **Auto-adjust:** auto_skip=true, target_cpu=60 (recommended for most users)
- **Low resource:** 720p, 5 fps, skip_ratio=2, h264_enabled=false

## Display Capture

**Status:** Fully supported in rkmpi/rkmpi-yuyv modes with `--display` flag.

Captures the printer's LCD framebuffer (/dev/fb0) and streams it as MJPEG for remote viewing of the printer's touchscreen.

### Endpoints
- `/display` - MJPEG multipart stream (configurable 1-10 fps)
- `/display/snapshot` - Single JPEG frame

### Features
- **On-demand encoding**: Only captures when clients connected AND enabled (saves CPU)
- **Disabled by default**: Enable via control page to save CPU when not needed
- **Configurable FPS**: 1-10 fps selectable via control page
- **Auto-orientation**: Detects printer model and applies correct rotation
- **Full hardware acceleration**: RGA rotation + RGA color conversion + VENC JPEG encoding

### Printer Model Orientation
| Model | Orientation | RGA Operation |
|-------|-------------|---------------|
| KS1, KS1M | 180° flip | `imflip()` FLIP_H_V |
| K3M | 270° rotation | `imrotate()` ROT_270 |
| K3, K2P, K3V2 | 90° rotation | `imrotate()` ROT_90 |

---

## Touch Control

**Status:** Fully supported - Click on the display stream to inject touch events.

### API Endpoint

**POST `/api/touch`**
```json
{
  "x": 400,
  "y": 240,
  "duration": 100
}
```

### Coordinate Transformation

| Model | Model ID | Display Size | Touch Transform |
|-------|----------|--------------|-----------------|
| KS1, KS1M | 20025, 20029 | 800x480 | `touch = (800-x, 480-y)` |
| K3, K2P, K3V2 | 20024, 20021, 20027 | 480x800 | `touch = (y, 480-x)` |
| K3M | 20026 | 480x800 | `touch = (800-y, x)` |

### Touch Input Device
- **Device:** `/dev/input/event0`
- **Driver:** `hyn_ts` (Hynitron touch controller)
- **Resolution:** 800x480 (native panel)
- **Protocol:** Multi-touch Type B (Linux input subsystem)

Implementation: `touch_inject.c` in rkmpi-encoder.

---

## Timelapse Recording

h264-streamer supports two timelapse recording modes:

| Feature | Native (Anycubic Slicer) | Advanced (Moonraker) |
|---------|--------------------------|----------------------|
| Trigger | Enable in slicer before slicing | Enable in control panel |
| Layer detection | Slicer G-code → RPC commands | Moonraker WebSocket API |
| Hyperlapse mode | No | Yes |
| USB storage | No | Yes |
| Variable FPS | No | Yes |
| Configuration | None needed | Full control |

**Note**: When Advanced Timelapse is enabled, Native timelapse commands are ignored to prevent conflicts.

---

### Native Anycubic Timelapse

The RPC client handles timelapse commands from the Anycubic slicer.

#### How It Works

1. Slicer sends print start with `timelapse.status=1`
2. Firmware sends `openDelayCamera` RPC with gcode filepath
3. RPC client receives command, initializes timelapse recording
4. On each layer change, firmware sends `startLanCapture` RPC
5. rkmpi_enc captures JPEG frame from its internal frame buffer to temp storage
6. On print complete (`print_stats.state == "complete"`), assembles MP4

### RPC Commands Handled

| Method | Handler | Action |
|--------|---------|--------|
| `openDelayCamera` | `timelapse_init()` | Create temp dir, extract gcode name |
| `startLanCapture` | `timelapse_capture_frame()` | Copy JPEG from frame buffer |
| `stopLanCapture` | Reply only | No action needed |
| `SetLed` | Reply only | LED controlled by firmware |

### Output Location

- Videos: `/useremain/app/gk/Time-lapse-Video/`
- Naming: `{gcode_name}_{seq}.mp4`
- Thumbnail: `{gcode_name}_{seq}_{frames}.jpg` (last frame)

### Implementation Files

- `rpc_client.c` - RPC client with timelapse command handlers
- `timelapse.c` / `timelapse.h` - Frame capture and MP4 assembly
- `timelapse_venc.c` / `timelapse_venc.h` - Hardware VENC encoding
- Source: `/shared/dev/anycubic/rkmpi-encoder/`

---

## Advanced Timelapse (Moonraker Integration)

**Status:** Fully supported - Independent timelapse recording via Moonraker WebSocket API, inspired by [moonraker-timelapse](https://github.com/mainsail-crew/moonraker-timelapse).

### Overview

When enabled, the Moonraker WebSocket client (`moonraker_client.c`) connects to Moonraker, subscribes to print status updates, and drives timelapse recording via direct function calls to the timelapse module. No IPC or control files needed — everything runs in-process.

### Features

- **Layer Mode** - Capture frame on each layer change
- **Hyperlapse Mode** - Capture frames at configurable time intervals
- **Variable FPS** - Auto-calculate output FPS based on frame count and target video length
- **USB Storage** - Optional storage to USB drive at `/mnt/udisk`
- **Custom Mode Override** - When enabled, Anycubic RPC timelapse commands are ignored
- **Hardware VENC encoding** - Uses RV1106 hardware encoder for MP4 assembly (no ffmpeg)

### Configuration Settings

All timelapse settings are available in the Timelapse Settings panel on the control page.

#### General Settings

| Setting | Default | Description |
|---------|---------|-------------|
| `timelapse_enabled` | false | Enable advanced timelapse recording |
| `timelapse_mode` | layer | Capture mode: `layer` or `hyperlapse` |
| `timelapse_hyperlapse_interval` | 30 | Seconds between captures in hyperlapse mode (5-300) |

#### Storage Settings

| Setting | Default | Description |
|---------|---------|-------------|
| `timelapse_storage` | internal | Storage location: `internal` or `usb` |
| `timelapse_usb_path` | /mnt/udisk/timelapse | USB output directory |

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
| `timelapse_duplicate_last_frame` | 0 | Repeat final frame N times |

#### Variable FPS Settings

| Setting | Default | Description |
|---------|---------|-------------|
| `timelapse_variable_fps` | false | Auto-calculate FPS based on frame count |
| `timelapse_target_length` | 10 | Target video duration in seconds |
| `timelapse_variable_fps_min` | 5 | Minimum FPS |
| `timelapse_variable_fps_max` | 60 | Maximum FPS |

### API Endpoints

| Endpoint | Method | Description |
|----------|--------|-------------|
| `/api/timelapse/settings` | POST | Save timelapse configuration |
| `/api/timelapse/storage` | GET | Check USB/internal storage status |
| `/api/timelapse/moonraker` | GET | Check Moonraker connection status |

### How It Works

1. **Enable** timelapse in control panel settings
2. **Print start**: MoonrakerClient detects `print_stats.state == "printing"`
3. **Frame capture**:
   - Layer mode: On layer change (via `virtual_sdcard.current_layer` or `print_stats.info.current_layer`)
   - Hyperlapse: At configured interval (timer thread)
4. **Print complete**: Calls `timelapse_finalize()` directly — assembles MP4 via hardware VENC
5. **Print cancel**: Calls `timelapse_cancel()` to clean up

### MoonrakerClient Implementation

The `MoonrakerClient` (`moonraker_client.c`) provides:
- **Minimal WebSocket client** (RFC 6455) — text frames, ping/pong, no compression
- **JSON-RPC 2.0** subscription to `print_stats` + `virtual_sdcard` objects
- **Direct timelapse calls** — `timelapse_init()`, `timelapse_capture_frame()`, `timelapse_finalize()` called in-process
- **Automatic reconnection** with 5s delay on connection loss
- **Custom mode** — `timelapse_set_custom_mode(1)` prevents RPC timelapse conflicts

### Moonraker Status API

**GET `/api/timelapse/moonraker`**
```json
{
  "connected": true,
  "print_state": "printing",
  "current_layer": 42,
  "total_layers": 200,
  "filename": "benchy.gcode",
  "timelapse_active": true,
  "timelapse_frames": 41
}
```

---

## Timelapse Management UI

**Status:** Fully supported - Web interface for browsing, previewing, downloading, and deleting timelapse recordings.

### Access

- **URL:** `http://<printer-ip>:8081/timelapse`
- **Control Page:** Click "Time Lapse" button on `/control` page

### Features

- **Recording List** - Shows all MP4 recordings with thumbnails
- **Storage Selection** - Switch between internal and USB storage locations
- **Auto Thumbnail Generation** - Creates thumbnails from videos using ffprobe/ffmpeg
- **Metadata Display** - Duration, file size, frame count, creation date
- **Video Preview** - Play videos in browser modal (HTML5 video player)
- **Download** - Download MP4 files directly
- **Delete** - Remove recordings with confirmation dialog
- **Sorting** - Sort by date (newest/oldest), name, or size

### API Endpoints

| Endpoint | Method | Description |
|----------|--------|-------------|
| `/timelapse` | GET | Timelapse manager HTML page |
| `/api/timelapse/list?storage=internal\|usb` | GET | JSON list of recordings with metadata |
| `/api/timelapse/thumb/<name>?storage=...` | GET | Serve thumbnail JPEG |
| `/api/timelapse/video/<name>?storage=...` | GET | Serve MP4 video (supports HTTP Range) |
| `/api/timelapse/delete/<name>?storage=...` | DELETE | Delete recording and thumbnail |

### File Storage

| Storage | Path |
|---------|------|
| Internal | `/useremain/app/gk/Time-lapse-Video/` |
| USB | `/mnt/udisk/Time-lapse-Video/` |

### Security

- **Path Traversal Protection** - Filenames are sanitized to prevent `../` attacks
- **Directory Restriction** - Only files within timelapse directories can be accessed
- **Extension Validation** - Only `.mp4` and `.jpg` files are served
