#!/bin/sh
#
# Auto-skip CPU test script
# Tests the auto-skip feature at different target CPU levels
# Creates variable CPU load and measures resulting skip_ratio, encoder CPU, H.264 FPS
#

PRINTER_IP="${1:-192.168.178.43}"
API_URL="http://$PRINTER_IP:8080"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Stress process PIDs
STRESS_PIDS=""

cleanup() {
    echo "Cleaning up..."
    # Kill all stress processes
    for pid in $STRESS_PIDS; do
        kill $pid 2>/dev/null
    done
    # Reset auto_skip to off
    curl -s -X POST -d "autolanmode=1&h264_enabled=1&skip_ratio=2&auto_skip=0&target_cpu=60" "$API_URL/control" >/dev/null
    exit 0
}

trap cleanup INT TERM

# Function to create CPU load on the printer
# Usage: create_load <percentage>
create_load() {
    local target_pct=$1

    # Kill existing stress processes
    for pid in $STRESS_PIDS; do
        kill $pid 2>/dev/null
    done
    STRESS_PIDS=""

    if [ "$target_pct" -le 0 ]; then
        return
    fi

    # Start stress processes on printer
    # Each busy loop uses ~100% of available CPU time
    # We use duty cycle to control percentage
    local duty=$target_pct

    sshpass -p 'rockchip' ssh -o StrictHostKeyChecking=no root@$PRINTER_IP "
        # Kill any existing stress processes
        for p in \$(ps | grep 'stress_loop' | grep -v grep | awk '{print \$1}'); do
            kill \$p 2>/dev/null
        done

        # Create stress loop with duty cycle
        (
            while true; do
                # Busy work for duty% of time
                end=\$(( \$(date +%s%N) + ${duty}000000 ))
                while [ \$(date +%s%N) -lt \$end ]; do
                    : # busy loop
                done
                # Sleep for (100-duty)% of time
                usleep \$(( (100 - ${duty}) * 1000 )) 2>/dev/null || sleep 0.\$(( 100 - ${duty} ))
            done
        ) &
        echo \$!
    " 2>/dev/null &

    STRESS_PIDS="$STRESS_PIDS $!"
    sleep 1
}

# Function to get current stats
get_stats() {
    curl -s "$API_URL/api/stats" 2>/dev/null
}

# Function to set auto-skip target
set_target_cpu() {
    local target=$1
    curl -s -X POST -d "autolanmode=1&h264_enabled=1&skip_ratio=2&auto_skip=1&target_cpu=$target" "$API_URL/control" >/dev/null
    sleep 2
}

# Function to disable auto-skip
disable_auto_skip() {
    curl -s -X POST -d "autolanmode=1&h264_enabled=1&skip_ratio=2&auto_skip=0&target_cpu=60" "$API_URL/control" >/dev/null
}

# Main test function
run_test() {
    local target_cpu=$1
    local load_pct=$2

    # Set target CPU
    set_target_cpu $target_cpu

    # Create load
    create_load $load_pct

    # Wait for auto-skip to stabilize
    sleep 5

    # Get stats (average of 3 samples)
    local total_cpu=0
    local encoder_cpu=0
    local h264_fps=0
    local skip_ratio=0
    local samples=3

    for i in $(seq 1 $samples); do
        stats=$(get_stats)

        cpu=$(echo "$stats" | python3 -c "import sys,json; d=json.load(sys.stdin); print(d['cpu']['total'])" 2>/dev/null)
        enc=$(echo "$stats" | python3 -c "import sys,json; d=json.load(sys.stdin); procs=d['cpu']['processes']; pids=[k for k in procs.keys()]; print(procs[pids[0]] if pids else 0)" 2>/dev/null)
        fps=$(echo "$stats" | python3 -c "import sys,json; d=json.load(sys.stdin); print(d['fps']['h264'])" 2>/dev/null)
        skip=$(echo "$stats" | python3 -c "import sys,json; d=json.load(sys.stdin); print(d['skip_ratio'])" 2>/dev/null)

        total_cpu=$(echo "$total_cpu + $cpu" | bc 2>/dev/null || echo "$total_cpu")
        encoder_cpu=$(echo "$encoder_cpu + $enc" | bc 2>/dev/null || echo "$encoder_cpu")
        h264_fps=$(echo "$h264_fps + $fps" | bc 2>/dev/null || echo "$h264_fps")
        skip_ratio=$skip

        sleep 1
    done

    # Calculate averages
    total_cpu=$(echo "scale=1; $total_cpu / $samples" | bc 2>/dev/null || echo "$total_cpu")
    encoder_cpu=$(echo "scale=1; $encoder_cpu / $samples" | bc 2>/dev/null || echo "$encoder_cpu")
    h264_fps=$(echo "scale=1; $h264_fps / $samples" | bc 2>/dev/null || echo "$h264_fps")

    # Calculate free CPU
    free_cpu=$(echo "scale=1; 100 - $total_cpu" | bc 2>/dev/null || echo "0")

    # Check if target met (within 10% tolerance)
    local status="OK"
    target_free=$(echo "100 - $target_cpu" | bc)
    if [ $(echo "$free_cpu < $target_free - 10" | bc 2>/dev/null || echo "0") -eq 1 ]; then
        status="LOW"
    fi

    echo "$target_cpu|$load_pct|$skip_ratio|$total_cpu|$encoder_cpu|$h264_fps|$free_cpu|$status"
}

# Print header
echo ""
echo "====================================================================="
echo "  Auto-Skip CPU Test - Testing at different target CPU levels"
echo "====================================================================="
echo ""
echo "Printer: $PRINTER_IP"
echo ""

# Check connectivity
if ! curl -s "$API_URL/status" >/dev/null 2>&1; then
    echo "ERROR: Cannot connect to printer at $PRINTER_IP"
    exit 1
fi

echo "Connected to printer. Starting tests..."
echo ""

# Table header
printf "%-12s | %-10s | %-10s | %-10s | %-12s | %-10s | %-10s | %-8s\n" \
    "Target CPU" "Load %" "Skip Ratio" "Total CPU" "Encoder CPU" "H.264 FPS" "Free CPU" "Status"
printf "%s\n" "-------------|------------|------------|------------|--------------|------------|------------|----------"

# Test each target CPU level
for target in 30 40 50 60 70 80; do
    # Test with different load levels (0, 20, 40, 60%)
    for load in 0 20 40 60; do
        result=$(run_test $target $load)

        IFS='|' read -r t_cpu l_pct skip tot_cpu enc_cpu fps free stat <<EOF
$result
EOF

        # Color code status
        if [ "$stat" = "OK" ]; then
            stat_color="${GREEN}OK${NC}"
        else
            stat_color="${RED}LOW${NC}"
        fi

        printf "%-12s | %-10s | %-10s | %-10s | %-12s | %-10s | %-10s | %b\n" \
            "${t_cpu}%" "${l_pct}%" "$skip" "${tot_cpu}%" "${enc_cpu}%" "$fps" "${free}%" "$stat_color"
    done
    echo ""
done

# Cleanup
cleanup
