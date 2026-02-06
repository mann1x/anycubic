# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

---

## h264-streamer

### [1.6.3] - 2026-02-06

#### Fixed
- WiFi route fix now runs in the IP monitor loop (every 30s) instead of at startup, handling late wlan0 DHCP and re-checking on IP changes

### [1.6.2] - 2026-02-06

#### Fixed
- High WiFi CPU usage when eth1 and wlan0 are on the same subnet — MJPEG stream traffic was routed through WiFi instead of USB ethernet due to wlan0 routes having equal priority; startup now sets wlan0 route metric to 100 so eth1 is preferred with automatic WiFi fallback

### [1.6.1] - 2026-02-06

#### Fixed
- Control page ACProxyCam FLV Proxy checkbox not being sent to server (JavaScript form handler missing the field)
- Control page POST body truncation for large forms (read full Content-Length)
- Removed unnecessary startCapture keepalive flooding from MQTT responder subprocess

### [1.6.0] - 2026-02-06

#### Added
- ACProxyCam FLV proxy mode - offloads H.264 encoding from the printer to ACProxyCam running on a more powerful host
- `POST /api/acproxycam/flv` endpoint for ACProxyCam to announce its FLV URL
- `GET /api/acproxycam/flv` endpoint for proxy status monitoring
- Control page checkbox to enable/disable ACProxyCam FLV proxy (rkmpi mode only)
- Transparent per-client FLV byte proxy from ACProxyCam to slicer connections
- Automatic `--no-flv` flag when proxy is enabled (skips VENC initialization on printer)
- FLV proxy client connection tracking with upstream error handling

#### Fixed
- MQTT/RPC responders now start immediately with proxy server, preventing slicer timeout on initial `startCapture`
- MQTT PINGREQ keepalive in standalone responder prevents broker disconnection
- Periodic `startCapture` keepalive prevents firmware from stopping the camera stream

#### Changed
- Refactored `_serve_flv_stream` into dispatch + local/proxied methods
- Extracted RPC/MQTT responder subprocess management into reusable helpers

### [1.5.0] - 2025-02-05

#### Added
- Multi-camera support for up to 4 USB cameras with individual settings
- Moonraker Camera Settings panel for per-camera provisioning with custom names
- Dynamic resolution detection via V4L2 ioctls
- Per-camera V4L2 controls (brightness, contrast, etc.)
- Camera selector UI in control page
- New API endpoints: `/api/cameras`, `/api/camera/enable`, `/api/camera/disable`, `/api/camera/settings`, `/api/moonraker/cameras`
- Restart command in app.sh for cleaner encoder restarts

#### Changed
- Improved CAM#2 enable/disable toggle button styling (ON/OFF text)
- Moonraker provisioning now uses saved per-camera settings

### [1.4.0] - 2025-02-04

#### Added
- USB camera V4L2 controls (brightness, contrast, exposure, etc.)
- Camera Controls panel with real-time adjustment
- Hardware VENC timelapse encoding (no ffmpeg dependency)
- Timelapse first layer detection improvements

#### Fixed
- Timelapse capture timing for better first/last frames
- FLV headers not sent when H.264 already running
- Return 503 for /flv when H.264 encoding is disabled

### [1.3.0] - 2025-02-03

#### Added
- `/api/config` endpoint for external tool integration (ACProxyCam)
- Camera max FPS auto-detection from encoder
- Dynamic MJPEG FPS slider that adjusts to detected camera capabilities
- Snapshot auto-refresh when switching to snapshot tab

#### Changed
- Renamed "H.264 Resolution" to "Camera Resolution" - now applies to both rkmpi and rkmpi-yuyv modes
- Camera resolution setting now controls actual capture resolution in rkmpi-yuyv mode

#### Fixed
- h264_enabled setting now persists across restarts and encoder type changes
- display_enabled setting now persists when changing encoder type
- MJPEG FPS slider validation when camera max FPS is detected lower than default

### [1.2.0] - 2025-01-28

#### Added
- Advanced timelapse recording with Moonraker integration
- Layer-based and hyperlapse timelapse modes
- Timelapse management UI - browse, preview, download, delete recordings
- Variable FPS timelapse output for consistent video length
- USB storage support for timelapse recordings

#### Fixed
- MQTT keepalive to prevent broker disconnections during long prints

### [1.1.0] - 2025-01-20

#### Added
- Timelapse management UI for browsing and downloading recordings
- Operating mode documentation (go-klipper vs vanilla-klipper)
- Touch control for remote display interaction

#### Fixed
- Logging setting not loaded from saved config
- Hardcoded ports issue
- Config persistence with fsync

---

## rkmpi-encoder

### [1.4.0] - 2025-02-05

#### Added
- Multi-camera support with `--no-flv` flag for secondary cameras
- Per-camera command/control files via `--cmd-file` and `--ctrl-file` options
- USB camera V4L2 controls support (brightness, contrast, exposure, etc.)
- JPEG frame validation to prevent white/corrupt frames

#### Fixed
- VENC channel conflicts when multiple encoders run simultaneously
- Timelapse VENC encoding bugs and buffer overflow for 4:2:2 JPEG

### [1.3.0] - 2025-02-04

#### Added
- Hardware VENC timelapse encoding with minimp4 muxer
- Deferred timelapse encoding and unified command file architecture
- Pre-validation of JPEG header before decompression

#### Fixed
- VENC channel conflict between display and timelapse
- Compiler warnings in timelapse_venc.c

### [1.2.0] - 2025-02-03

#### Added
- Snapshot support when camera is idle (wakes camera for snapshot requests)
- YUYV idle mode - skips capture when no clients connected
- Camera max FPS reporting in ctrl file for h264_server.py
- Support for custom camera resolution in YUYV mode via -w/-h flags

#### Changed
- Stats logging now uses interval FPS (matches UI display) instead of total average FPS

#### Fixed
- Snapshot requests now work correctly when no streaming clients are connected
- Rate limiting now always applies in MJPEG mode

### [1.1.0] - 2025-01-20

#### Added
- Display capture with full hardware acceleration (RGA + VENC)
- Timelapse recording with automatic MP4 assembly
- MQTT keepalive (automatic PINGREQ)
- YUYV capture mode with hardware JPEG encoding
- Adaptive frame rate detection
- Connection warmup throttling to prevent CPU spikes

#### Fixed
- USB buffer optimization (increased to 5 buffers)

---

## fb-status

### [1.0.0] - 2025-01-15

#### Added
- Initial release
- Direct framebuffer rendering with TTF fonts
- Screen save/restore via ffmpeg
- Pipe mode for long-running scripts
- Auto screen orientation detection per printer model
- Printer busy/free API integration
