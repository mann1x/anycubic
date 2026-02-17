. /useremain/rinkhals/.current/tools.sh

APP_ROOT="$(dirname "$(realpath "$0")")"
PID_FILE=/tmp/rinkhals/h264_server.pid
CHILD_PID=
CONFIG_FILE="/useremain/home/rinkhals/apps/29-h264-streamer.config"

# Cleanup function to kill child process
cleanup() {
    rm -f "$PID_FILE" 2>/dev/null
    rm -f /tmp/h264_stream.fifo 2>/dev/null
    rm -f /tmp/h264_ctrl 2>/dev/null
    if [ -n "$CHILD_PID" ]; then
        kill $CHILD_PID 2>/dev/null
        wait $CHILD_PID 2>/dev/null
    fi
    exit 0
}

trap cleanup SIGTERM SIGINT SIGHUP EXIT

# Set library path for RKMPI encoder and ffmpeg
export LD_LIBRARY_PATH=/oem/usr/lib:/ac_lib/lib/third_lib:/userdata/app/gk:$LD_LIBRARY_PATH

# Verify rkmpi_enc binary exists
if [ ! -f "$APP_ROOT/rkmpi_enc" ]; then
    echo "ERROR: rkmpi_enc binary not found" >> "$APP_LOG"
    exit 1
fi
chmod +x "$APP_ROOT/rkmpi_enc"

# Read encoder type from config file (default: rkmpi)
ENCODER_TYPE="rkmpi"
if [ -f "$CONFIG_FILE" ]; then
    # Simple JSON extraction (no jq on printer)
    ET=$(sed -n 's/.*"encoder_type"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' "$CONFIG_FILE" 2>/dev/null)
    if [ -n "$ET" ]; then
        ENCODER_TYPE="$ET"
    fi
fi

# Read camera resolution from config (default: 1280x720)
CAMERA_RES="1280x720"
if [ -f "$CONFIG_FILE" ]; then
    RES=$(sed -n 's/.*"h264_resolution"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' "$CONFIG_FILE" 2>/dev/null)
    if [ -n "$RES" ]; then
        CAMERA_RES="$RES"
    fi
fi

# Read MJPEG FPS from config (default: 10)
MJPEG_FPS="10"
if [ -f "$CONFIG_FILE" ]; then
    FPS=$(sed -n 's/.*"mjpeg_fps"[[:space:]]*:[[:space:]]*"\{0,1\}\([0-9]*\).*/\1/p' "$CONFIG_FILE" 2>/dev/null)
    if [ -n "$FPS" ] && [ "$FPS" -gt 0 ] 2>/dev/null; then
        MJPEG_FPS="$FPS"
    fi
fi

# Read bitrate from config (default: 512)
BITRATE="512"
if [ -f "$CONFIG_FILE" ]; then
    BR=$(sed -n 's/.*"bitrate"[[:space:]]*:[[:space:]]*"\{0,1\}\([0-9]*\).*/\1/p' "$CONFIG_FILE" 2>/dev/null)
    if [ -n "$BR" ] && [ "$BR" -gt 0 ] 2>/dev/null; then
        BITRATE="$BR"
    fi
fi

# Read h264_enabled from config (default: true)
H264_FLAG=""
if [ -f "$CONFIG_FILE" ]; then
    H264=$(sed -n 's/.*"h264_enabled"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' "$CONFIG_FILE" 2>/dev/null)
    if [ "$H264" = "false" ]; then
        H264_FLAG="--no-h264"
    fi
fi

# Read display capture from config
DISPLAY_FLAG=""
DISPLAY_FPS=""
if [ -f "$CONFIG_FILE" ]; then
    DISP=$(sed -n 's/.*"display_enabled"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' "$CONFIG_FILE" 2>/dev/null)
    if [ "$DISP" = "true" ]; then
        DISPLAY_FLAG="--display"
        DFPS=$(sed -n 's/.*"display_fps"[[:space:]]*:[[:space:]]*"\{0,1\}\([0-9]*\).*/\1/p' "$CONFIG_FILE" 2>/dev/null)
        if [ -n "$DFPS" ] && [ "$DFPS" -gt 0 ] 2>/dev/null; then
            DISPLAY_FPS="--display-fps $DFPS"
        fi
    fi
fi

# Note: ACProxyCam FLV proxy mode is handled by control_server.
# The built-in FLV server always runs on the primary encoder.
FLV_FLAG=""

# Detect primary camera device
# Look for USB camera by path (filter for "usb" to exclude ISP devices)
CAMERA_DEVICE=""
for dev in /dev/v4l/by-path/*usb*-video-index0; do
    if [ -e "$dev" ]; then
        REAL_DEV=$(readlink -f "$dev")
        CAMERA_DEVICE="$REAL_DEV"
        break
    fi
done
if [ -z "$CAMERA_DEVICE" ]; then
    # Fallback to /dev/video10
    CAMERA_DEVICE="/dev/video10"
fi

# Parse resolution
CAM_WIDTH=$(echo "$CAMERA_RES" | cut -d'x' -f1)
CAM_HEIGHT=$(echo "$CAMERA_RES" | cut -d'x' -f2)

# Build encoder arguments
ENCODER_ARGS="-S -N -d $CAMERA_DEVICE -w $CAM_WIDTH -h $CAM_HEIGHT -f $MJPEG_FPS -b $BITRATE"

# Add YUYV flag for rkmpi-yuyv mode
if [ "$ENCODER_TYPE" = "rkmpi-yuyv" ]; then
    ENCODER_ARGS="$ENCODER_ARGS --yuyv"
fi

# Read auto-skip settings from config
if [ -f "$CONFIG_FILE" ]; then
    AS=$(sed -n 's/.*"auto_skip"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' "$CONFIG_FILE" 2>/dev/null)
    if [ "$AS" = "true" ]; then
        ENCODER_ARGS="$ENCODER_ARGS --auto-skip"
        TC=$(sed -n 's/.*"target_cpu"[[:space:]]*:[[:space:]]*"\{0,1\}\([0-9]*\).*/\1/p' "$CONFIG_FILE" 2>/dev/null)
        if [ -n "$TC" ] && [ "$TC" -gt 0 ] 2>/dev/null; then
            ENCODER_ARGS="$ENCODER_ARGS --target-cpu $TC"
        fi
    fi
    SR=$(sed -n 's/.*"skip_ratio"[[:space:]]*:[[:space:]]*"\{0,1\}\([0-9]*\).*/\1/p' "$CONFIG_FILE" 2>/dev/null)
    if [ -n "$SR" ] && [ "$SR" -gt 0 ] 2>/dev/null; then
        ENCODER_ARGS="$ENCODER_ARGS --skip $SR"
    fi
fi

# Read JPEG quality from config
if [ -f "$CONFIG_FILE" ]; then
    JQ=$(sed -n 's/.*"jpeg_quality"[[:space:]]*:[[:space:]]*"\{0,1\}\([0-9]*\).*/\1/p' "$CONFIG_FILE" 2>/dev/null)
    if [ -n "$JQ" ] && [ "$JQ" -gt 0 ] 2>/dev/null; then
        ENCODER_ARGS="$ENCODER_ARGS --jpeg-quality $JQ"
    fi
fi

# Add H264 resolution if different from camera
if [ "$CAMERA_RES" != "${CAM_WIDTH}x${CAM_HEIGHT}" ] || [ "$CAMERA_RES" != "1280x720" ]; then
    # Only add if not default
    if [ "$CAMERA_RES" != "1280x720" ]; then
        ENCODER_ARGS="$ENCODER_ARGS --h264-resolution $CAMERA_RES"
    fi
fi

# Vanilla-klipper mode
if [ "$APP_MODE" = "--mode vanilla-klipper" ]; then
    ENCODER_ARGS="$ENCODER_ARGS --mode vanilla-klipper"
fi

# Add optional streaming/control port overrides
if [ -n "$APP_STREAMING_PORT" ]; then
    ENCODER_ARGS="$ENCODER_ARGS $APP_STREAMING_PORT"
fi
if [ -n "$APP_CONTROL_PORT" ]; then
    ENCODER_ARGS="$ENCODER_ARGS $APP_CONTROL_PORT"
fi

echo "Starting rkmpi_enc (primary mode)" >> "$APP_LOG"
echo "  Encoder type: $ENCODER_TYPE" >> "$APP_LOG"
echo "  Camera: $CAMERA_DEVICE ${CAM_WIDTH}x${CAM_HEIGHT}" >> "$APP_LOG"
echo "  Config: $CONFIG_FILE" >> "$APP_LOG"
echo "  Args: $ENCODER_ARGS $H264_FLAG $DISPLAY_FLAG $DISPLAY_FPS $FLV_FLAG" >> "$APP_LOG"

# shellcheck disable=SC2086  # Intentional word splitting
"$APP_ROOT/rkmpi_enc" --primary --config "$CONFIG_FILE" \
    --template-dir "$APP_ROOT" \
    $ENCODER_ARGS $H264_FLAG $DISPLAY_FLAG $DISPLAY_FPS $FLV_FLAG \
    -v >> "$APP_LOG" 2>&1 &
CHILD_PID=$!
echo $CHILD_PID > "$PID_FILE"

# Wait for child to exit
wait $CHILD_PID
rm -f "$PID_FILE" 2>/dev/null
