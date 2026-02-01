# Timelapse Implementation Plan for h264-streamer

## Executive Summary

Implement timelapse recording in the `rkmpi_enc` encoder, leveraging its existing RPC client, direct JPEG access, and efficient C implementation.

## Architecture Decision

**Chosen approach: All timelapse logic in rkmpi_enc (encoder)**

| Option | Pros | Cons |
|--------|------|------|
| **Encoder (chosen)** | Direct JPEG access, RPC already integrated, efficient, minimal IPC | Need ffmpeg subprocess for MP4 |
| Python server | Easier MP4 via subprocess | Need IPC for frames, complex coordination |
| Hybrid | Flexibility | Most complex, more failure points |

The encoder already has:
- ✅ RPC client responding to `startLanCapture`, `stopLanCapture`
- ✅ MQTT client for video topics
- ✅ Direct access to JPEG frames
- ✅ HTTP `/snapshot` endpoint

## Implementation Phases

### Phase 1: RPC Handler Extensions

**File: `rpc_client.c`**

Add handlers for timelapse RPC commands:

```c
/* Timelapse state */
typedef struct {
    int active;                     // Timelapse recording in progress
    char gcode_name[256];           // Base name for output (from filepath)
    char output_dir[256];           // /useremain/app/gk/Time-lapse-Video/
    char temp_dir[256];             // /tmp/timelapse_frames/
    int frame_count;                // Number of frames captured
    int sequence_num;               // _01, _02, etc. for multiple prints
} TimelapseState;

static TimelapseState g_timelapse = {0};
```

**New RPC handlers:**

| Method | Action |
|--------|--------|
| `openDelayCamera` | Initialize timelapse, parse gcode path, create temp dir |
| `startLanCapture` | If timelapse active, save JPEG frame |
| `stopLanCapture` | Return success (no action needed) |
| `SetLed` | Return success (LED handled by firmware) |

**openDelayCamera handler:**
```c
static void handle_open_delay_camera(RPCClient *client, int req_id, cJSON *params) {
    cJSON *filepath = cJSON_GetObjectItem(params, "filepath");
    if (!cJSON_IsString(filepath)) {
        rpc_send_video_error(client, req_id, "openDelayCamera", "11401");
        return;
    }

    // Extract base name: /useremain/app/gk/gcodes/Foo.gcode -> Foo
    const char *path = filepath->valuestring;
    const char *basename = strrchr(path, '/');
    basename = basename ? basename + 1 : path;

    // Remove .gcode extension
    strncpy(g_timelapse.gcode_name, basename, sizeof(g_timelapse.gcode_name) - 1);
    char *ext = strstr(g_timelapse.gcode_name, ".gcode");
    if (ext) *ext = '\0';

    // Determine sequence number (_01, _02, etc.)
    g_timelapse.sequence_num = find_next_sequence_num(g_timelapse.gcode_name);

    // Create temp directory
    snprintf(g_timelapse.temp_dir, sizeof(g_timelapse.temp_dir),
             "/tmp/timelapse_%d", getpid());
    mkdir(g_timelapse.temp_dir, 0755);

    // Initialize state
    g_timelapse.active = 1;
    g_timelapse.frame_count = 0;
    strcpy(g_timelapse.output_dir, "/useremain/app/gk/Time-lapse-Video/");

    rpc_send_video_reply(client, req_id, "openDelayCamera");
    rpc_log("Timelapse started: %s (seq %02d)\n",
            g_timelapse.gcode_name, g_timelapse.sequence_num);
}
```

**startLanCapture handler (timelapse mode):**
```c
static void handle_start_lan_capture(RPCClient *client, int req_id, cJSON *params) {
    // Always respond success
    rpc_send_video_reply(client, req_id, "startLanCapture");

    // If timelapse active, capture frame
    if (g_timelapse.active) {
        capture_timelapse_frame();
    }
}
```

### Phase 2: Frame Capture Integration

**File: `rkmpi_enc.c` or new `timelapse.c`**

Need to expose JPEG frame capture to the timelapse system:

```c
/* Called from RPC handler when startLanCapture received during timelapse */
int capture_timelapse_frame(void) {
    if (!g_timelapse.active) return -1;

    // Get current JPEG frame from encoder
    // Option 1: Use existing frame buffer
    // Option 2: Trigger snapshot capture

    uint8_t *jpeg_data;
    size_t jpeg_size;

    // Get latest JPEG from frame buffer
    if (get_current_jpeg_frame(&jpeg_data, &jpeg_size) != 0) {
        rpc_log("Timelapse: Failed to get JPEG frame\n");
        return -1;
    }

    // Save to temp directory
    char filename[512];
    snprintf(filename, sizeof(filename), "%s/frame_%04d.jpg",
             g_timelapse.temp_dir, g_timelapse.frame_count);

    FILE *f = fopen(filename, "wb");
    if (f) {
        fwrite(jpeg_data, 1, jpeg_size, f);
        fclose(f);
        g_timelapse.frame_count++;
        rpc_log("Timelapse: Captured frame %d\n", g_timelapse.frame_count);
    }

    return 0;
}
```

**Frame buffer access:**

The encoder already maintains JPEG frames. Need to add a thread-safe way to access the latest frame:

```c
/* In frame_buffer.c or mjpeg_server.c */
typedef struct {
    uint8_t *data;
    size_t size;
    pthread_mutex_t mutex;
} LatestFrame;

static LatestFrame g_latest_jpeg = {0};

/* Called by encoder when new JPEG is ready */
void update_latest_jpeg(uint8_t *data, size_t size) {
    pthread_mutex_lock(&g_latest_jpeg.mutex);
    // Reallocate if needed
    if (g_latest_jpeg.data == NULL || size > current_alloc) {
        g_latest_jpeg.data = realloc(g_latest_jpeg.data, size);
    }
    memcpy(g_latest_jpeg.data, data, size);
    g_latest_jpeg.size = size;
    pthread_mutex_unlock(&g_latest_jpeg.mutex);
}

/* Called by timelapse to get frame */
int get_current_jpeg_frame(uint8_t **data, size_t *size) {
    pthread_mutex_lock(&g_latest_jpeg.mutex);
    if (g_latest_jpeg.data == NULL || g_latest_jpeg.size == 0) {
        pthread_mutex_unlock(&g_latest_jpeg.mutex);
        return -1;
    }
    *data = g_latest_jpeg.data;
    *size = g_latest_jpeg.size;
    pthread_mutex_unlock(&g_latest_jpeg.mutex);
    return 0;
}
```

### Phase 3: Print Completion Detection

**File: `rpc_client.c`**

Monitor `print_stats.state` in RPC messages:

```c
static void rpc_handle_message(RPCClient *client, const char *msg) {
    cJSON *json = cJSON_Parse(msg);
    // ... existing code ...

    /* Check for print completion */
    if (g_timelapse.active) {
        cJSON *status = cJSON_GetObjectItem(params, "status");
        if (status) {
            cJSON *print_stats = cJSON_GetObjectItem(status, "print_stats");
            if (print_stats) {
                cJSON *state = cJSON_GetObjectItem(print_stats, "state");
                if (cJSON_IsString(state) &&
                    strcmp(state->valuestring, "complete") == 0) {
                    finalize_timelapse();
                }
            }
        }
    }
}
```

### Phase 4: MP4 Assembly

**File: `timelapse.c` (new)**

```c
/* Called when print_stats.state == "complete" */
int finalize_timelapse(void) {
    if (!g_timelapse.active || g_timelapse.frame_count == 0) {
        g_timelapse.active = 0;
        return -1;
    }

    rpc_log("Timelapse: Finalizing %d frames...\n", g_timelapse.frame_count);

    // Build output filename
    char output_mp4[512];
    char output_thumb[512];
    snprintf(output_mp4, sizeof(output_mp4), "%s%s_%02d.mp4",
             g_timelapse.output_dir, g_timelapse.gcode_name,
             g_timelapse.sequence_num);
    snprintf(output_thumb, sizeof(output_thumb), "%s%s_%02d_%d.jpg",
             g_timelapse.output_dir, g_timelapse.gcode_name,
             g_timelapse.sequence_num, g_timelapse.frame_count);

    // Assemble MP4 using ffmpeg
    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
             "ffmpeg -y -framerate 10 -i '%s/frame_%%04d.jpg' "
             "-c:v libx264 -pix_fmt yuv420p -preset fast "
             "-crf 23 '%s' >/dev/null 2>&1",
             g_timelapse.temp_dir, output_mp4);

    int ret = system(cmd);
    if (ret != 0) {
        rpc_log("Timelapse: ffmpeg failed (%d)\n", ret);
    } else {
        rpc_log("Timelapse: Created %s\n", output_mp4);
    }

    // Copy last frame as thumbnail
    char last_frame[512];
    snprintf(last_frame, sizeof(last_frame), "%s/frame_%04d.jpg",
             g_timelapse.temp_dir, g_timelapse.frame_count - 1);
    snprintf(cmd, sizeof(cmd), "cp '%s' '%s'", last_frame, output_thumb);
    system(cmd);

    // Cleanup temp directory
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", g_timelapse.temp_dir);
    system(cmd);

    // Reset state
    g_timelapse.active = 0;
    g_timelapse.frame_count = 0;

    return ret == 0 ? 0 : -1;
}
```

### Phase 5: RPC Client (No Changes Needed)

**Note:** When h264-streamer uses rkmpi/rkmpi-yuyv encoder mode, the encoder runs with `-S` (server mode), which already starts the RPC client.

**No code changes required** - RPC is already active when timelapse would be used.

### Phase 6: Camera Status Check

The slicer/display checks camera status before starting prints. Need to ensure our RPC responder passes this check.

**Investigation needed:** Capture what query is made and respond appropriately.

Likely candidates:
- May be part of general info query
- May check if video endpoints respond
- May use MQTT `video` topic

**Workaround:** If status check fails, the encoder may need to respond to additional RPC methods.

## File Changes Summary

| File | Changes |
|------|---------|
| `rpc_client.c` | Add openDelayCamera, SetLed handlers; monitor print_stats.state |
| `rpc_client.h` | Add TimelapseState struct, function declarations |
| `rkmpi_enc.c` | Start RPC always; expose JPEG frame access |
| `timelapse.c` (new) | Frame capture, MP4 assembly, temp file management |
| `timelapse.h` (new) | Timelapse function declarations |
| `Makefile` | Add timelapse.c |

## Dependencies

- **ffmpeg**: Already available on printer (`/usr/bin/ffmpeg`)
- **libx264**: Check availability - may need to use different codec
- **Temp storage**: `/tmp/` has limited space; may need to use `/useremain/`

## Testing Plan

1. **Unit test RPC handlers**: Mock messages, verify responses
2. **Frame capture test**: Verify JPEG saved correctly
3. **MP4 assembly test**: Run ffmpeg manually on captured frames
4. **Integration test**: Full print with timelapse enabled
5. **Edge cases**: Print cancel, printer reboot during timelapse

## Risk Mitigation

| Risk | Mitigation |
|------|------------|
| ffmpeg not available | Check at startup, disable timelapse if missing |
| Temp space exhausted | Monitor /tmp usage, use /useremain if needed |
| Frame capture during encoding | Use mutex, copy frame data |
| Print cancelled mid-timelapse | Cleanup temp files on cancel/error |
| Multiple rapid prints | Track sequence numbers per gcode name |

## Estimated Effort

| Phase | Effort |
|-------|--------|
| Phase 1: RPC handlers | 2-3 hours |
| Phase 2: Frame capture | 2-3 hours |
| Phase 3: Print detection | 1 hour |
| Phase 4: MP4 assembly | 2 hours |
| Phase 5: RPC client | N/A (already active in server mode) |
| Phase 6: Camera check | 2-4 hours (investigation) |
| Testing | 3-4 hours |
| **Total** | **12-17 hours** |

## Alternative: Minimal Implementation

If full implementation is too complex, a minimal version:

1. Just respond to `openDelayCamera` with success
2. On `startLanCapture`, save JPEG to `/useremain/app/gk/Time-lapse-Video/`
3. Let user assemble MP4 manually (or add script)

This would allow prints to start with timelapse enabled, and frames would be captured, but no automatic MP4 assembly.
