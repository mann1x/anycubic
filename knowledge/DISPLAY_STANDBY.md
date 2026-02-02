# Display Standby Mode - RV1106 Anycubic Printers

## Summary

Investigation into detecting and controlling the LCD display standby mode on RV1106-based Anycubic printers (Kobra 2 Pro, Kobra 3, Kobra S1, etc.).

**Status**: Display wake via sysfs brightness CONFIRMED WORKING. Touch injection CONFIRMED WORKING.

## Key Findings

### Display Hardware

| Component | Path/Details |
|-----------|--------------|
| Framebuffer | `/dev/fb0` (800x480, 32bpp) |
| DRM device | `/dev/dri/card0` |
| Touch input | `/dev/input/event0` (hyn_ts) |
| Backlight type | PWM-controlled |
| UI process | K3SysUi (Qt-based, PID typically 766) |

### Touch Device Details

| Property | Value |
|----------|-------|
| Name | hyn_ts (Hynitron touchscreen) |
| Bus | 0018 (I2C) |
| Sysfs | `/devices/platform/ff460000.i2c/i2c-3/3-0015/input/input1` |
| Protocol | Multi-touch Protocol B |
| X Range | 0-800 |
| Y Range | 0-480 |

**Supported Events:**
- `ABS_MT_SLOT` - Touch slot for multi-touch
- `ABS_MT_TRACKING_ID` - Touch tracking ID (-1 = touch up)
- `ABS_MT_POSITION_X` - X coordinate (0-800)
- `ABS_MT_POSITION_Y` - Y coordinate (0-480)
- `ABS_MT_TOUCH_MAJOR` - Touch area
- `ABS_MT_PRESSURE` - Touch pressure
- `BTN_TOUCH` - Touch state (1=down, 0=up)

### Backlight Control Architecture

K3SysUi controls the display backlight via two mechanisms:

1. **Sysfs abstraction** (safe to read):
   - `/sys/class/backlight/backlight/brightness` - software brightness value (0-255)
   - `/sys/class/backlight/backlight/actual_brightness` - current actual brightness
   - `/sys/class/backlight/backlight/max_brightness` - 255

2. **Direct PWM control** (K3SysUi holds file descriptors):
   - `/sys/devices/platform/ff350000.pwm/pwm/pwmchip0/pwm0/enable` - 0=off, 1=on
   - `/sys/devices/platform/ff350000.pwm/pwm/pwmchip0/pwm0/duty_cycle` - brightness level
   - `/sys/devices/platform/ff350000.pwm/pwm/pwmchip0/pwm0/period` - 400000 (fixed)

### Standby Timeout Configuration

Located in `/userdata/app/gk/config/para.cfg`:
```json
"backlight_close_time": 10
```

This value appears to be in **minutes** (10 minutes default).

## Detecting Standby State

### Safe Method (Recommended)

```bash
# Read actual brightness - always safe, no side effects
brightness=$(cat /sys/class/backlight/backlight/actual_brightness)
if [ "$brightness" -eq 0 ]; then
    echo "Display is in STANDBY"
else
    echo "Display is AWAKE (brightness: $brightness)"
fi
```

### State Values

| actual_brightness | Display State |
|-------------------|---------------|
| 0 | Standby (backlight off) |
| 1-255 | Awake (255 = full brightness) |

### Standby Mode Behavior

When K3SysUi puts the display into standby mode, it does TWO things:
1. Sets brightness to 0 (backlight off)
2. **Changes the framebuffer background to gray** (not black)

This means capturing the framebuffer while in standby will show gray content, not the actual UI.

**Important**: Touch injection is the preferred wake method because it triggers K3SysUi to:
- Restore brightness to full (255)
- Restore the proper black background
- Reset the sleep timer

Simply setting brightness via sysfs only turns on the backlight but does NOT restore the background.

## Waking the Display

### Method 1: Sysfs Brightness (CONFIRMED WORKING)

```bash
# Write to brightness sysfs - CONFIRMED WORKING
echo 128 > /sys/class/backlight/backlight/brightness  # Wake with medium brightness
echo 255 > /sys/class/backlight/backlight/brightness  # Wake with full brightness
```

**Tested**: Writing to brightness sysfs successfully wakes the display from standby.

**Note**: K3SysUi has its own timer (`BackLightTimerHandler`) that will put the display back to sleep after the configured timeout.

### Method 2: Direct PWM (DANGEROUS - DO NOT USE)

```bash
# DO NOT DO THIS - causes conflicts with K3SysUi
# K3SysUi holds open file descriptors to PWM sysfs
# Writing directly can cause system instability and loud error beeps
echo 1 > /sys/devices/platform/ff350000.pwm/pwm/pwmchip0/pwm0/enable  # BAD!
```

### Method 3: Simulate Touch Input

K3SysUi monitors `/dev/input/event0` for touch events. However, K3SysUi **grabs the device exclusively** via `EVIOCGRAB`, which complicates injection.

#### Approach 1: Direct Event Write (CONFIRMED WORKING)

Writing directly to `/dev/input/event0` works. K3SysUi receives and processes the injected touch events.

**C Implementation** (`touch_direct.c`):
```c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <linux/input.h>

void emit(int fd, int type, int code, int val) {
    struct input_event ie = {0};
    gettimeofday(&ie.time, NULL);
    ie.type = type;
    ie.code = code;
    ie.value = val;
    write(fd, &ie, sizeof(ie));
}

int inject_touch(int x, int y, int duration_ms) {
    int fd = open("/dev/input/event0", O_WRONLY);
    if (fd < 0) return -1;

    // Touch down - MT Protocol B
    emit(fd, EV_ABS, ABS_MT_SLOT, 0);
    emit(fd, EV_ABS, ABS_MT_TRACKING_ID, 1);
    emit(fd, EV_ABS, ABS_MT_POSITION_X, x);
    emit(fd, EV_ABS, ABS_MT_POSITION_Y, y);
    emit(fd, EV_ABS, ABS_MT_TOUCH_MAJOR, 50);
    emit(fd, EV_ABS, ABS_MT_PRESSURE, 100);
    emit(fd, EV_KEY, BTN_TOUCH, 1);
    emit(fd, EV_SYN, SYN_REPORT, 0);

    usleep(duration_ms * 1000);

    // Touch up
    emit(fd, EV_ABS, ABS_MT_TRACKING_ID, -1);
    emit(fd, EV_KEY, BTN_TOUCH, 0);
    emit(fd, EV_SYN, SYN_REPORT, 0);

    close(fd);
    return 0;
}
```

**Status**: CONFIRMED WORKING - K3SysUi receives and processes injected touch events.

#### Approach 2: uinput Virtual Device (DOES NOT WORK)

Creating a virtual touch device via `/dev/uinput` works technically, but K3SysUi **ignores virtual devices**. It only monitors the physical `/dev/input/event0` device that it opened at startup.

**Note**: K3SysUi has event0 open on fd 9 and 11, and uses epoll for event monitoring.

## K3SysUi Internals

From binary string analysis:

```
echo 255 > /sys/class/backlight/backlight/brightness  # Wake command
echo 0 > /sys/class/backlight/backlight/brightness    # Sleep command
BackLightTimerHandler()                                # Timer callback
backlight_close_time                                   # Config key
```

K3SysUi is a Qt application that:
- Reads touch events from `/dev/input/event0`
- Controls backlight via shell commands to sysfs
- Has direct PWM file descriptors open (for fine control)
- Implements a timer that triggers sleep after inactivity

### K3SysUi File Descriptors (from /proc/PID/fd)

| FD | Target | Purpose |
|----|--------|---------|
| 6 | /dev/fb0 | Framebuffer for UI rendering |
| 9, 11 | /dev/input/event0 | Touch input |
| 13 | pwm0/enable | PWM on/off control |
| 14 | pwm0/duty_cycle | PWM brightness level |
| 15 | pwm0/period | PWM period (fixed) |

## RPC API

No display-related methods are exposed via the gkapi RPC interface (port 18086). The following methods were tested and returned "unregistered":

- `System/GetWakeAllowed`
- `System/ScreenSaver`
- `Para/GetBacklightCloseTime`

## Recommendations

### For Detection Only
Use `actual_brightness` sysfs - it's read-only and always accurate.

### For Wake Functionality (CONFIRMED WORKING)
1. **Touch injection** (RECOMMENDED) - Write events directly to `/dev/input/event0`
   - Restores brightness to full
   - Restores proper black background (standby uses gray)
   - Resets K3SysUi sleep timer
2. **Brightness sysfs** (partial wake only) - Write to `/sys/class/backlight/backlight/brightness`
   - Only turns on backlight
   - Does NOT restore background (stays gray)
   - Does NOT reset sleep timer

**For fb-status**: Use touch injection only at safe coordinates (upper-right corner).
This properly wakes K3SysUi and restores the framebuffer to actual UI content.

### Avoid
- Never write directly to PWM sysfs files
- Never kill or send signals to K3SysUi process
- Never try to manually restart K3SysUi - it requires boot-time Qt/framebuffer setup
- Don't assume brightness writes will persist (timer will override)

### Forcing Screen Refresh
There is no safe programmatic way to force K3SysUi to refresh/repaint the display. Available options:
- **Physical touch** on the screen
- **Reboot** the printer
- Trigger a state change K3SysUi monitors (start print, change settings, etc.)

**WARNING**: Sending SIGUSR1 or other signals to K3SysUi will crash it, and manual restart
results in incorrect display orientation due to missing boot-time environment setup.

## Related Files

| File | Purpose |
|------|---------|
| `/userdata/app/gk/config/para.cfg` | Display timeout config |
| `/userdata/app/gk/K3SysUi` | UI binary |
| `/tmp/gkui.log` | K3SysUi log output |
| `/tmp/rinkhals/K3SysUi.log` | Rinkhals-redirected logs |

## Touch Control Investigation Status

### Completed
- [x] Identified touch device: hyn_ts on `/dev/input/event0`
- [x] Documented MT Protocol B event format
- [x] Confirmed display wake via brightness sysfs
- [x] Created direct event injection tool (`touch_direct.c`)
- [x] Tested uinput approach (does not work - K3SysUi ignores virtual devices)
- [x] Confirmed direct event write works - K3SysUi receives injected touches
- [x] Confirmed EVIOCGRAB does NOT prevent write injection

### Pending
- [ ] Integrate touch injection into display stream web interface
- [ ] Add display wake to fb-status before framebuffer capture

### Use Cases
1. **fb-status**: Wake display before capturing framebuffer background
2. **Display stream**: Remote touch control via web interface mouse clicks

## Safe Wake Touch Coordinates

To wake the display without triggering any UI action, touch the **upper-right corner** where the Anycubic status icon is located.

### Discovering Display Properties

Display resolution and orientation vary by printer model. Discover dynamically:

```bash
# Get framebuffer dimensions
cat /sys/class/graphics/fb0/virtual_size
# Output: 800,480 (width,height)

# Get bits per pixel and line length for orientation detection
cat /sys/class/graphics/fb0/bits_per_pixel  # typically 32
cat /sys/class/graphics/fb0/stride          # bytes per line

# Detect orientation from fb_var_screeninfo
# width > height = landscape (0° or 180°)
# height > width = portrait (90° or 270°)
```

### Safe Wake Coordinates Formula

The status icon is in the **upper-right corner** of the display (as viewed by user). Touch coordinates must account for both resolution AND orientation.

```c
// Discover dimensions at runtime
int fb_width, fb_height;  // From /sys/class/graphics/fb0/virtual_size
int orientation;          // 0, 90, 180, 270 degrees

// Safe wake coordinates depend on orientation
// Status icon is always upper-right in USER's view
int safe_x, safe_y;

switch (orientation) {
    case 0:    // Normal landscape
        safe_x = fb_width - 2;
        safe_y = 2;
        break;
    case 90:   // Portrait, rotated CW
        safe_x = fb_width - 2;
        safe_y = fb_height - 2;
        break;
    case 180:  // Landscape, upside down
        safe_x = 2;
        safe_y = fb_height - 2;
        break;
    case 270:  // Portrait, rotated CCW
        safe_x = 2;
        safe_y = 2;
        break;
}
```

### Known Display Configurations

Orientation is determined by model ID from `/userdata/app/gk/config/api.cfg`:

| Model | Model ID | Orientation | FB Size | Touch Safe Wake |
|-------|----------|-------------|---------|-----------------|
| Kobra 2 Pro | 20021 | ROTATE_90 | 480x800 | (478, 2) |
| Kobra 3 | 20024 | ROTATE_90 | 480x800 | (478, 2) |
| Kobra S1 | 20025 | FLIP_180 | 800x480 | (2, 478) |
| Kobra 3 Max | 20026 | ROTATE_270 | 480x800 | (2, 2) |
| Kobra 3 V2 | 20027 | ROTATE_90 | 480x800 | (478, 2) |
| Kobra S1 Max | 20029 | FLIP_180 | 800x480 | (2, 478) |

**Note**: fb-status uses the same orientation detection logic. See `detect_orientation()` in `fb_status.c`.

**Usage in fb-status:**
1. Discover display dimensions from framebuffer
2. Determine safe coordinates based on orientation
3. Inject touch at safe coordinates: `inject_touch(safe_x, safe_y, 50)`
4. Wait briefly (100ms) for K3SysUi to restore display
5. Capture framebuffer background (now shows actual UI, not gray)
