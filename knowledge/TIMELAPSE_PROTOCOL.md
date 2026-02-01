# Timelapse Protocol for Anycubic Kobra S1

This document describes the timelapse recording protocol based on packet captures from a working gkcam implementation.

## Overview

Timelapse recording is triggered by the slicer when starting a print with timelapse enabled. The printer's firmware communicates with gkcam via:
- **RPC (port 18086)**: Binary API with ETX (0x03) delimited JSON messages
- **MQTT (port 9883 TLS)**: Pub/sub for async events and status

## Print Start Requirements

Before a print with timelapse can start, the system performs a camera check:
- The slicer and printer display verify the camera is connected
- If gkcam is not responding properly, the print will be blocked with "Camera not connected" error
- This check must be passed for timelapse-enabled prints to begin

## RPC Protocol (Port 18086)

### Message Format
```
{JSON_MESSAGE}\x03
```
Messages are ETX (0x03) delimited JSON.

### Video Stream Request/Reply Pattern

Requests come as `process_status_update` with nested `video_stream_request`:
```json
{
  "id": 0,
  "method": "process_status_update",
  "params": {
    "eventtime": 0,
    "response": "",
    "status": {
      "video_stream_request": {
        "id": <request_id>,
        "method": "<method_name>",
        "params": {<method_params>}
      }
    }
  }
}
```

Replies are sent as `Video/VideoStreamReply`:
```json
{
  "id": 0,
  "method": "Video/VideoStreamReply",
  "params": {
    "eventtime": 0,
    "status": {
      "video_stream_reply": {
        "id": <request_id>,
        "method": "<method_name>",
        "result": {}
      }
    }
  }
}
```

On error:
```json
{
  "id": 0,
  "method": "Video/VideoStreamReply",
  "params": {
    "eventtime": 0,
    "status": {
      "video_stream_reply": {
        "id": <request_id>,
        "method": "<method_name>",
        "error": {
          "code": "11401"
        }
      }
    }
  }
}
```

### Error Codes

| Code | Message (Chinese) | Meaning |
|------|-------------------|---------|
| 11401 | 摄像头未连接 | Camera not connected |

## Timelapse RPC Commands

### 1. SetLed - Turn LED On/Off

**Request:**
```json
{
  "video_stream_request": {
    "id": 1,
    "method": "SetLed",
    "params": {"enable": 1}
  }
}
```

**Response:**
```json
{
  "video_stream_reply": {
    "id": 1,
    "method": "SetLed",
    "result": {}
  }
}
```

**Purpose:** Turn on the printer's LED for proper lighting during frame capture.

### 2. openDelayCamera - Initialize Timelapse

**Request:**
```json
{
  "video_stream_request": {
    "id": 2,
    "method": "openDelayCamera",
    "params": {
      "filepath": "/useremain/app/gk/gcodes/Filename_PLA_0.2_Xm.gcode"
    }
  }
}
```

**Response:**
```json
{
  "video_stream_reply": {
    "id": 2,
    "method": "openDelayCamera",
    "result": {}
  }
}
```

**Purpose:** Initialize timelapse recording. The gcode filepath is used to derive the output MP4 filename.

**Output naming convention:**
- Input: `/useremain/app/gk/gcodes/Cylinder_plate(01)_PLA_0.2_6m49s.gcode`
- Output: `/useremain/app/gk/Time-lapse-Video/Cylinder_plate(01)_PLA_0.2_6m49s_01.mp4`
- Thumbnail: `/useremain/app/gk/Time-lapse-Video/Cylinder_plate(01)_PLA_0.2_6m49s_01_<frames>.jpg`

### 3. startLanCapture - Capture Timelapse Frame

**Request:**
```json
{
  "video_stream_request": {
    "id": 5,
    "method": "startLanCapture",
    "params": {
      "appid": "",
      "channel": "",
      "key": "",
      "license": "",
      "mode": "",
      "rtc_token": "",
      "salt": "",
      "uid": 0
    }
  }
}
```

**Response:**
```json
{
  "video_stream_reply": {
    "id": 5,
    "method": "startLanCapture",
    "result": {}
  }
}
```

**Purpose:** Capture a single timelapse frame. Called once per layer change during printing.

**Note:** The params (appid, channel, etc.) are for RTC streaming and can be ignored for timelapse.

### 4. stopLanCapture - Stop Capture

**Request:**
```json
{
  "video_stream_request": {
    "id": 3,
    "method": "stopLanCapture",
    "params": {}
  }
}
```

**Response:**
```json
{
  "video_stream_reply": {
    "id": 3,
    "method": "stopLanCapture",
    "result": {}
  }
}
```

**Purpose:** Stop any ongoing capture. Called at print start to ensure clean state.

## Timelapse Flow Timeline

```
Print Start
  │
  ├─► SetLed enable=1              # Turn on LED
  │
  ├─► openDelayCamera filepath=... # Initialize timelapse with gcode path
  │
  ├─► stopLanCapture               # Stop any existing capture
  │
  └─► startLanCapture              # Capture first frame (layer 0)

Layer Change (repeat for each layer)
  │
  ├─► SetLed enable=1              # Ensure LED on
  │
  └─► startLanCapture              # Capture frame for this layer

Print Complete
  │
  └─► (implicit)                   # gkcam assembles MP4 automatically
                                   # No explicit closeDelayCamera needed
```

## MQTT Protocol (Port 9883 TLS)

### Topics

**Video control:**
```
anycubic/anycubicCloud/v1/web/printer/{modelId}/{deviceId}/video
anycubic/anycubicCloud/v1/printer/public/{modelId}/{deviceId}/video/report
```

### Print Start Message (with timelapse)

```json
{
  "type": "print",
  "action": "start",
  "msgid": "uuid",
  "timestamp": 1234567890,
  "data": {
    "filename": "Cylinder_plate(01)_PLA_0.2_6m49s.gcode",
    "task_settings": {
      "timelapse": {
        "status": 1,
        "count": 495,
        "type": -481303831
      }
    }
  }
}
```

### Print Status Report (with timelapse enabled)

```json
{
  "type": "info",
  "action": "report",
  "data": {
    "project": {
      "filename": "...",
      "state": "printing",
      "task_settings": {
        "camera_timelapse": 1
      }
    }
  }
}
```

## File Locations

| Purpose | Path |
|---------|------|
| Timelapse videos | `/useremain/app/gk/Time-lapse-Video/` |
| Gcode files | `/useremain/app/gk/gcodes/` |
| Temp frames (during capture) | Unknown (internal to gkcam) |

## Implementation Requirements for h264-streamer

To support timelapse recording, h264-streamer needs:

### 1. RPC Response Handlers

Add to `rpc_client.c`:
- Handle `openDelayCamera`: Save filepath, return success, initialize frame storage
- Handle `startLanCapture`: Capture JPEG snapshot, save to temp folder
- Handle `stopLanCapture`: Return success (may stop ongoing capture)
- Handle `SetLed`: Return success (LED control is handled separately via MQTT)

### 2. Timelapse State Machine

```c
typedef struct {
    int active;                    // Timelapse in progress
    char gcode_path[256];          // Gcode filepath (for output naming)
    char output_dir[256];          // /useremain/app/gk/Time-lapse-Video/
    char temp_dir[256];            // Temp frame storage
    int frame_count;               // Number of frames captured
} TimelapseState;
```

### 3. Frame Capture

On `startLanCapture` when timelapse is active:
1. Get current JPEG frame from encoder
2. Save to temp directory as `frame_NNNN.jpg`
3. Increment frame count

### 4. MP4 Assembly

On print complete (detected via RPC `print_stats.state == "complete"`):
1. Use ffmpeg to assemble frames into MP4:
   ```bash
   ffmpeg -framerate 10 -i frame_%04d.jpg -c:v libx264 -pix_fmt yuv420p output.mp4
   ```
2. Copy last frame as thumbnail with `_<frame_count>.jpg` suffix
3. Clean up temp frames

### 5. Camera Status Check

Respond to camera status queries so prints can start:
- May need to handle additional RPC methods for camera status
- Ensure `startLanCapture` returns success quickly

## Captured Data Reference

Successful timelapse capture data saved at:
- `/mnt/udisk/timelapse_capture/rpc_20260130_133503.pcap`
- `/mnt/udisk/timelapse_capture/rpc_20260130_133503_decoded.jsonl`
- `/mnt/udisk/timelapse_capture/mqtt_20260130_133503.jsonl`

Failed timelapse capture (for comparison):
- `/mnt/udisk/timelapse_capture_failed/`

## Statistics from Successful Capture

| Metric | Value |
|--------|-------|
| Print duration | ~7 minutes |
| Frames captured | 21 |
| Frame interval | ~20 seconds (per layer) |
| Output video size | 221 KB |
| RPC methods used | SetLed (15), openDelayCamera (5), startLanCapture (10), stopLanCapture (5) |

## Notes

1. **No explicit close command**: Timelapse ends implicitly when print completes
2. **Frame rate**: Approximately 1 frame per layer, not time-based
3. **gkcam reliability**: Pure gkcam mode requires clean power cycle to work reliably
4. **Error 11401**: "Camera not connected" - indicates gkcam cannot access camera (likely because another process has it)
