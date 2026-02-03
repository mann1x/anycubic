# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

---

## h264-streamer

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
