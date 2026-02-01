#!/usr/bin/env python3
"""
Auto-skip CPU test script
Tests the auto-skip feature at different target CPU levels by creating
variable CPU load and measuring resulting skip_ratio, encoder CPU, H.264 FPS.

Usage: python3 test_auto_skip.py [printer_ip]
"""

import json
import os
import subprocess
import sys
import time
import urllib.request
from urllib.parse import urlencode

PRINTER_IP = sys.argv[1] if len(sys.argv) > 1 else "192.168.178.43"
API_URL = f"http://{PRINTER_IP}:8080"
SSH_PASS = "rockchip"

# Store stress process info
stress_running = False


def ssh_cmd(cmd, background=False):
    """Execute command on printer via SSH"""
    ssh = f"sshpass -p '{SSH_PASS}' ssh -o StrictHostKeyChecking=no root@{PRINTER_IP}"
    if background:
        full_cmd = f"{ssh} '{cmd}' &"
    else:
        full_cmd = f"{ssh} '{cmd}'"
    try:
        result = subprocess.run(full_cmd, shell=True, capture_output=True, text=True, timeout=10)
        return result.stdout.strip()
    except Exception as e:
        return ""


def get_stats():
    """Get current stats from API"""
    try:
        with urllib.request.urlopen(f"{API_URL}/api/stats", timeout=5) as resp:
            return json.loads(resp.read().decode())
    except Exception as e:
        print(f"Error getting stats: {e}")
        return None


def set_settings(auto_skip=True, target_cpu=60, skip_ratio=2):
    """Set auto-skip settings via API"""
    data = urlencode({
        'autolanmode': '1',
        'h264_enabled': '1',
        'skip_ratio': str(skip_ratio),
        'auto_skip': '1' if auto_skip else '0',
        'target_cpu': str(target_cpu)
    }).encode()
    try:
        req = urllib.request.Request(f"{API_URL}/control", data=data, method='POST')
        with urllib.request.urlopen(req, timeout=5):
            pass
        time.sleep(1)
        return True
    except Exception as e:
        print(f"Error setting config: {e}")
        return False


def create_cpu_load(load_percent):
    """Create CPU load on printer using busy loop"""
    global stress_running

    # Kill existing stress processes
    ssh_cmd("for p in $(ps | grep 'cpu_stress' | grep -v grep | awk '{print $1}'); do kill $p 2>/dev/null; done")
    stress_running = False

    if load_percent <= 0:
        return

    # Create a stress script on the printer
    # Uses a duty cycle approach: busy for load%, sleep for (100-load)%
    stress_script = f"""
#!/bin/sh
# cpu_stress script
while true; do
    # Busy loop for {load_percent}ms
    i=0
    while [ $i -lt {load_percent * 100} ]; do
        i=$((i+1))
    done
    # Sleep for {100 - load_percent}ms
    usleep {(100 - load_percent) * 1000} 2>/dev/null || true
done
"""
    # Write and execute the stress script
    ssh_cmd(f"cat > /tmp/cpu_stress.sh << 'ENDSCRIPT'\n{stress_script}\nENDSCRIPT")
    ssh_cmd("chmod +x /tmp/cpu_stress.sh && /tmp/cpu_stress.sh &", background=True)
    stress_running = True
    time.sleep(1)


def stop_cpu_load():
    """Stop CPU load"""
    global stress_running
    ssh_cmd("for p in $(ps | grep 'cpu_stress' | grep -v grep | awk '{print $1}'); do kill $p 2>/dev/null; done")
    stress_running = False


def measure_stats(samples=5, delay=1):
    """Measure average stats over multiple samples"""
    total_cpu_sum = 0
    encoder_cpu_sum = 0
    h264_fps_sum = 0
    skip_ratio = 0
    valid_samples = 0

    for i in range(samples):
        time.sleep(delay)
        stats = get_stats()
        if stats:
            total_cpu_sum += stats['cpu']['total']
            # Get encoder CPU (first non-self process)
            procs = stats['cpu']['processes']
            if procs:
                encoder_cpu_sum += list(procs.values())[0]
            h264_fps_sum += stats['fps']['h264']
            skip_ratio = stats['skip_ratio']
            valid_samples += 1

    if valid_samples == 0:
        return None

    return {
        'total_cpu': round(total_cpu_sum / valid_samples, 1),
        'encoder_cpu': round(encoder_cpu_sum / valid_samples, 1),
        'h264_fps': round(h264_fps_sum / valid_samples, 1),
        'skip_ratio': skip_ratio,
        'free_cpu': round(100 - total_cpu_sum / valid_samples, 1)
    }


def run_test(target_cpu, additional_load):
    """Run a single test with given target CPU and additional load"""
    # Set auto-skip with target
    set_settings(auto_skip=True, target_cpu=target_cpu)

    # Create additional CPU load
    create_cpu_load(additional_load)

    # Wait for auto-skip to stabilize (encoder checks every 5 seconds)
    time.sleep(8)

    # Measure stats
    stats = measure_stats(samples=5, delay=1)

    return stats


def main():
    print()
    print("=" * 100)
    print("  Auto-Skip CPU Test - Testing at different target CPU levels")
    print("=" * 100)
    print()
    print(f"Printer: {PRINTER_IP}")
    print()

    # Check connectivity
    stats = get_stats()
    if not stats:
        print(f"ERROR: Cannot connect to printer at {PRINTER_IP}")
        sys.exit(1)

    print("Connected to printer. Starting tests...")
    print()

    # Results table
    results = []

    # Table header
    print(f"{'Target CPU':^12} | {'Load %':^10} | {'Skip Ratio':^10} | {'Total CPU':^10} | "
          f"{'Enc CPU':^10} | {'H.264 FPS':^10} | {'Free CPU':^10} | {'Status':^8}")
    print("-" * 100)

    # Test each target CPU level
    for target in [30, 40, 50, 60, 70, 80]:
        # Test with different additional load levels
        for load in [0, 10, 20, 30, 40]:
            stats = run_test(target, load)

            if stats:
                # Check if free CPU target is met (within 5% tolerance)
                target_free = 100 - target
                status = "OK" if stats['free_cpu'] >= target_free - 5 else "LOW"

                print(f"{target:^12}% | {load:^10}% | {stats['skip_ratio']:^10} | "
                      f"{stats['total_cpu']:^10}% | {stats['encoder_cpu']:^10}% | "
                      f"{stats['h264_fps']:^10} | {stats['free_cpu']:^10}% | {status:^8}")

                results.append({
                    'target_cpu': target,
                    'load': load,
                    **stats,
                    'status': status
                })
            else:
                print(f"{target:^12}% | {load:^10}% | {'ERROR':^10} | {'N/A':^10} | "
                      f"{'N/A':^10} | {'N/A':^10} | {'N/A':^10} | {'ERR':^8}")

        print()  # Blank line between target groups

    # Cleanup
    print("Cleaning up...")
    stop_cpu_load()
    set_settings(auto_skip=False, target_cpu=60, skip_ratio=2)

    # Summary
    print()
    print("=" * 100)
    print("  Summary")
    print("=" * 100)
    ok_count = sum(1 for r in results if r.get('status') == 'OK')
    total_count = len(results)
    print(f"Tests passed: {ok_count}/{total_count}")

    if ok_count < total_count:
        print("\nFailed tests (free CPU below target):")
        for r in results:
            if r.get('status') != 'OK':
                print(f"  Target {r['target_cpu']}%, Load {r['load']}%: "
                      f"Free CPU {r['free_cpu']}% (needed {100-r['target_cpu']}%)")


if __name__ == '__main__':
    try:
        main()
    except KeyboardInterrupt:
        print("\nInterrupted, cleaning up...")
        stop_cpu_load()
        set_settings(auto_skip=False, target_cpu=60, skip_ratio=2)
        sys.exit(1)
