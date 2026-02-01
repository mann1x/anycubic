# Anycubic Tools - Development Guide

Development tools and utilities for Anycubic RV1106-based 3D printers (Kobra 2 Pro, Kobra 3, Kobra S1, etc.).

## Repository Structure

```
anycubic/
├── rkmpi-encoder/      # Hardware H.264/JPEG encoder (C)
├── h264-streamer/      # HTTP streaming server (Python + C)
├── fb-status/          # Framebuffer status display (C)
├── scripts/            # Test and utility scripts
└── knowledge/          # Protocol documentation
```

## Components

### rkmpi-encoder
Native USB camera capture with RV1106 hardware H.264 encoding.
- V4L2 MJPEG/YUYV capture from USB cameras
- TurboJPEG software decode OR hardware JPEG encoding
- RKMPI VENC hardware H.264 encoding
- Built-in HTTP, MQTT, and RPC servers
- **Timelapse recording** with automatic MP4 assembly
- See: `rkmpi-encoder/claude.md`

### h264-streamer
HTTP streaming application for Rinkhals custom firmware.
- MJPEG and H.264 FLV streaming
- Web control interface with live preview
- Moonraker webcam integration
- CPU-based auto frame skipping
- See: `h264-streamer/claude.md`

### fb-status
Lightweight framebuffer text overlay utility.
- Direct /dev/fb0 rendering with TTF fonts
- Screen save/restore via ffmpeg
- Pipe mode for long-running scripts
- Auto screen orientation detection
- See: `fb-status/claude.md`

---

## Target Hardware: RV1106

### Specifications
| Component | Specification |
|-----------|---------------|
| CPU | Single-core ARM Cortex-A7 @ 1.2GHz |
| Architecture | ARMv7-A with NEON SIMD |
| RAM | 256MB DDR3 (100-150MB available) |
| Video Encoder | H.264/H.265 up to 2304x1296 @ 30fps |
| USB | USB 2.0 OTG (~35-40 MB/s practical) |

### Printer Models
| Code | Model ID | Printer |
|------|----------|---------|
| K2P | 20021 | Kobra 2 Pro |
| K3 | 20024 | Kobra 3 |
| KS1 | 20025 | Kobra S1 |
| K3M | 20026 | Kobra 3 Max |
| K3V2 | 20027 | Kobra 3 V2 |
| KS1M | 20029 | Kobra S1 Max |

---

## Build System

### Cross-Compilation Toolchain
```bash
# Toolchain location
/shared/dev/rv1106-toolchain

# Compiler prefix
arm-rockchip830-linux-uclibcgnueabihf-

# Compiler flags
-march=armv7-a -mfpu=neon -mfloat-abi=hard -fno-stack-protector -O2
```

### Build Commands
```bash
# Build rkmpi-encoder
cd rkmpi-encoder
make dynamic          # Dynamic linking (~2MB)
make static           # Static linking (~5MB+)
make install-h264     # Copy to h264-streamer

# Build fb-status
cd fb-status
make                  # Build binary (~70KB)
make deploy           # Deploy to printer
```

### Required Libraries (on printer)
Located in `/oem/usr/lib/`:
- librockit_full.so - Multimedia framework
- librockchip_mpp.so - Media processing
- librga.so - 2D graphics
- libdrm.so - Direct rendering
- libturbojpeg.so - JPEG codec

---

## Printer Connection

```bash
PRINTER_IP=192.168.178.43

# SSH access
sshpass -p 'rockchip' ssh root@$PRINTER_IP

# Deploy files
sshpass -p 'rockchip' scp ./binary root@$PRINTER_IP:/tmp/

# Run on printer
sshpass -p 'rockchip' ssh root@$PRINTER_IP '/tmp/binary'
```

---

## ⛔ Critical Warnings

### Never Kill Python Processes
```
NEVER USE: killall python
NEVER USE: pkill python

Moonraker, Klipper, and other critical services run as Python.
Killing all Python processes will BRICK the printer!

Always use app.sh for h264-streamer management.
```

### Never Kill gkapi
```
gkapi is the printer's core RPC service.
Killing it breaks communication between all firmware components.
Requires reboot to recover.
```

---

## Protocol Reference

### Native RPC API (Port 18086)
TCP with ETX (0x03) message delimiter. JSON-RPC format.

```json
// Query LAN mode
{"id": 2016, "method": "Printer/QueryLanPrintStatus", "params": null}

// Enable LAN mode
{"id": 2016, "method": "Printer/OpenLanPrint", "params": null}
```

### MQTT (Port 9883 TLS)
Camera control and timelapse commands.

Topic: `anycubic/anycubicCloud/v1/web/printer/{modelId}/{deviceId}/video`

Credentials: `/userdata/app/gk/config/device_account.json`

### Key Files on Printer
```
/userdata/app/gk/config/api.cfg         # Model ID
/userdata/app/gk/config/device_account.json  # MQTT credentials
/useremain/app/gk/Time-lapse-Video/     # Timelapse output
/oem/usr/lib/                           # SDK libraries
```

---

## Quick Start

### 1. Build the encoder
```bash
cd /shared/dev/anycubic/rkmpi-encoder
make dynamic
```

### 2. Deploy to printer
```bash
make deploy PRINTER_IP=192.168.178.43
```

### 3. Test on printer
```bash
sshpass -p 'rockchip' ssh root@192.168.178.43
export LD_LIBRARY_PATH=/oem/usr/lib:$LD_LIBRARY_PATH
/tmp/rkmpi_enc -v
```

---

## Rinkhals Integration

This repository is the **primary source** for h264-streamer. Rinkhals uses symlinks to reference it.

### Symlink Structure
```
# Rinkhals app directory (symlink)
/shared/dev/Rinkhals/files/4-apps/home/rinkhals/apps/29-h264-streamer/
  -> /srv/dev-disk-by-label-opt/dev/anycubic/h264-streamer/29-h264-streamer/

# SWU build references the symlink, so changes here are included automatically
```

### Development Workflow
1. **Edit** files in `/shared/dev/anycubic/h264-streamer/29-h264-streamer/`
2. **Commit** to `mann1x/anycubic` repository
3. **Build SWU** via `/shared/dev/Rinkhals/build/swu-tools/h264-streamer/build-local.sh`
4. Rinkhals build picks up changes through the symlink

### Building h264-streamer SWU
```bash
# 1. Build encoder
cd /shared/dev/anycubic/rkmpi-encoder
make dynamic
make install-h264

# 2. Build SWU packages (all models)
/shared/dev/Rinkhals/build/swu-tools/h264-streamer/build-local.sh

# Output: /shared/dev/Rinkhals/build/dist/h264-streamer-*.swu
```

---

## Related Projects

- **Rinkhals** - Custom firmware for Anycubic printers: `/shared/dev/Rinkhals`
- **ACProxyCam** - Windows camera proxy application

---

## File Locations Summary

| Component | Source | Binary |
|-----------|--------|--------|
| rkmpi-encoder | `rkmpi-encoder/rkmpi_enc.c` | `rkmpi-encoder/rkmpi_enc` |
| h264-streamer | `h264-streamer/29-h264-streamer/` | `h264-streamer/29-h264-streamer/rkmpi_enc` |
| fb-status | `fb-status/fb_status.c` | `fb-status/fb_status` |

---

## Troubleshooting

### "cannot open shared object file"
```bash
export LD_LIBRARY_PATH=/oem/usr/lib:$LD_LIBRARY_PATH
```

### Camera busy
```bash
# On printer, use Rinkhals helper
. /useremain/rinkhals/.current/tools.sh
kill_by_name gkcam
```

### No video device
```bash
ls /dev/video*
ls /dev/v4l/by-id/
```
