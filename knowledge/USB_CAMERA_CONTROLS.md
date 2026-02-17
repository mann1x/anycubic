# USB Camera V4L2 Controls

Documentation of V4L2 controls available on Anycubic printer USB cameras.

## Camera Identification

```
Name: Integrated Camera: Integrated C
Driver: uvcvideo
Device: /dev/video10
```

## Available Controls

Enumerated via `VIDIOC_QUERYCTRL` with `V4L2_CTRL_FLAG_NEXT_CTRL`:

### User Class Controls (0x00980xxx)

| Control ID | Name | Type | Min | Max | Default | Notes |
|------------|------|------|-----|-----|---------|-------|
| 0x00980900 | Brightness | int | 0 | 255 | 0 | |
| 0x00980901 | Contrast | int | 0 | 255 | 32 | |
| 0x00980902 | Saturation | int | 0 | 132 | 85 | |
| 0x00980903 | Hue | int | -180 | 180 | 0 | |
| 0x0098090c | White Balance Temperature, Auto | bool | 0 | 1 | 1 | Must disable to set manual temp |
| 0x00980910 | Gamma | int | 90 | 150 | 100 | |
| 0x00980913 | Gain | int | 0 | 1 | 1 | On/Off toggle |
| 0x00980918 | Power Line Frequency | menu | 0 | 2 | 1 | 0=Disabled, 1=50Hz, 2=60Hz |
| 0x0098091a | White Balance Temperature | int | 2800 | 6500 | 4000 | Only works when auto=0 |
| 0x0098091b | Sharpness | int | 0 | 30 | 3 | |
| 0x0098091c | Backlight Compensation | int | 0 | 7 | 0 | |

### Camera Class Controls (0x009a0xxx)

| Control ID | Name | Type | Min | Max | Default | Notes |
|------------|------|------|-----|-----|---------|-------|
| 0x009a0901 | Exposure, Auto | menu | 0 | 3 | 3 | 1=Manual, 3=Aperture Priority |
| 0x009a0902 | Exposure (Absolute) | int | 10 | 2500 | 156 | Only works when auto=1 |
| 0x009a0903 | Exposure, Auto Priority | bool | 0 | 1 | 0 | Constant FPS vs variable |

### JPEG Class Controls (0x009d0xxx)

**NOT AVAILABLE** on this camera. The camera's MJPEG compression quality is fixed in firmware and cannot be changed via V4L2.

## Control Dependencies

### White Balance
- `white_balance_auto=1`: Camera controls temperature automatically
- `white_balance_auto=0`: Manual temperature control via `white_balance_temp`
- Setting `white_balance_temp` while `auto=1` returns "Input/output error"

### Exposure
- `exposure_auto=3` (Aperture Priority): Camera controls exposure automatically
- `exposure_auto=1` (Manual): Manual control via `exposure_absolute`
- Setting `exposure_absolute` while `auto=3` returns "Input/output error"
- Value `exposure_auto=0` is INVALID and returns "Invalid argument"

## JPEG Quality Control

The USB camera does NOT support `V4L2_CID_JPEG_COMPRESSION_QUALITY`.

**Workaround in rkmpi-yuyv mode:**
- Camera outputs raw YUYV frames
- rkmpi_enc encodes to JPEG via hardware VENC
- Quality controlled by `--jpeg-quality` parameter (1-100, default 85)

## Multi-Camera Control

For multi-camera setups (up to 4 cameras), each camera has its own control files:

| Camera | Command File | Control File |
|--------|--------------|--------------|
| CAM#1 | /tmp/h264_cmd | /tmp/h264_ctrl |
| CAM#2 | /tmp/h264_cmd_2 | /tmp/h264_ctrl_2 |
| CAM#3 | /tmp/h264_cmd_3 | /tmp/h264_ctrl_3 |
| CAM#4 | /tmp/h264_cmd_4 | /tmp/h264_ctrl_4 |

Each encoder instance is started with `--cmd-file` and `--ctrl-file` options to specify the appropriate files. This prevents interference between camera controls when multiple encoders are running.

**Example secondary camera launch:**
```bash
rkmpi_enc -S -N -v --no-flv --streaming-port 8082 -d /dev/video12 \
    --cmd-file /tmp/h264_cmd_2 --ctrl-file /tmp/h264_ctrl_2 -y
```

The control server manages which control file to write to based on the active camera selection in the UI.

## Implementation in rkmpi_enc

### Data Structures

```c
typedef struct {
    int brightness;           /* 0-255, default 0 */
    int contrast;             /* 0-255, default 32 */
    int saturation;           /* 0-132, default 85 */
    int hue;                  /* -180 to 180, default 0 */
    int gamma;                /* 90-150, default 100 */
    int sharpness;            /* 0-30, default 3 */
    int gain;                 /* 0-1, default 1 */
    int backlight_comp;       /* 0-7, default 0 */
    int white_balance_temp;   /* 2800-6500, default 4000 */
    int white_balance_auto;   /* 0-1, default 1 */
    int exposure_auto;        /* 1=manual, 3=aperture priority */
    int exposure_absolute;    /* 10-2500, default 156 */
    int exposure_auto_priority; /* 0-1, default 0 */
    int power_line_freq;      /* 0=disabled, 1=50Hz, 2=60Hz */
    unsigned int set_mask;    /* Bitmask of controls to apply */
} CameraControls;
```

### Control Flow

1. Control server writes `cam_*` values to `/tmp/h264_cmd`
2. rkmpi_enc reads cmd file and applies to camera via `VIDIOC_S_CTRL`
3. rkmpi_enc writes current values back to ctrl file for control server to display

### Ctrl File Format

```
cam_brightness=0
cam_contrast=32
cam_saturation=85
cam_hue=0
cam_gamma=100
cam_sharpness=3
cam_gain=1
cam_backlight=0
cam_wb_auto=1
cam_wb_temp=4000
cam_exposure_auto=3
cam_exposure=156
cam_exposure_priority=0
cam_power_line=1
```

## API Endpoints (control_server.c)

### GET /api/camera/controls

Returns all camera controls with current values, ranges, and defaults:

```json
{
  "brightness": {"value": 0, "min": 0, "max": 255, "default": 0},
  "contrast": {"value": 32, "min": 0, "max": 255, "default": 32},
  "exposure_auto": {"value": 3, "min": 1, "max": 3, "default": 3,
                    "options": {"1": "Manual", "3": "Auto"}}
}
```

### POST /api/camera/set

Set a single control in real-time:

```json
{"control": "contrast", "value": 50}
```

Response: `{"status": "ok", "control": "contrast", "value": 50}`

### POST /api/camera/reset

Reset all controls to defaults.

## Troubleshooting

### White/Flash Frames

Occasional white frames may appear due to:
1. DMA cache coherency issues (cacheable buffer + timing)
2. VENC encoding failure
3. USB bandwidth issues

**Mitigation:** JPEG validation added to skip invalid frames:
- Check for 0xFFD8 header
- Require minimum 100 byte size
- Log bad frames: `[JPEG] Bad frame detected: len=X, header=0xXXXX`

### Control Read Failures

If `VIDIOC_G_CTRL` returns all zeros:
- May need streaming active before controls are readable
- Some cameras require extended controls API (`VIDIOC_G_EXT_CTRLS`)
- Current implementation relies on Python config values, not camera readback
