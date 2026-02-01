. /useremain/rinkhals/.current/tools.sh

export APP_ROOT=$(dirname $(realpath $0))
export APP_NAME=$(basename $APP_ROOT)

# Check logging property (default false = no logging)
LOGGING=$(get_app_property $APP_NAME logging)
if [ "$LOGGING" = "true" ]; then
    export APP_LOG=$RINKHALS_LOGS/app-h264-streamer.log
else
    export APP_LOG=/dev/null
fi

# Check autolanmode property (default true)
AUTOLANMODE=$(get_app_property $APP_NAME autolanmode)
if [ "$AUTOLANMODE" = "false" ]; then
    export APP_AUTOLANMODE="--no-autolanmode"
else
    export APP_AUTOLANMODE="--autolanmode"
fi

# Check auto_skip property (default true)
AUTO_SKIP=$(get_app_property $APP_NAME auto_skip)
if [ "$AUTO_SKIP" = "false" ]; then
    export APP_AUTO_SKIP=""
else
    export APP_AUTO_SKIP="--auto-skip"
fi

# Check target_cpu property (default 25)
TARGET_CPU=$(get_app_property $APP_NAME target_cpu)
if [ -n "$TARGET_CPU" ] && [ "$TARGET_CPU" != "null" ]; then
    export APP_TARGET_CPU="--target-cpu $TARGET_CPU"
else
    export APP_TARGET_CPU="--target-cpu 25"
fi

# Check skip_ratio property (default 4 = 25%)
SKIP_RATIO=$(get_app_property $APP_NAME skip_ratio)
if [ -n "$SKIP_RATIO" ] && [ "$SKIP_RATIO" != "null" ]; then
    export APP_SKIP_RATIO="--skip-ratio $SKIP_RATIO"
else
    export APP_SKIP_RATIO=""
fi

# Check port property (default 8080)
PORT=$(get_app_property $APP_NAME port)
if [ -n "$PORT" ] && [ "$PORT" != "null" ]; then
    export APP_PORT="--port $PORT"
else
    export APP_PORT=""
fi

# Check encoder_type property (default rkmpi-yuyv)
ENCODER_TYPE=$(get_app_property $APP_NAME encoder_type)
if [ "$ENCODER_TYPE" = "gkcam" ]; then
    export APP_ENCODER_TYPE="--encoder-type gkcam"
elif [ "$ENCODER_TYPE" = "rkmpi" ]; then
    export APP_ENCODER_TYPE="--encoder-type rkmpi"
else
    export APP_ENCODER_TYPE="--encoder-type rkmpi-yuyv"
fi

# Check jpeg_quality property (default 85, only used in rkmpi-yuyv mode)
JPEG_QUALITY=$(get_app_property $APP_NAME jpeg_quality)
if [ -n "$JPEG_QUALITY" ] && [ "$JPEG_QUALITY" != "null" ]; then
    export APP_JPEG_QUALITY="--jpeg-quality $JPEG_QUALITY"
else
    export APP_JPEG_QUALITY=""
fi

# Check gkcam_all_frames property (default false = keyframes only)
GKCAM_ALL_FRAMES=$(get_app_property $APP_NAME gkcam_all_frames)
if [ "$GKCAM_ALL_FRAMES" = "true" ]; then
    export APP_GKCAM_ALL_FRAMES="--gkcam-all-frames"
else
    export APP_GKCAM_ALL_FRAMES=""
fi

# Check bitrate property (default 512 kbps)
BITRATE=$(get_app_property $APP_NAME bitrate)
if [ -n "$BITRATE" ] && [ "$BITRATE" != "null" ]; then
    export APP_BITRATE="--bitrate $BITRATE"
else
    export APP_BITRATE=""
fi

# Check mjpeg_fps property (default 10)
MJPEG_FPS=$(get_app_property $APP_NAME mjpeg_fps)
if [ -n "$MJPEG_FPS" ] && [ "$MJPEG_FPS" != "null" ]; then
    export APP_MJPEG_FPS="--mjpeg-fps $MJPEG_FPS"
else
    export APP_MJPEG_FPS=""
fi

status() {
    PIDS=$(get_by_name h264_monitor)

    if [ "$PIDS" = "" ]; then
        report_status $APP_STATUS_STOPPED
    else
        # Read PID from file (ps output truncates long python paths)
        PID_FILE=/tmp/rinkhals/h264_server.pid
        if [ -f "$PID_FILE" ]; then
            PIDS=$(cat $PID_FILE)
        fi
        report_status $APP_STATUS_STARTED "$PIDS"
    fi
}
start() {
    cd $APP_ROOT
    echo "Starting h264-streamer app" >> $APP_LOG

    PIDS=$(get_by_name h264_monitor)
    if [ "$PIDS" = "" ]; then
        chmod +x ./h264_monitor.sh
        ./h264_monitor.sh >> $APP_LOG 2>&1 &
    fi
}
debug() {
    kill_by_name h264_monitor
    kill_by_name h264_server
    kill_by_name rkmpi_enc

    cd $APP_ROOT

    chmod +x ./h264_monitor.sh
    ./h264_monitor.sh
}
stop() {
    # Kill gkcam first (like mjpg-streamer does) to ensure clean state
    kill_by_name gkcam

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

    # Restart gkcam fresh (ensures clean state after any mode)
    echo "Restarting gkcam..." >> $APP_LOG
    cd /userdata/app/gk
    LD_LIBRARY_PATH=/userdata/app/gk:$LD_LIBRARY_PATH \
        ./gkcam >> $RINKHALS_LOGS/gkcam.log 2>&1 &
}

case "$1" in
    status)
        status
        ;;
    start)
        start
        ;;
    debug)
        shift
        debug $@
        ;;
    stop)
        stop
        ;;
    *)
        echo "Usage: $0 {status|start|stop}" >&2
        exit 1
        ;;
esac
