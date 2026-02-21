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

# Note: encoder settings (encoder_type, bitrate, mjpeg_fps, h264_resolution,
# auto_skip, target_cpu, skip_ratio, jpeg_quality, autolanmode, display, timelapse)
# are now read from the JSON config file by h264_monitor.sh directly.

status() {
    PIDS=$(get_by_name h264_monitor)

    if [ "$PIDS" = "" ]; then
        report_status $APP_STATUS_STOPPED
    else
        # Read PID from file for accurate reporting
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

    # Kill gkcam to free camera/ISP resources (required in all modes)
    kill_by_name gkcam

    PIDS=$(get_by_name h264_monitor)
    if [ "$PIDS" = "" ]; then
        chmod +x ./h264_monitor.sh
        ./h264_monitor.sh >> "$APP_LOG" 2>&1 &
    fi
}
debug() {
    kill_by_name h264_monitor
    kill_by_name rkmpi_enc

    cd "$APP_ROOT"

    chmod +x ./h264_monitor.sh
    ./h264_monitor.sh
}
stop() {
    # Kill gkcam to ensure clean state (required in all modes)
    kill_by_name gkcam

    # Stop our processes
    # Use SIGTERM first to allow cleanup, then SIGKILL
    kill_by_name rkmpi_enc 15
    kill_by_name h264_monitor 15
    sleep 1
    # Force kill if still running
    kill_by_name rkmpi_enc 9
    kill_by_name h264_monitor 9

    # Cleanup FIFOs and control files
    rm -f /tmp/h264_stream.fifo
    rm -f /tmp/h264_cmd /tmp/h264_ctrl
    rm -f /tmp/h264_cmd_[0-9] /tmp/h264_ctrl_[0-9]

    sleep 2

    # Restart gkcam so stock camera service works again
    echo "Restarting gkcam..." >> "$APP_LOG"
    cd /userdata/app/gk
    LD_LIBRARY_PATH=/userdata/app/gk:$LD_LIBRARY_PATH \
        ./gkcam >> "$RINKHALS_LOGS/gkcam.log" 2>&1 &
}
restart() {
    # Kill gkcam to free camera/ISP resources (required in all modes)
    kill_by_name gkcam

    # Stop our processes
    # Use SIGTERM first to allow cleanup, then SIGKILL
    kill_by_name rkmpi_enc 15
    kill_by_name h264_monitor 15
    sleep 1
    # Force kill if still running
    kill_by_name rkmpi_enc 9
    kill_by_name h264_monitor 9

    # Cleanup FIFOs and control files
    rm -f /tmp/h264_stream.fifo
    rm -f /tmp/h264_cmd /tmp/h264_ctrl
    rm -f /tmp/h264_cmd_[0-9] /tmp/h264_ctrl_[0-9]

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
