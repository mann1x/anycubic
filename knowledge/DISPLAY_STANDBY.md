# Display Standby Mode - RV1106 Anycubic Printers

## Summary

Investigation into detecting and controlling the LCD display standby mode on RV1106-based Anycubic printers (Kobra 2 Pro, Kobra 3, Kobra S1, etc.).

**Status**: Display wake via sysfs brightness CONFIRMED WORKING. Touch injection PARTIALLY IMPLEMENTED - needs verification.

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

#### Approach 1: Direct Event Write (PARTIALLY WORKING)

Writing directly to `/dev/input/event0` appears to work (no errors), but verification of K3SysUi receiving events is pending.

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

**Status**: Events write successfully, but K3SysUi response not verified. May need visual confirmation via display stream.

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

### For Wake Functionality
1. Try writing to `brightness` sysfs first
2. If that doesn't work reliably, consider:
   - Sending a signal to K3SysUi (needs investigation)
   - Finding an IPC mechanism
   - Physical touch remains the most reliable method

### Avoid
- Never write directly to PWM sysfs files
- Never kill K3SysUi process
- Don't assume brightness writes will persist (timer will override)

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

### Pending
- [ ] Visually verify touch injection works via display stream
- [ ] Investigate if EVIOCGRAB prevents write injection
- [ ] Integrate touch injection into display stream web interface
- [ ] Add display wake to fb-status before framebuffer capture

### Use Cases
1. **fb-status**: Wake display before capturing framebuffer background
2. **Display stream**: Remote touch control via web interface mouse clicks
