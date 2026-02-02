# Anycubic RV1106 Tools

Tools and utilities for Anycubic RV1106-based 3D printers (Kobra 2 Pro, Kobra 3, Kobra S1, and variants).

## Overview

This repository contains development tools for working with the RV1106 SoC found in modern Anycubic printers:

| Component | Description |
|-----------|-------------|
| **rkmpi-encoder** | Hardware H.264/JPEG encoder using RV1106 VENC |
| **h264-streamer** | HTTP streaming server with web UI |
| **fb-status** | Framebuffer status display utility |
| **knowledge** | Protocol documentation and captures |

## Features

### Video Streaming
- USB camera capture (MJPEG or YUYV)
- Hardware H.264 encoding via RV1106 VENC
- MJPEG and FLV HTTP streaming
- **Display capture** for remote LCD viewing (full hardware acceleration)
- **Touch control** - click on display stream to interact with printer UI
- Web-based control interface
- Moonraker webcam integration

### Timelapse Recording
- Native firmware integration
- Automatic frame capture per layer
- MP4 assembly with ffmpeg
- Thumbnail generation

### Display Utilities
- Direct framebuffer text rendering
- TTF font support
- Screen save/restore
- Auto screen orientation

## Supported Printers

| Model | Code | Model ID |
|-------|------|----------|
| Kobra 2 Pro | K2P | 20021 |
| Kobra 3 | K3 | 20024 |
| Kobra S1 | KS1 | 20025 |
| Kobra 3 Max | K3M | 20026 |
| Kobra 3 V2 | K3V2 | 20027 |
| Kobra S1 Max | KS1M | 20029 |

## Hardware Specifications

The RV1106 SoC provides:
- **CPU**: Single-core ARM Cortex-A7 @ 1.2GHz (ARMv7-A + NEON)
- **RAM**: 256MB DDR3 (shared with system)
- **Video Engine**: H.264/H.265 encoder up to 2304x1296 @ 30fps
- **RGA**: 2D graphics accelerator
- **USB**: USB 2.0 OTG

## Building

### Prerequisites

- RV1106 ARM cross-compilation toolchain
- Make

### Build Commands

```bash
# Build hardware encoder
cd rkmpi-encoder
make dynamic

# Build framebuffer utility
cd fb-status
make
```

## Usage

### Streaming Server

The h264-streamer provides HTTP endpoints:

| Endpoint | Description |
|----------|-------------|
| `/stream` | MJPEG multipart stream |
| `/snapshot` | Single JPEG frame |
| `/flv` | H.264 in FLV container |
| `/display` | LCD framebuffer MJPEG stream |
| `/display/snapshot` | Single LCD frame |
| `/control` | Web UI with settings |

### Framebuffer Display

```bash
# Show status message
fb_status show "Calibrating..."

# With colors
fb_status show "Error!" -c red

# Pipe mode for scripts
fb_status pipe -b << 'EOF'
show Starting process...
color yellow
show Step 1 of 3
EOF
```

## Documentation

Detailed documentation is available in each component's `claude.md`:

- [rkmpi-encoder/claude.md](rkmpi-encoder/claude.md) - Encoder architecture and build system
- [h264-streamer/claude.md](h264-streamer/claude.md) - Streaming server configuration
- [fb-status/claude.md](fb-status/claude.md) - Display utility usage

Protocol documentation:
- [knowledge/TIMELAPSE_PROTOCOL.md](knowledge/TIMELAPSE_PROTOCOL.md) - Timelapse RPC protocol

## Related Projects

- [Rinkhals](https://github.com/jbatonnet/Rinkhals) - Custom firmware for Anycubic printers

## Releases

Each component is versioned independently:

| Component | Version File | Release Tag Format |
|-----------|--------------|-------------------|
| rkmpi-encoder | `rkmpi-encoder/VERSION` | `rkmpi-encoder/vX.Y.Z` |
| h264-streamer | `h264-streamer/VERSION` | `h264-streamer/vX.Y.Z` |
| fb-status | `fb-status/VERSION` | `fb-status/vX.Y.Z` |

### Creating a Release

1. Update the `VERSION` file for the component(s) you want to release
2. Commit and push
3. Create and push a tag:
   ```bash
   # Single component
   git tag rkmpi-encoder/v1.0.0
   git push origin rkmpi-encoder/v1.0.0

   # Or release all unreleased versions
   git tag release/2024-01-15
   git push origin release/2024-01-15
   ```

The GitHub Actions workflow will automatically build and create releases.

## License

This project is provided as-is for educational and development purposes.

## Acknowledgments

- RockChip for the RV1106 SDK
- The Rinkhals project for custom firmware support
- stb_truetype for font rendering
