#!/bin/sh
#
# test_timelapse.sh - Simulate a timelapse recording without a real print
#
# Sends timelapse commands via the h264_cmd command file to capture frames
# and produce an MP4 video. Useful for testing the timelapse pipeline
# (capture, VENC encoding, MP4 muxing, thumbnail generation) end-to-end.
#
# Usage:
#   Run on the printer (or via SSH):
#     ./test_timelapse.sh [frames] [name]
#
#   Examples:
#     ./test_timelapse.sh              # 200 frames, default name
#     ./test_timelapse.sh 50           # 50 frames
#     ./test_timelapse.sh 100 my_test  # 100 frames, custom name
#
#   Via SSH from dev machine:
#     sshpass -p 'rockchip' scp scripts/test_timelapse.sh root@PRINTER:/tmp/
#     sshpass -p 'rockchip' ssh root@PRINTER /tmp/test_timelapse.sh
#
# Notes:
#   - Captures one frame per second to avoid duplicate detection
#     (static scenes produce identical frames that get skipped)
#   - Move the nozzle or wave something at the camera for best results
#   - Output goes to the configured timelapse directory (internal storage
#     when using command file directly, since configure_timelapse() is
#     only called by the Moonraker print-start path)
#   - The encoder must be running (app.sh start) before running this script
#   - At 30fps output, 200 frames = ~6.7 second video
#

CMD_FILE=/tmp/h264_cmd
LOG=/tmp/rinkhals/app-h264-streamer.log
FRAMES=${1:-200}
NAME=${2:-test_timelapse}

# Verify encoder is running
if ! [ -e "$CMD_FILE" ] && ! pidof rkmpi_enc >/dev/null 2>&1; then
    echo "Error: rkmpi_enc not running. Start h264-streamer first."
    exit 1
fi

echo "=== Timelapse test: $FRAMES frames, name=$NAME ==="
echo "Tip: move the nozzle or wave at the camera for unique frames"
echo ""

# Initialize timelapse session
echo "timelapse_init:$NAME" > $CMD_FILE
sleep 2

# Capture one frame per second
# At 1fps the camera always produces a new frame (avoids duplicate skip)
i=0
while [ $i -lt $FRAMES ]; do
    echo "timelapse_capture" > $CMD_FILE
    i=$((i + 1))
    if [ $((i % 25)) -eq 0 ]; then
        echo "  Progress: $i/$FRAMES frames"
    fi
    sleep 1
done

echo ""
echo "Finalizing $FRAMES frames..."
sleep 1
echo "timelapse_finalize" > $CMD_FILE

# Wait for VENC encoding to complete
echo "Waiting for VENC encoding..."
for j in $(seq 1 120); do
    sleep 1
    if tail -3 "$LOG" 2>/dev/null | grep -q "Cleaning up temp"; then
        echo ""
        echo "=== Complete ==="
        grep "TIMELAPSE: Finalizing" "$LOG" | tail -1
        grep "TIMELAPSE: VENC created" "$LOG" | tail -1
        grep "TIMELAPSE: Created thumbnail" "$LOG" | tail -1
        echo ""
        echo "Output files:"
        # Find the most recent files matching the name
        ls -lht /useremain/app/gk/Time-lapse-Video/${NAME}* 2>/dev/null | head -4
        exit 0
    fi
done

echo ""
echo "Timeout waiting for encoding. Recent log:"
grep "TIMELAPSE:" "$LOG" | tail -10
exit 1
