. /useremain/rinkhals/.current/tools.sh

APP_ROOT=$(dirname $(realpath $0))
PID_FILE=/tmp/rinkhals/h264_server.pid
CHILD_PID=

# Cleanup function to kill child process
cleanup() {
    rm -f $PID_FILE 2>/dev/null
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

# Check rkmpi_enc binary only if using rkmpi encoder type
if [ "$APP_ENCODER_TYPE" = "--encoder-type rkmpi" ]; then
    if [ ! -f "$APP_ROOT/rkmpi_enc" ]; then
        echo "ERROR: rkmpi_enc binary not found" >> $APP_LOG
        exit 1
    fi
    chmod +x "$APP_ROOT/rkmpi_enc"
    echo "Using encoder: rkmpi ($APP_ROOT/rkmpi_enc)" >> $APP_LOG
else
    echo "Using encoder: gkcam (via ffmpeg)" >> $APP_LOG
fi

echo "Mode: $APP_MODE" >> $APP_LOG
echo "Streaming port: $APP_STREAMING_PORT" >> $APP_LOG
echo "Control port: $APP_CONTROL_PORT" >> $APP_LOG
echo "Encoder type: $APP_ENCODER_TYPE" >> $APP_LOG
echo "Gkcam all frames: $APP_GKCAM_ALL_FRAMES" >> $APP_LOG
echo "Autolanmode: $APP_AUTOLANMODE" >> $APP_LOG
echo "Auto-skip: $APP_AUTO_SKIP $APP_TARGET_CPU" >> $APP_LOG
echo "Skip ratio: $APP_SKIP_RATIO" >> $APP_LOG
echo "Port: $APP_PORT" >> $APP_LOG
echo "Bitrate: $APP_BITRATE" >> $APP_LOG
echo "MJPEG FPS: $APP_MJPEG_FPS" >> $APP_LOG
echo "JPEG Quality: $APP_JPEG_QUALITY" >> $APP_LOG
echo "H.264 Resolution: $APP_H264_RESOLUTION" >> $APP_LOG

# Start Python H.264 server
echo "Starting h264_server.py" >> $APP_LOG
chmod +x $APP_ROOT/h264_server.py
python $APP_ROOT/h264_server.py $APP_MODE $APP_STREAMING_PORT $APP_CONTROL_PORT $APP_ENCODER_TYPE $APP_GKCAM_ALL_FRAMES $APP_AUTOLANMODE $APP_AUTO_SKIP $APP_TARGET_CPU $APP_SKIP_RATIO $APP_PORT $APP_BITRATE $APP_MJPEG_FPS $APP_JPEG_QUALITY $APP_H264_RESOLUTION >> $APP_LOG 2>&1 &
CHILD_PID=$!
echo $CHILD_PID > $PID_FILE

# Wait for child to exit
wait $CHILD_PID
rm -f $PID_FILE 2>/dev/null
