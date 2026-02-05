# Knowledge Base

Protocol documentation, research notes, and technical findings for Anycubic RV1106 printers.

## Contents

```
knowledge/
├── TIMELAPSE_PROTOCOL.md      # Timelapse RPC/MQTT protocol
├── TIMELAPSE_IMPLEMENTATION_PLAN.md  # Implementation design
├── USB_CAMERA_BANDWIDTH.md    # USB bandwidth analysis
├── USB_CAMERA_CONTROLS.md     # V4L2 camera controls reference
├── MJPEG_CPU_OPTIMIZATION.md  # CPU optimization techniques
└── DISPLAY_STANDBY.md         # Display wake/sleep control
```

## Document Summaries

### [TIMELAPSE_PROTOCOL.md](TIMELAPSE_PROTOCOL.md)

Complete protocol documentation for timelapse recording based on gkcam packet captures.

**Topics covered:**
- RPC message format (ETX-delimited JSON on port 18086)
- Video stream request/reply patterns
- MQTT topics and commands (port 9883 TLS)
- Frame capture triggers (`startLanCapture`)
- Print completion detection
- Output file naming conventions

### [TIMELAPSE_IMPLEMENTATION_PLAN.md](TIMELAPSE_IMPLEMENTATION_PLAN.md)

Design document for implementing timelapse in rkmpi_enc.

**Topics covered:**
- Architecture decision (encoder vs Python vs hybrid)
- RPC handler extensions
- Frame capture logic
- MP4 assembly with ffmpeg
- Phase-by-phase implementation plan

### [USB_CAMERA_BANDWIDTH.md](USB_CAMERA_BANDWIDTH.md)

Investigation into USB bandwidth constraints for camera capture.

**Key findings:**
- USB 2.0 isochronous limit: 24 MB/s
- YUYV 720p limited to ~4 fps (1.8 MB/frame)
- MJPEG 720p can achieve 30+ fps (50-150 KB/frame)
- MMAP is optimal V4L2 access method
- USERPTR not supported on ARM dma_contig allocator

### [MJPEG_CPU_OPTIMIZATION.md](MJPEG_CPU_OPTIMIZATION.md)

CPU optimization techniques for MJPEG streaming.

**Topics covered:**
- Architecture overview (decode path vs passthrough)
- Camera frame rate detection (advertised vs actual)
- Auto-skip based on CPU usage
- TurboJPEG optimization flags
- Target: <5% CPU for MJPEG passthrough

### [DISPLAY_STANDBY.md](DISPLAY_STANDBY.md)

Research into display standby detection and wake control.

**Key findings:**
- Display wake via sysfs brightness: **Confirmed working**
- Touch injection via /dev/input/event0: **Confirmed working**
- Touch device: hyn_ts (Hynitron), I2C bus 0018
- Resolution: 800x480, Multi-touch Protocol B
- Backlight: PWM-controlled

### [USB_CAMERA_CONTROLS.md](USB_CAMERA_CONTROLS.md)

V4L2 camera controls reference for USB cameras on Anycubic printers.

**Topics covered:**
- Available V4L2 controls (brightness, contrast, exposure, etc.)
- Control IDs and ranges for UVC cameras
- Control dependencies (auto vs manual modes)
- Implementation in rkmpi_enc and h264_server.py
- Control file format (`/tmp/h264_ctrl`)
- API endpoints for real-time adjustment

## Related Resources

- **Packet captures**: `/shared/dev/Rinkhals/docs/timelapse-captures/`
- **Firmware analysis**: See Rinkhals project documentation
