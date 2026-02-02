# fb-status

Framebuffer status display utility for Anycubic RV1106 printers.

## Features

- **Direct Framebuffer Rendering** - Write text directly to /dev/fb0
- **TTF Font Support** - Dynamic font rendering via stb_truetype
- **Screen Save/Restore** - Capture and restore original screen content
- **Pipe Mode** - Long-running mode for scripts with dynamic updates
- **Auto Orientation** - Detects printer model and applies correct rotation
- **Printer Busy/Free** - Integration with Native API for UI blocking

## Usage

### One-shot Commands

```bash
# Save current screen
fb_status save

# Show status message (green text)
fb_status show "Calibrating..."

# Show with color
fb_status show "Error!" -c red
fb_status show "Warning" -c yellow -g black

# Custom hex color
fb_status show "Custom" -c FF00FF

# Font options
fb_status show "Large" -s 48        # Font size
fb_status show "Bold" -B            # Bold font
fb_status show "Top" -p top         # Position

# Auto-hide after timeout
fb_status show "Quick" -t 3

# Multiline
fb_status show "Line 1\nLine 2\nLine 3"

# Restore original screen
fb_status hide

# Set printer busy/free
fb_status busy
fb_status free
```

### Pipe Mode

For scripts that need dynamic display updates:

```bash
fb_status pipe -b << 'EOF'
show Starting process...
color yellow
show Step 1 of 3
color green
show Complete!
close -f 3
EOF
```

#### Pipe Commands

| Command | Description |
|---------|-------------|
| `show <message>` | Display message (supports `\n`) |
| `color <color>` | Set text color |
| `bg <color>` | Set background color |
| `size <n>` | Set font size (8-200) |
| `position <pos>` | Set position: top, center, bottom |
| `bold` | Enable bold font |
| `italic` | Enable italic font |
| `regular` | Reset to regular font |
| `busy` | Set printer busy |
| `free` | Set printer free |
| `hide` | Temporarily restore screen |
| `close` | Restore and exit |
| `close -f` | Close and set free |
| `close <secs>` | Wait then close |

## Command-Line Options

| Option | Description | Default |
|--------|-------------|---------|
| `-c, --color` | Text color (name or hex) | green |
| `-g, --bg` | Background color | 222222 |
| `-s, --size` | Font size (pixels) | 32 |
| `-p, --position` | Position: top/center/bottom | center |
| `-t, --timeout` | Auto-hide after N seconds | - |
| `-b, --busy` | Also set printer busy | - |
| `-f, --free` | Also set printer free | - |
| `-B, --bold` | Use bold font | - |
| `-I, --italic` | Use italic font | - |
| `-F, --font` | Custom font path | - |
| `-m, --message` | Initial message (pipe mode) | - |
| `-q, --quiet` | No output (pipe mode) | - |

## Colors

### Named Colors

green, red, yellow, blue, white, black, orange, cyan, magenta, gray, pink, purple, lime, aqua, navy, teal, maroon, olive, silver

### Hex RGB

```bash
fb_status show "Red" -c FF0000
fb_status show "Green" -c "#00FF00"
fb_status show "Blue" -c 0x0000FF
fb_status show "Short" -c F00       # Expands to FF0000
```

## Screen Orientation

Auto-detected from `/userdata/app/gk/config/api.cfg`:

| Model | Model ID | Orientation |
|-------|----------|-------------|
| KS1, KS1M | 20025, 20029 | FLIP_180 |
| K3M | 20026 | ROTATE_270 |
| K3, K2P, K3V2 | 20024, 20021, 20027 | ROTATE_90 |

## Building

```bash
# Build
make

# Deploy to printer
make deploy PRINTER_IP=192.168.x.x

# Test on printer
make test PRINTER_IP=192.168.x.x
```

### Prerequisites

- RV1106 cross-compilation toolchain

## Technical Details

| Property | Value |
|----------|-------|
| Display | 800x480, 32bpp BGRX |
| Framebuffer | /dev/fb0 |
| Primary Font | /opt/rinkhals/ui/assets/AlibabaSans-Regular.ttf |
| Fallback Font | /oem/usr/share/simsun_en.ttf |
| Binary Size | ~58KB (stripped) |

## Files

| File | Description |
|------|-------------|
| `fb_status.c` | Main source (~1250 lines) |
| `stb_truetype.h` | TTF font library |
| `Makefile` | Build configuration |

## Example Script

```bash
#!/bin/sh
# Calibration progress display

exec 3> >(exec /tmp/fb_status pipe -q -b)
sleep 1

echo "show Preheating...\nPlease wait" >&3
# ... preheating work ...

echo "color yellow" >&3
echo "show Probing bed\nStep 2 of 4" >&3
# ... probing work ...

echo "color green" >&3
echo "close -f 3 Calibration Complete!" >&3
```

## Documentation

- [claude.md](claude.md) - Detailed technical documentation
