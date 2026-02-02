# h264-streamer App

USB camera streaming app for Rinkhals with hardware H.264 encoding using the RV1106 VENC.

---

## ⛔ CRITICAL: NEVER KILL PYTHON PROCESSES ⛔

```
██████████████████████████████████████████████████████████████████████████████
██                                                                          ██
██   NEVER USE: killall python                                              ██
██   NEVER USE: pkill python                                                ██
██   NEVER USE: pkill -f python                                             ██
██                                                                          ██
██   This DESTROYS the printer! Moonraker, Klipper, and other critical      ██
██   services run as Python processes. Killing all Python = BRICK.          ██
██                                                                          ██
██   ALWAYS USE app.sh FOR PROCESS MANAGEMENT:                              ██
██                                                                          ██
██   # Stop h264-streamer:                                                  ██
██   /useremain/home/rinkhals/apps/29-h264-streamer/app.sh stop             ██
██                                                                          ██
██   # Start h264-streamer:                                                 ██
██   /useremain/home/rinkhals/apps/29-h264-streamer/app.sh start            ██
██                                                                          ██
██   If you need to kill h264_server.py specifically:                       ██
██   pkill -f h264_server.py   (NOT just "python"!)                         ██
██                                                                          ██
██████████████████████████████████████████████████████████████████████████████
```

---

## Overview

This app provides HTTP endpoints for streaming video from a USB camera:

**Main server (port 8080, configurable via `port` property):**
- `/stream` - MJPEG multipart stream (USB camera)
- `/snapshot` - Single JPEG frame (USB camera)
- `/display` - MJPEG stream of printer LCD framebuffer (5 fps)
- `/display/snapshot` - Single JPEG of printer LCD
- `/control` - Web UI with settings, FPS/CPU monitoring, and live stream preview
- `/status` - JSON status
- `/api/stats` - JSON stats (FPS, CPU, settings) with 1s refresh

**FLV server (port 18088, fixed for Anycubic slicer):**
- `/flv` - H.264 in FLV container

## Architecture

**rkmpi mode (MJPEG capture):**
```
USB Camera (MJPEG) → rkmpi_enc → stdout (MJPEG multipart) → h264_server.py → HTTP
                         ↓
                    TurboJPEG decode → NV12 → VENC → H.264 FIFO
```

**rkmpi-yuyv mode (YUYV capture with HW JPEG):**
```
USB Camera (YUYV) → rkmpi_enc → YUYV→NV12 (CPU) → VENC Ch0 → H.264 FIFO
                                       ↓
                                   VENC Ch1 → JPEG → stdout → h264_server.py → HTTP
```

## Components

### h264_server.py
Python HTTP server that:
- Manages LAN mode via local binary API (port 18086)
- Kills gkcam when LAN mode is enabled (to free the camera)
- Spawns rkmpi_enc subprocess
- Reads MJPEG from encoder stdout and H.264 from FIFO
- Provides FLV muxing for H.264 streams
- Monitors CPU usage

### rkmpi_enc
Native binary encoder built with RV1106 SDK:
- V4L2 capture from USB camera (MJPEG or YUYV)
- In MJPEG mode: TurboJPEG software JPEG decoding
- In YUYV mode: CPU YUYV→NV12 conversion + hardware JPEG encoding (VENC Ch1)
- RKMPI VENC hardware H.264 encoding (VENC Ch0)
- Outputs MJPEG to stdout and H.264 to FIFO

Source: `/shared/dev/anycubic/rkmpi-encoder/`

### app.sh
App lifecycle management:
- Reads `autolanmode` property from app.json
- Sets up LD_LIBRARY_PATH for RKMPI libs
- Manages process lifecycle

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

The `/control` endpoint (on port 8081) provides a web UI with:
- **Tab-based preview**: Snapshot, MJPEG Stream, H.264 Stream, Display Stream
- **H.264 player**: Uses flv.js from CDN for live FLV playback in browser
- **Display preview**: Shows printer LCD framebuffer (5 fps MJPEG)
- **Performance stats**: Real-time MJPEG FPS, H.264 FPS, Total CPU, Encoder CPU
- **Settings**:
  - Auto LAN mode toggle
  - H.264 encoding toggle
  - **Frame Rate slider**: 0-100% with text input (100%=all frames, 0%=~1fps)
  - **Auto Skip toggle**: CPU-based dynamic frame rate adjustment
  - **Target CPU %**: Maximum CPU target when auto-skip enabled (30-90%)

### Port Layout
- **Port 8080**: Streaming endpoints (`/stream`, `/snapshot`, `/display`, `/display/snapshot`)
- **Port 8081**: Control page (`/control`, `/api/*`)
- **Port 18088**: FLV H.264 stream (`/flv`)

### API Stats Response (`/api/stats`)
```json
{
  "cpu": {"total": 47.4, "processes": {"2673": 18.6, "2591": 11.3}},
  "fps": {"mjpeg": 10.0, "h264": 5.0},
  "h264_enabled": true,
  "skip_ratio": 2,
  "auto_skip": true,
  "target_cpu": 60,
  "autolanmode": true
}
```

## Operating Modes

The `--mode` flag controls how h264-streamer integrates with the printer's firmware.

### go-klipper (default)

For Anycubic printers running **Rinkhals custom firmware**. This is the standard configuration.

**Features:**
- **LAN Mode Management** - Queries and auto-enables LAN mode via local binary API (port 18086)
- **gkcam Integration** - Kills gkcam to free the USB camera, restarts it on shutdown
- **MQTT/RPC Responders** - Handles timelapse commands from gkapi (openDelayCamera, startLanCapture, etc.)
- **Camera Stream Control** - Sends startCapture/stopCapture via MQTT to manage gkcam state
- **Full Firmware Integration** - Works alongside Anycubic's native services

**Use when:**
- Running on an Anycubic printer with Rinkhals
- You need timelapse recording support
- You want the camera to work with the Anycubic slicer

### vanilla-klipper

For setups using **external Klipper** (e.g., on a Raspberry Pi) connected to the printer via USB, or for testing without Anycubic firmware dependencies.

**Features:**
- **No Firmware Integration** - Skips all Anycubic-specific initialization
- **No LAN Mode** - Does not query or enable LAN mode
- **No gkcam Management** - Does not kill or restart gkcam
- **No MQTT/RPC** - Disables MQTT client and RPC responders
- **Pure Camera Streaming** - Just captures from USB camera and serves HTTP streams

**Use when:**
- Using an external Raspberry Pi running Klipper
- The printer's native firmware is not involved
- You only need basic camera streaming without timelapse
- Testing the encoder in isolation

### Mode Comparison

| Feature | go-klipper | vanilla-klipper |
|---------|------------|-----------------|
| LAN mode management | ✅ | ❌ |
| gkcam kill/restart | ✅ | ❌ |
| MQTT/RPC responders | ✅ | ❌ |
| Timelapse recording | ✅ | ❌ |
| Anycubic slicer support | ✅ | ❌ |
| Works without firmware | ❌ | ✅ |
| External Klipper | ❌ | ✅ |

### Setting the Mode

**Via app.json property:**
```json
{
    "mode": "vanilla-klipper"
}
```

**Via command line:**
```bash
python h264_server.py --mode vanilla-klipper
```

---

## Properties (app.json)

- `mode` (string, default: "go-klipper") - Operating mode: "go-klipper" or "vanilla-klipper"
- `encoder_type` (string, default: "rkmpi-yuyv") - Encoder mode: "gkcam", "rkmpi", or "rkmpi-yuyv"
- `gkcam_all_frames` (bool, default: false) - In gkcam mode, decode all frames (true) or keyframes only (false)
- `autolanmode` (bool, default: true) - Automatically enable LAN mode on startup (go-klipper only)
- `auto_skip` (bool, default: false) - Enable automatic skip ratio based on CPU usage (rkmpi only)
- `target_cpu` (int, default: 60) - Target max CPU usage % for auto-skip (30-90, rkmpi only)
- `bitrate` (int, default: 512) - H.264 bitrate in kbps (100-4000, rkmpi only)
- `mjpeg_fps` (int, default: 10) - MJPEG camera framerate (2-30, rkmpi only). Actual rate depends on camera support.
- `jpeg_quality` (int, default: 85) - JPEG quality for hardware encoding (1-99, rkmpi-yuyv only)
- `port` (int, default: 8080) - Main HTTP server port (MJPEG, snapshots, control)
- `logging` (bool, default: false) - Enable debug logging

**Note:** The FLV endpoint is always on port 18088:
- In **gkcam mode**: served by gkcam natively
- In **rkmpi/rkmpi-yuyv mode**: served by h264-streamer

## Encoder Modes

### gkcam mode (default)
- Uses gkcam's built-in FLV stream at `:18088/flv`
- Lower CPU usage (no encoding required)
- Does NOT kill gkcam

**Two sub-modes controlled by `gkcam_all_frames`:**

1. **Keyframes only** (`gkcam_all_frames=false`, default):
   - Decodes only H.264 keyframes (IDR) via ffmpeg single-frame decode
   - ~1 FPS MJPEG output (keyframes typically arrive at 1/sec)
   - Lowest CPU usage

2. **All frames** (`gkcam_all_frames=true`):
   - Uses streaming ffmpeg transcoder to decode all frames
   - ~2 FPS MJPEG output (limited by transcoding overhead)
   - Higher CPU usage but smoother video

### rkmpi mode
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

SWU packages are built via the Rinkhals build system.

### Build Scripts Location
- **Local build script:** `/shared/dev/Rinkhals/build/swu-tools/h264-streamer/build-local.sh`
- **Build tools:** `/shared/dev/Rinkhals/build/tools.sh`

### Build All Models (Local)
```bash
/shared/dev/Rinkhals/build/swu-tools/h264-streamer/build-local.sh
```

This builds SWU packages for all Kobra models (K2P, K3, K3V2, KS1, KS1M, K3M) to `/shared/dev/Rinkhals/build/dist/`.

### Build Single Model
```bash
/shared/dev/Rinkhals/build/swu-tools/h264-streamer/build-local.sh KS1
```

### SWU Passwords by Model (Reference)
Handled automatically by build scripts via `build/tools.sh`:
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
- App install path: `/useremain/home/rinkhals/apps/29-h264-streamer/` (SWU packages install apps here)
- RKMPI libraries: `/oem/usr/lib/`
- FIFO pipes: `/tmp/mjpeg.pipe`, `/tmp/h264.pipe`

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

# Copy Python files (if changed)
sshpass -p 'rockchip' scp /shared/dev/anycubic/h264-streamer/29-h264-streamer/h264_server.py root@$PRINTER_IP:/useremain/home/rinkhals/apps/29-h264-streamer/

# Start app
sshpass -p 'rockchip' ssh root@$PRINTER_IP '/useremain/home/rinkhals/apps/29-h264-streamer/app.sh start'

# Test endpoints
curl http://$PRINTER_IP:8080/status
curl http://$PRINTER_IP:8080/snapshot -o snapshot.jpg
curl http://$PRINTER_IP:8080/display/snapshot -o display.jpg
```

### Check logs
```bash
# Main application log (h264_server.py + encoder output)
sshpass -p 'rockchip' ssh root@$PRINTER_IP 'tail -50 /tmp/rinkhals/app-h264-streamer.log'

# Encoder-only log (raw stderr from rkmpi_enc)
sshpass -p 'rockchip' ssh root@$PRINTER_IP 'cat /tmp/rkmpi.log'
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

The control page (`/control`) uses JavaScript to handle form submission. **Important:** When adding new form fields, you must update TWO places:

1. **HTML form** - Add the input/select element inside the `<form id="settings-form">` tag
2. **JavaScript submit handler** - Add the field to the form data builder

The JavaScript form handler is around line 4163 in `h264_server.py`:

```javascript
document.getElementById('settings-form').addEventListener('submit', function(e) {
    e.preventDefault();
    const formData = new FormData(this);
    const data = new URLSearchParams();

    // Each field must be explicitly added:
    data.append('field_name', document.querySelector('[name=field_name]').value);

    // For checkboxes:
    if (formData.has('checkbox_name')) {
        data.append('checkbox_name', 'on');
    }
    // ...
});
```

**Common mistake:** Adding a form field to the HTML but forgetting to add it to the JavaScript handler. The form uses `e.preventDefault()` and manually builds the POST data, so fields not explicitly added will NOT be submitted.

**Checklist for new control page settings:**
1. Add HTML input/select inside `<form id="settings-form">`
2. Add `data.append()` call in JavaScript submit handler
3. Add POST handler parsing in `_handle_control_post()` method
4. Add to `write_ctrl_file()` if encoder needs to read it
5. Add to `save_config()` for persistence
6. Add to config loading in `main()` for startup

### Process Management
**⛔ See CRITICAL WARNING at top of this file! ⛔**

Use app.sh for all process management:
```bash
sshpass -p 'rockchip' ssh root@$PRINTER_IP '/useremain/home/rinkhals/apps/29-h264-streamer/app.sh stop'
sshpass -p 'rockchip' ssh root@$PRINTER_IP '/useremain/home/rinkhals/apps/29-h264-streamer/app.sh start'
sshpass -p 'rockchip' ssh root@$PRINTER_IP '/useremain/home/rinkhals/apps/29-h264-streamer/app.sh status'
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

### Why Software MJPEG Decode?
Hardware MJPEG decode (VDEC) requires complex buffer management. At 720p15, TurboJPEG software decode uses only ~15% CPU and is much simpler to implement.

## USB Camera Constraints

### Bandwidth
- USB 2.0: 480 Mbps theoretical, ~35-40 MB/s practical
- MJPEG 720p30: ~3-8 MB/s (fits comfortably)
- Raw YUYV 720p30: ~27 MB/s (too high, don't use)

### Camera Requirements
- UVC (USB Video Class) compliant
- **Must support MJPEG output** (not just raw YUV)
- Common compatible: Logitech C270/C920, generic HD webcams

### Resolution/Format Support
- **Preferred:** 1280x720 MJPEG @ 15fps
- **Maximum practical:** 1920x1080 MJPEG @ 15fps
- **Fallback:** 1280x720 if detection fails

### Known Camera Behaviors
- Some cameras report capabilities they can't sustain at full framerate
- Dark scenes = larger MJPEG frames (less compression)
- Cheap cameras may have unstable USB connections
- Camera must be released by gkcam before h264-streamer can use it

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

This ensures the printer maintains at least 40% CPU headroom for other tasks.

### h264_enabled Setting
- `true` = Full H.264 encoding (higher CPU, lower bandwidth)
- `false` = MJPEG only (lower CPU, higher bandwidth)

### Recommended Settings
- **Monitoring:** 720p, 10-15 fps, skip_ratio=1, h264_enabled=true
- **Auto-adjust:** auto_skip=true, target_cpu=60 (recommended for most users)
- **Timelapse:** 720p, 1 fps, skip_ratio=0, h264_enabled=false
- **Low resource:** 720p, 5 fps, skip_ratio=2, h264_enabled=false

### Auto-Skip Test Results

| Target CPU | Skip Ratio | Total CPU | Enc CPU | H.264 FPS | Status |
|------------|------------|-----------|---------|-----------|--------|
| 30% | 5 | 36.4% | 9.5% | 3.8 | LOW* |
| 40% | 4 | 33.9% | 10.7% | 2.3 | OK |
| 50% | 3 | 44.2% | 11.5% | 2.6 | OK |
| 60% | 1 | 60.0% | 33.5% | 6.7 | OK |
| 70% | 2 | 62.4% | 37.1% | 10.0 | OK |
| 80% | 2 | 62.8% | 38.2% | 10.0 | OK |

*30% target is below encoder baseline (~35% CPU minimum)

## Display Capture

**Status:** ✅ **Fully supported** in rkmpi/rkmpi-yuyv modes with `--display` flag.

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
- **CPU usage**: ~0-10% when active (full hardware pipeline)

### How It Works
1. Framebuffer is memory-mapped from /dev/fb0 (800×480 BGRX, 32bpp)
2. Copied to DMA buffer (framebuffer mmap is slow/uncached)
3. RGA hardware rotation/flip based on printer model
4. RGA hardware color conversion BGRX → NV12
5. VENC hardware encodes to JPEG
6. Served via HTTP on `/display` endpoint

### Printer Model Orientation
| Model | Orientation | RGA Operation |
|-------|-------------|---------------|
| KS1, KS1M | 180° flip | `imflip()` FLIP_H_V |
| K3M | 270° rotation | `imrotate()` ROT_270 |
| K3, K2P, K3V2 | 90° rotation | `imrotate()` ROT_90 |

### Control Page Settings
- **Display Capture toggle**: Enable/disable display streaming
- **Display FPS**: 1, 2, 5, or 10 fps (lower = less CPU)

---

## Touch Control

**Status:** ✅ **Fully supported** - Click on the display stream to inject touch events.

Remote touch control allows interacting with the printer's touchscreen through the web interface. Click anywhere on the display stream image to send a touch event to the printer.

### How It Works
1. User clicks on the display stream image in the web browser
2. Browser calculates click position relative to display dimensions
3. POST request sent to `/api/touch` with coordinates
4. Server transforms coordinates based on printer model orientation
5. Touch event injected to `/dev/input/event0` (Linux input subsystem)
6. Printer UI responds to the touch

### API Endpoint

**POST `/api/touch`**
```json
{
  "x": 400,
  "y": 240,
  "duration": 100
}
```

Response:
```json
{
  "status": "ok",
  "x": 400,
  "y": 240,
  "touch_x": 400,
  "touch_y": 240,
  "duration": 100
}
```

### Coordinate Transformation

The touch panel is aligned with the native framebuffer (800×480), but the web display shows a rotated/flipped image depending on the printer model. The server automatically detects the model and applies the correct inverse transformation.

| Model | Model ID | Display Size | Touch Transform |
|-------|----------|--------------|-----------------|
| KS1, KS1M | 20025, 20029 | 800×480 | `touch = (800-x, 480-y)` |
| K3, K2P, K3V2 | 20024, 20021, 20027 | 480×800 | `touch = (y, 480-x)` |
| K3M | 20026 | 480×800 | `touch = (800-y, x)` |

### Touch Input Device
- **Device:** `/dev/input/event0`
- **Driver:** `hyn_ts` (Hynitron touch controller)
- **Resolution:** 800×480 (native panel)
- **Protocol:** Multi-touch Type B (Linux input subsystem)

### Implementation
- Touch injection: `inject_touch()` in `h264_server.py`
- Coordinate transform: `transform_touch_coordinates()` in `h264_server.py`
- Model detection: Reads `modelId` from `/userdata/app/gk/config/api.cfg`

---

## Timelapse Recording

**Status:** ✅ **Fully supported** in rkmpi/rkmpi-yuyv modes. The native `rkmpi_enc` encoder includes a built-in RPC client that handles timelapse commands.

### How It Works

1. Slicer sends print start with `timelapse.status=1`
2. Firmware sends `openDelayCamera` RPC with gcode filepath
3. `rkmpi_enc` RPC client receives command, initializes timelapse recording
4. On each layer change, firmware sends `startLanCapture` RPC
5. `rkmpi_enc` captures JPEG frame from its internal frame buffer to temp storage
6. On print complete (`print_stats.state == "complete"`), `rkmpi_enc` runs ffmpeg to assemble MP4

### RPC Commands Handled

| Method | Handler | Action |
|--------|---------|--------|
| `openDelayCamera` | `timelapse_init()` | Create temp dir, extract gcode name |
| `startLanCapture` | `timelapse_capture_frame()` | Copy JPEG from frame buffer |
| `stopLanCapture` | Reply only | No action needed |
| `SetLed` | Reply only | LED controlled by firmware |

### Print Completion Detection

The RPC client monitors `print_stats.state` in status updates:
- `"complete"` → `timelapse_finalize()` - Assemble MP4 with ffmpeg
- `"cancelled"` or `"error"` → `timelapse_cancel()` - Clean up temp files

### Output Location

- Videos: `/useremain/app/gk/Time-lapse-Video/`
- Naming: `{gcode_name}_{seq}.mp4` (e.g., `Benchy_PLA_0.2_1h_01.mp4`)
- Thumbnail: `{gcode_name}_{seq}_{frames}.jpg` (last frame)

### Implementation Files

- `rpc_client.c` - RPC client with timelapse command handlers
- `timelapse.c` / `timelapse.h` - Frame capture and MP4 assembly
- Source: `/shared/dev/anycubic/rkmpi-encoder/`

### Reference Data

- Protocol doc: `/shared/dev/anycubic/knowledge/TIMELAPSE_PROTOCOL.md`
- Timelapse captures: `/shared/dev/Rinkhals/docs/timelapse-captures/`

---

## Advanced Timelapse (Moonraker Integration)

**Status:** ✅ **Fully supported** - Independent timelapse recording via Moonraker WebSocket API, inspired by [moonraker-timelapse](https://github.com/mainsail-crew/moonraker-timelapse).

### Overview

When enabled, this mode captures timelapse frames independently of the Anycubic slicer's timelapse settings. It monitors print progress via Moonraker's WebSocket API and captures frames on layer changes or at fixed time intervals.

### Features

- **Layer Mode** - Capture frame on each layer change (`print_stats.info.current_layer`)
- **Hyperlapse Mode** - Capture frames at configurable time intervals
- **Variable FPS** - Auto-calculate output FPS based on frame count and target video length
- **USB Storage** - Optional storage to USB drive at `/mnt/udisk`
- **Custom Mode Override** - When enabled, Anycubic RPC timelapse commands are ignored

### Configuration Settings

| Setting | Default | Description |
|---------|---------|-------------|
| `timelapse_enabled` | false | Enable advanced timelapse |
| `timelapse_mode` | layer | `layer` or `hyperlapse` |
| `timelapse_hyperlapse_interval` | 30 | Seconds between captures (hyperlapse mode) |
| `timelapse_storage` | internal | `internal` or `usb` |
| `moonraker_host` | 127.0.0.1 | Moonraker server IP |
| `moonraker_port` | 7125 | Moonraker WebSocket port |
| `timelapse_output_fps` | 30 | Video playback framerate |
| `timelapse_variable_fps` | false | Auto-adjust FPS for target length |
| `timelapse_target_length` | 10 | Target video length in seconds |
| `timelapse_crf` | 23 | H.264 quality (0=best, 51=worst) |
| `timelapse_flip_x` | false | Horizontal flip |
| `timelapse_flip_y` | false | Vertical flip |

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
   - Layer mode: On `print_stats.info.current_layer` change
   - Hyperlapse: At configured interval (timer thread)
4. **Print complete**: Sends `timelapse_finalize` command to encoder
5. **Video assembly**: ffmpeg assembles frames with configured FPS, CRF, and flip settings
6. **Print cancel**: Sends `timelapse_cancel` to discard temporary frames

### MoonrakerClient Implementation

The `MoonrakerClient` class (`h264_server.py`) provides:
- **WebSocket connection** to Moonraker (pure Python, no external dependencies)
- **JSON-RPC 2.0** message handling
- **Subscription** to `print_stats` object for state monitoring
- **Automatic reconnection** on connection loss

### Control File Commands

h264_server.py communicates with rkmpi_enc via the control file:

| Command | Description |
|---------|-------------|
| `timelapse_init:<gcode>:<output_dir>` | Initialize custom timelapse |
| `timelapse_capture` | Capture single frame |
| `timelapse_finalize` | Assemble video from frames |
| `timelapse_cancel` | Discard frames, cleanup |
| `timelapse_fps:<n>` | Set output FPS |
| `timelapse_crf:<n>` | Set H.264 quality |
| `timelapse_variable_fps:<0\|1>:<min>:<max>:<target>:<dup>` | Configure variable FPS |
| `timelapse_flip:<flip_x>:<flip_y>` | Set flip options |
| `timelapse_output_dir:<path>` | Set output directory |
| `timelapse_custom_mode:<0\|1>` | Enable/disable custom mode (ignores RPC commands) |

### Custom Mode Flag

When `timelapse_custom_mode:1` is sent to the encoder:
- `openDelayCamera` RPC commands are acknowledged but ignored
- `startLanCapture` RPC commands are acknowledged but do not capture frames
- Print completion detection via RPC is disabled
- h264_server.py handles all timelapse logic via Moonraker

This allows the advanced timelapse to operate independently without interference from the Anycubic slicer's built-in timelapse commands.

### Notes

- Requires rkmpi encoder mode (not gkcam) for frame capture
- USB storage requires a mounted USB drive at `/mnt/udisk`
- Variable FPS calculates: `fps = max(min_fps, min(max_fps, frames / target_length))`
- Flip options use ffmpeg video filters: `hflip`, `vflip`, or `hflip,vflip`

---

## Timelapse Management UI

**Status:** ✅ **Fully supported** - Web interface for browsing, previewing, downloading, and deleting timelapse recordings.

### Access

- **URL:** `http://<printer-ip>:8081/timelapse`
- **Control Page:** Click "Time Lapse" button on `/control` page

### Features

- **Recording List** - Shows all MP4 recordings with thumbnails
- **Metadata Display** - Duration, file size, frame count for each recording
- **Video Preview** - Play videos in browser modal (HTML5 video player)
- **Download** - Download MP4 files directly
- **Delete** - Remove recordings with confirmation dialog
- **Sorting** - Sort by date (newest/oldest), name, or size
- **Summary** - Total recording count and storage used

### API Endpoints

| Endpoint | Method | Description |
|----------|--------|-------------|
| `/timelapse` | GET | Timelapse manager HTML page |
| `/api/timelapse/list` | GET | JSON list of recordings with metadata |
| `/api/timelapse/thumb/<name>` | GET | Serve thumbnail JPEG |
| `/api/timelapse/video/<name>` | GET | Serve MP4 video (supports HTTP Range) |
| `/api/timelapse/delete/<name>` | DELETE | Delete recording and thumbnail |

### List API Response

**GET `/api/timelapse/list`**
```json
{
  "recordings": [
    {
      "name": "Benchy_PLA_0.2_1h_01",
      "mp4": "Benchy_PLA_0.2_1h_01.mp4",
      "thumbnail": "Benchy_PLA_0.2_1h_01_126.jpg",
      "size": 2200000,
      "frames": 126,
      "duration": 12.6,
      "mtime": 1706889600
    }
  ],
  "total_size": 12900000,
  "count": 5
}
```

### Delete API Response

**DELETE `/api/timelapse/delete/<name>`**
```json
{
  "success": true,
  "deleted": ["Benchy_PLA_0.2_1h_01.mp4", "Benchy_PLA_0.2_1h_01_126.jpg"]
}
```

### File Storage

| Item | Path |
|------|------|
| Directory | `/useremain/app/gk/Time-lapse-Video/` |
| Video | `{gcode_name}_{sequence}.mp4` |
| Thumbnail | `{gcode_name}_{sequence}_{frames}.jpg` |

### Security

- **Path Traversal Protection** - Filenames are sanitized to prevent `../` attacks
- **Directory Restriction** - Only files within `TIMELAPSE_DIR` can be accessed
- **Extension Validation** - Only `.mp4` and `.jpg` files are served

### Implementation

- **Constants:** `TIMELAPSE_DIR`, `TIMELAPSE_FPS` (10 fps for duration calculation)
- **Methods:**
  - `_serve_timelapse_page()` - Serves HTML/CSS/JS page
  - `_serve_timelapse_list()` - Scans directory, returns JSON
  - `_serve_timelapse_thumb()` - Serves JPEG thumbnails
  - `_serve_timelapse_video()` - Serves MP4 with Range support for seeking
  - `_handle_timelapse_delete()` - Deletes MP4 and matching thumbnail
