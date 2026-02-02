# Anycubic RV1106 Tools

Tools and utilities for Anycubic RV1106-based 3D printers.

## Supported Printers

| Model | Code | Model ID |
|-------|------|----------|
| Kobra 2 Pro | K2P | 20021 |
| Kobra 3 | K3 | 20024 |
| Kobra S1 | KS1 | 20025 |
| Kobra 3 Max | K3M | 20026 |
| Kobra 3 V2 | K3V2 | 20027 |
| Kobra S1 Max | KS1M | 20029 |

## Components

### [rkmpi-encoder](rkmpi-encoder/)

Hardware H.264/JPEG encoder using the RV1106 Video Encoder (VENC).

- USB camera capture (V4L2 MJPEG/YUYV)
- Hardware H.264 encoding for streaming
- Hardware MJPEG encoding for snapshots
- Display capture with hardware rotation (RGA)
- Built-in HTTP/MQTT/RPC servers
- Timelapse recording support

```bash
# Quick start
cd rkmpi-encoder && make dynamic
./rkmpi_enc --help
```

### [h264-streamer](h264-streamer/)

HTTP streaming server with web UI for Rinkhals custom firmware.

- MJPEG and H.264 FLV streaming endpoints
- Display capture for remote LCD viewing
- Touch control - interact with printer UI remotely
- Web control interface with live preview
- Moonraker webcam integration
- Auto frame skipping based on CPU load

```bash
# Endpoints
/stream          # MJPEG multipart stream
/snapshot        # Single JPEG frame
/flv             # H.264 in FLV container
/display         # LCD framebuffer stream
/control         # Web UI
```

### [fb-status](fb-status/)

Framebuffer status display utility for showing messages on the printer LCD.

- Direct framebuffer rendering with TTF fonts
- Screen save/restore
- Pipe mode for long-running scripts
- Auto screen orientation per printer model
- Printer busy/free API integration

```bash
# Quick examples
fb_status show "Calibrating..."
fb_status show "Error!" -c red
fb_status pipe -b << 'EOF'
show Starting process...
color yellow
show Step 1 of 3
EOF
```

### [knowledge](knowledge/)

Protocol documentation, research notes, and captures.

- Timelapse RPC protocol
- USB camera bandwidth analysis
- Display standby/wake control
- CPU optimization techniques

## Hardware Specifications

| Component | Specification |
|-----------|---------------|
| SoC | Rockchip RV1106 |
| CPU | ARM Cortex-A7 @ 1.2GHz (single core) |
| Architecture | ARMv7-A with NEON SIMD |
| RAM | 256MB DDR3 |
| Video Encoder | H.264/H.265 up to 2304x1296 @ 30fps |
| RGA | 2D graphics accelerator |
| USB | USB 2.0 OTG |

## Releases

Pre-built binaries and SWU packages are available on the [Releases](https://github.com/mann1x/anycubic/releases) page.

| Component | Latest Release | Assets |
|-----------|---------------|--------|
| rkmpi-encoder | [v1.0.0](https://github.com/mann1x/anycubic/releases/tag/rkmpi-encoder/v1.0.0) | `rkmpi_enc` binary |
| h264-streamer | [v1.0.0](https://github.com/mann1x/anycubic/releases/tag/h264-streamer/v1.0.0) | SWU packages for all models |
| fb-status | [v1.0.0](https://github.com/mann1x/anycubic/releases/tag/fb-status/v1.0.0) | `fb_status` binary |

### Creating a Release

```bash
# Update version and create tag
echo "1.0.1" > h264-streamer/VERSION
git add -A && git commit -m "Bump h264-streamer to 1.0.1"
git tag h264-streamer/v1.0.1
git push origin main h264-streamer/v1.0.1
```

## Building

### Prerequisites

- RV1106 ARM cross-compilation toolchain
- Make

### Build Commands

```bash
# Build encoder
cd rkmpi-encoder && make dynamic

# Build framebuffer utility
cd fb-status && make

# Build h264-streamer SWU
./scripts/build-h264-swu.sh KS1 ./dist
```

## Documentation

- [CLAUDE.md](CLAUDE.md) - Development guide with build system, deployment, and internal documentation

## Related Projects

- [Rinkhals](https://github.com/jbatonnet/Rinkhals) - Custom firmware for Anycubic printers

## License

GPL v3 - See [LICENSE](LICENSE)

## Acknowledgments

- RockChip for the RV1106 SDK
- The Rinkhals project for custom firmware support
- [stb_truetype](https://github.com/nothings/stb) for font rendering
