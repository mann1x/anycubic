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
- **Display capture** with full hardware acceleration (RGA + VENC)
- **Timelapse recording** with automatic MP4 assembly (libx264 + mpeg4 fallback)
- **MQTT keepalive** - automatic PINGREQ to prevent broker disconnections
- See: `rkmpi-encoder/claude.md`

### h264-streamer
HTTP streaming application for Rinkhals custom firmware.
- MJPEG and H.264 FLV streaming
- **Display capture** for remote LCD viewing (RGA hardware rotation)
- Web control interface with live preview
- Moonraker webcam integration
- CPU-based auto frame skipping
- **Timelapse management UI** - browse, preview, download, delete recordings
- **Advanced timelapse** - layer/hyperlapse modes via Moonraker integration
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

## Deployment

### Deploy h264-streamer to Printer

```bash
PRINTER_IP=192.168.178.43
APP_DIR=/useremain/home/rinkhals/apps/29-h264-streamer

# 1. Build encoder
cd rkmpi-encoder
make dynamic

# 2. Stop h264-streamer
sshpass -p 'rockchip' ssh root@$PRINTER_IP "$APP_DIR/app.sh stop"

# 3. Deploy files
sshpass -p 'rockchip' scp rkmpi_enc root@$PRINTER_IP:$APP_DIR/rkmpi_enc
sshpass -p 'rockchip' scp ../h264-streamer/29-h264-streamer/h264_server.py root@$PRINTER_IP:$APP_DIR/h264_server.py

# 4. Start h264-streamer
sshpass -p 'rockchip' ssh root@$PRINTER_IP "$APP_DIR/app.sh start"

# 5. Check status and logs
sshpass -p 'rockchip' ssh root@$PRINTER_IP "$APP_DIR/app.sh status"
sshpass -p 'rockchip' ssh root@$PRINTER_IP "tail -20 /tmp/rinkhals/app-h264-streamer.log"
```

### Important Paths on Printer

| Path | Description |
|------|-------------|
| `/useremain/home/rinkhals/apps/29-h264-streamer/` | h264-streamer app directory |
| `/useremain/home/rinkhals/apps/29-h264-streamer.config` | Persistent config file (JSON) |
| `/tmp/h264_ctrl` | Runtime control file (encoder ↔ Python) |
| `/tmp/rinkhals/app-h264-streamer.log` | Application log file |

### Control File Format

The `/tmp/h264_ctrl` file is used for bidirectional communication:

**Written by h264_server.py:**
- `h264=0|1` - Enable/disable H.264 encoding
- `display_enabled=0|1` - Enable/disable display capture
- `display_fps=N` - Display capture frame rate

**Written by rkmpi_enc:**
- `mjpeg_fps=N.N` - Current MJPEG output FPS
- `h264_fps=N.N` - Current H.264 output FPS
- `mjpeg_clients=N` - Connected MJPEG clients
- `flv_clients=N` - Connected FLV clients
- `camera_max_fps=N` - Detected camera max FPS

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
/useremain/app/gk/Time-lapse-Video/     # Timelapse output (internal)
/mnt/udisk/Time-lapse-Video/            # Timelapse output (USB)
/ac_lib/lib/third_bin/ffmpeg            # ffmpeg binary for timelapse
/ac_lib/lib/third_bin/ffprobe           # ffprobe for metadata extraction
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

## Releases & Versioning

### Version Files
Each component has its own `VERSION` file:
```
rkmpi-encoder/VERSION    # e.g., "1.0.0"
h264-streamer/VERSION    # e.g., "1.0.0"
fb-status/VERSION        # e.g., "1.0.0"
```

### Release Tags
| Tag Format | Example | Effect |
|------------|---------|--------|
| `{component}/v{version}` | `rkmpi-encoder/v1.0.1` | Release single component |
| `release/{date}` | `release/2024-02-02` | Release all unreleased components |

### Creating a Release

**Single component:**
```bash
# 1. Update version
echo "1.0.1" > h264-streamer/VERSION

# 2. Update CHANGELOG.md with release notes (REQUIRED)
# Add entry under the component section with version and date

# 3. Commit and push
git add h264-streamer/VERSION CHANGELOG.md
git commit -m "Bump h264-streamer to 1.0.1"
git push

# 4. Tag and push (triggers CI)
git tag h264-streamer/v1.0.1
git push origin h264-streamer/v1.0.1
```

**Release all components:**
```bash
# Updates VERSION files first, then:
git tag release/2024-02-02
git push origin release/2024-02-02
```
The workflow checks existing releases and only builds components with new versions.

### Changelog Requirements

**IMPORTANT:** Always update `CHANGELOG.md` before creating a release tag.

The changelog follows [Keep a Changelog](https://keepachangelog.com/) format:
- Group changes under: Added, Changed, Fixed, Removed
- List user-facing changes, not internal refactoring
- Include the release date in ISO format (YYYY-MM-DD)

Example entry:
```markdown
### [1.2.0] - 2025-02-03

#### Added
- New feature description

#### Fixed
- Bug fix description
```

### Release Notes Format

GitHub release notes are auto-generated by CI but should be updated manually to add "What's New" section.

**After CI creates the release**, update it with `gh release edit` to add a "What's New" section at the end:

```bash
# View current release notes
gh release view h264-streamer/v1.3.0

# Edit to add What's New section (APPEND to existing content, don't replace)
gh release edit h264-streamer/v1.3.0 --notes "$(cat <<'EOF'
[KEEP EXISTING CONTENT: description, supported printers, installation, features]

---

## What's New in v1.3.0

### Section Title
- Change description
- Another change

See [CHANGELOG.md](https://github.com/mann1x/anycubic/blob/main/CHANGELOG.md) for full history.
EOF
)"
```

**Release notes structure (auto-generated + manual addition):**
1. Component description (auto)
2. Built with / Dependencies (auto)
3. Supported Printers table (auto, h264-streamer only)
4. Installation instructions (auto)
5. Features list (auto)
6. **What's New section (MANUAL - add after release)**
7. Link to CHANGELOG.md (MANUAL)

### GitHub Actions Workflow
The `.github/workflows/release.yml` workflow:
1. Parses the tag to determine which components to build
2. Downloads the cross-compilation toolchain
3. Builds binaries (rkmpi_enc, fb_status)
4. Packages h264-streamer SWU for all 6 printer models
5. Creates GitHub releases with assets and release notes

### CI Build Requirements
These directories are tracked in git for CI builds:
```
rkmpi-encoder/include/       # SDK headers (~11MB)
rkmpi-encoder/lib-printer/   # Runtime libs for linking (~5MB)
rkmpi-encoder/openssl-arm/   # OpenSSL headers + static libs (~6MB)
```

### SWU Package Structure
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
            ├── h264_server.py
            └── rkmpi_enc    # Encoder binary
```

### SWU Passwords by Model
| Models | Password |
|--------|----------|
| K2P, K3, K3V2 | `U2FsdGVkX19deTfqpXHZnB5GeyQ/dtlbHjkUnwgCi+w=` |
| KS1, KS1M | `U2FsdGVkX1+lG6cHmshPLI/LaQr9cZCjA8HZt6Y8qmbB7riY` |
| K3M | `4DKXtEGStWHpPgZm8Xna9qluzAI8VJzpOsEIgd8brTLiXs8fLSu3vRx8o7fMf4h6` |

### Local SWU Build
```bash
# Build encoder and copy to h264-streamer
cd rkmpi-encoder
make dynamic
make install-h264

# Build SWU for specific model
./scripts/build-h264-swu.sh KS1 ./dist

# Or use Rinkhals build system for all models
/shared/dev/Rinkhals/build/swu-tools/h264-streamer/build-local.sh
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
