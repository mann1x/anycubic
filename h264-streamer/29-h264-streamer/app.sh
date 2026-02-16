. /useremain/rinkhals/.current/tools.sh

export APP_ROOT="$(dirname "$(realpath "$0")")"
export APP_NAME="$(basename "$APP_ROOT")"

# Check mode property (default go-klipper)
MODE="$(get_app_property "$APP_NAME" mode)"
if [ "$MODE" = "vanilla-klipper" ]; then
    export APP_MODE="--mode vanilla-klipper"
else
    export APP_MODE="--mode go-klipper"
fi

# Check streaming_port property (default 8080)
STREAMING_PORT="$(get_app_property "$APP_NAME" streaming_port)"
if [ -n "$STREAMING_PORT" ] && [ "$STREAMING_PORT" != "null" ]; then
    export APP_STREAMING_PORT="--streaming-port $STREAMING_PORT"
else
    export APP_STREAMING_PORT=""
fi

# Check control_port property (default 8081)
CONTROL_PORT="$(get_app_property "$APP_NAME" control_port)"
if [ -n "$CONTROL_PORT" ] && [ "$CONTROL_PORT" != "null" ]; then
    export APP_CONTROL_PORT="--control-port $CONTROL_PORT"
else
    export APP_CONTROL_PORT=""
fi

# Check logging property (default false = no logging)
LOGGING="$(get_app_property "$APP_NAME" logging)"
if [ "$LOGGING" = "true" ]; then
    export APP_LOG="$RINKHALS_LOGS/app-h264-streamer.log"
else
    export APP_LOG=/dev/null
fi

# Check autolanmode property (default true)
AUTOLANMODE="$(get_app_property "$APP_NAME" autolanmode)"
if [ "$AUTOLANMODE" = "false" ]; then
    export APP_AUTOLANMODE="--no-autolanmode"
else
    export APP_AUTOLANMODE="--autolanmode"
fi

# Check auto_skip property (default true)
AUTO_SKIP="$(get_app_property "$APP_NAME" auto_skip)"
if [ "$AUTO_SKIP" = "false" ]; then
    export APP_AUTO_SKIP=""
else
    export APP_AUTO_SKIP="--auto-skip"
fi

# Check target_cpu property (default 25)
TARGET_CPU="$(get_app_property "$APP_NAME" target_cpu)"
if [ -n "$TARGET_CPU" ] && [ "$TARGET_CPU" != "null" ]; then
    export APP_TARGET_CPU="--target-cpu $TARGET_CPU"
else
    export APP_TARGET_CPU="--target-cpu 25"
fi

# Check skip_ratio property (default 4 = 25%)
SKIP_RATIO="$(get_app_property "$APP_NAME" skip_ratio)"
if [ -n "$SKIP_RATIO" ] && [ "$SKIP_RATIO" != "null" ]; then
    export APP_SKIP_RATIO="--skip-ratio $SKIP_RATIO"
else
    export APP_SKIP_RATIO=""
fi

# Check port property (default 8080)
PORT="$(get_app_property "$APP_NAME" port)"
if [ -n "$PORT" ] && [ "$PORT" != "null" ]; then
    export APP_PORT="--port $PORT"
else
    export APP_PORT=""
fi

# Check encoder_type property (default rkmpi-yuyv)
ENCODER_TYPE="$(get_app_property "$APP_NAME" encoder_type)"
if [ "$ENCODER_TYPE" = "gkcam" ]; then
    export APP_ENCODER_TYPE="--encoder-type gkcam"
elif [ "$ENCODER_TYPE" = "rkmpi" ]; then
    export APP_ENCODER_TYPE="--encoder-type rkmpi"
else
    export APP_ENCODER_TYPE="--encoder-type rkmpi-yuyv"
fi

# Check jpeg_quality property (default 85, only used in rkmpi-yuyv mode)
JPEG_QUALITY="$(get_app_property "$APP_NAME" jpeg_quality)"
if [ -n "$JPEG_QUALITY" ] && [ "$JPEG_QUALITY" != "null" ]; then
    export APP_JPEG_QUALITY="--jpeg-quality $JPEG_QUALITY"
else
    export APP_JPEG_QUALITY=""
fi

# Check gkcam_all_frames property (default false = keyframes only)
GKCAM_ALL_FRAMES="$(get_app_property "$APP_NAME" gkcam_all_frames)"
if [ "$GKCAM_ALL_FRAMES" = "true" ]; then
    export APP_GKCAM_ALL_FRAMES="--gkcam-all-frames"
else
    export APP_GKCAM_ALL_FRAMES=""
fi

# Check bitrate property (default 512 kbps)
BITRATE="$(get_app_property "$APP_NAME" bitrate)"
if [ -n "$BITRATE" ] && [ "$BITRATE" != "null" ]; then
    export APP_BITRATE="--bitrate $BITRATE"
else
    export APP_BITRATE=""
fi

# Check mjpeg_fps property (default 10)
MJPEG_FPS="$(get_app_property "$APP_NAME" mjpeg_fps)"
if [ -n "$MJPEG_FPS" ] && [ "$MJPEG_FPS" != "null" ]; then
    export APP_MJPEG_FPS="--mjpeg-fps $MJPEG_FPS"
else
    export APP_MJPEG_FPS=""
fi

# Check h264_resolution property (default 1280x720, only for rkmpi mode)
H264_RESOLUTION="$(get_app_property "$APP_NAME" h264_resolution)"
if [ -n "$H264_RESOLUTION" ] && [ "$H264_RESOLUTION" != "null" ] && [ "$H264_RESOLUTION" != "1280x720" ]; then
    export APP_H264_RESOLUTION="--h264-resolution $H264_RESOLUTION"
else
    export APP_H264_RESOLUTION=""
fi

status() {
    PIDS=$(get_by_name h264_monitor)

    if [ "$PIDS" = "" ]; then
        report_status $APP_STATUS_STOPPED
    else
        # Read PID from file (ps output truncates long python paths)
        PID_FILE=/tmp/rinkhals/h264_server.pid
        if [ -f "$PID_FILE" ]; then
            PIDS=$(cat "$PID_FILE")
        fi
        report_status $APP_STATUS_STARTED "$PIDS"
    fi
}
start() {
    cd "$APP_ROOT"
    echo "Starting h264-streamer app" >> "$APP_LOG"

    PIDS=$(get_by_name h264_monitor)
    if [ "$PIDS" = "" ]; then
        chmod +x ./h264_monitor.sh
        ./h264_monitor.sh >> "$APP_LOG" 2>&1 &
    fi
}
debug() {
    kill_by_name h264_monitor
    kill_by_name h264_server
    kill_by_name rkmpi_enc

    cd "$APP_ROOT"

    chmod +x ./h264_monitor.sh
    ./h264_monitor.sh
}
stop() {
    # In go-klipper mode, kill gkcam first (like mjpg-streamer does) to ensure clean state
    if [ "$MODE" != "vanilla-klipper" ]; then
        kill_by_name gkcam
    fi

    # Stop our processes
    # Use SIGTERM first to allow cleanup, then SIGKILL
    kill_by_name rkmpi_enc 15
    kill_by_name h264_server 15
    kill_by_name h264_monitor 15
    sleep 1
    # Force kill if still running
    kill_by_name rkmpi_enc 9
    kill_by_name h264_server 9
    kill_by_name h264_monitor 9

    # Cleanup FIFOs
    rm -f /tmp/h264_stream.fifo
    rm -f /tmp/mjpeg.pipe
    rm -f /tmp/h264.pipe

    sleep 2

    # Restart gkcam fresh (go-klipper mode only)
    if [ "$MODE" != "vanilla-klipper" ]; then
        echo "Restarting gkcam..." >> "$APP_LOG"
        cd /userdata/app/gk
        LD_LIBRARY_PATH=/userdata/app/gk:$LD_LIBRARY_PATH \
            ./gkcam >> "$RINKHALS_LOGS/gkcam.log" 2>&1 &
    else
        echo "Vanilla-klipper mode: skipping gkcam restart" >> "$APP_LOG"
    fi
}
restart() {
    # In go-klipper mode, kill gkcam first (like mjpg-streamer does) to ensure clean state
    if [ "$MODE" != "vanilla-klipper" ]; then
        kill_by_name gkcam
    fi

    # Stop our processes
    # Use SIGTERM first to allow cleanup, then SIGKILL
    kill_by_name rkmpi_enc 15
    kill_by_name h264_server 15
    kill_by_name h264_monitor 15
    sleep 1
    # Force kill if still running
    kill_by_name rkmpi_enc 9
    kill_by_name h264_server 9
    kill_by_name h264_monitor 9

    # Cleanup FIFOs
    rm -f /tmp/h264_stream.fifo
    rm -f /tmp/mjpeg.pipe
    rm -f /tmp/h264.pipe

    cd "$APP_ROOT"
    echo "Restarting h264-streamer app" >> "$APP_LOG"

    PIDS=$(get_by_name h264_monitor)
    if [ "$PIDS" = "" ]; then
        chmod +x ./h264_monitor.sh
        ./h264_monitor.sh >> "$APP_LOG" 2>&1 &
    fi
}

case "$1" in
    status)
        status
        ;;
    start)
        start
        ;;
    restart)
        restart
        ;;
    debug)
        shift
        debug "$@"
        ;;
    stop)
        stop
        ;;
    *)
        echo "Usage: $0 {status|start|stop|restart|debug}" >&2
        exit 1
        ;;
esac
