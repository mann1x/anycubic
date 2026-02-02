#!/bin/sh
# USB Camera Diagnostics for RV1106
# Run on printer to check USB bandwidth and settings

echo "=== USB Camera Diagnostics ==="
echo ""

# 1. USB device tree
echo "--- USB Device Tree ---"
if command -v lsusb >/dev/null 2>&1; then
    lsusb -t 2>/dev/null || echo "(lsusb -t not available)"
    echo ""
    lsusb 2>/dev/null || echo "(lsusb not available)"
else
    echo "(lsusb not installed)"
fi
echo ""

# 2. Video devices
echo "--- Video Devices ---"
ls -la /dev/video* 2>/dev/null || echo "No /dev/video* devices"
ls -la /dev/v4l/by-id/ 2>/dev/null || echo "No /dev/v4l/by-id/"
echo ""

# 3. USB core parameters
echo "--- USB Core Parameters ---"
if [ -d /sys/module/usbcore/parameters ]; then
    echo "usbfs_memory_mb: $(cat /sys/module/usbcore/parameters/usbfs_memory_mb 2>/dev/null || echo 'N/A')"
    echo "autosuspend: $(cat /sys/module/usbcore/parameters/autosuspend 2>/dev/null || echo 'N/A')"
    echo "autosuspend_delay_ms: $(cat /sys/module/usbcore/parameters/autosuspend_delay_ms 2>/dev/null || echo 'N/A')"
else
    echo "(usbcore parameters not available)"
fi
echo ""

# 4. UVC video parameters
echo "--- UVC Video Parameters ---"
if [ -d /sys/module/uvcvideo/parameters ]; then
    echo "quirks: $(cat /sys/module/uvcvideo/parameters/quirks 2>/dev/null || echo 'N/A')"
    echo "timeout: $(cat /sys/module/uvcvideo/parameters/timeout 2>/dev/null || echo 'N/A')"
    echo "trace: $(cat /sys/module/uvcvideo/parameters/trace 2>/dev/null || echo 'N/A')"
    echo "nodrop: $(cat /sys/module/uvcvideo/parameters/nodrop 2>/dev/null || echo 'N/A')"
else
    echo "(uvcvideo module not loaded or parameters not available)"
fi
echo ""

# 5. USB device details (find camera)
echo "--- USB Device Power Settings ---"
for dev in /sys/bus/usb/devices/*; do
    if [ -f "$dev/product" ]; then
        product=$(cat "$dev/product" 2>/dev/null)
        # Look for camera-related devices
        case "$product" in
            *[Cc]amera*|*[Ww]ebcam*|*USB*Video*|*UVC*)
                echo "Device: $(basename $dev)"
                echo "  Product: $product"
                echo "  Manufacturer: $(cat $dev/manufacturer 2>/dev/null || echo 'N/A')"
                echo "  Speed: $(cat $dev/speed 2>/dev/null || echo 'N/A') Mbps"
                echo "  Power control: $(cat $dev/power/control 2>/dev/null || echo 'N/A')"
                echo "  Autosuspend: $(cat $dev/power/autosuspend_delay_ms 2>/dev/null || echo 'N/A') ms"
                echo "  bMaxPower: $(cat $dev/bMaxPower 2>/dev/null || echo 'N/A')"
                echo ""
                ;;
        esac
    fi
done

# Also check for video class devices
for dev in /sys/bus/usb/devices/*; do
    if [ -f "$dev/bInterfaceClass" ]; then
        class=$(cat "$dev/bInterfaceClass" 2>/dev/null)
        # 0e = Video class
        if [ "$class" = "0e" ]; then
            parent=$(dirname "$dev")
            echo "Video Class Interface: $(basename $dev)"
            if [ -f "$parent/product" ]; then
                echo "  Product: $(cat $parent/product 2>/dev/null)"
            fi
            echo "  Interface Class: $class (Video)"
            echo "  Interface Subclass: $(cat $dev/bInterfaceSubClass 2>/dev/null || echo 'N/A')"
            echo ""
        fi
    fi
done

# 6. Memory info
echo "--- Memory Info ---"
free -m 2>/dev/null || cat /proc/meminfo | head -5
echo ""

# 7. CPU info
echo "--- CPU Info ---"
cat /proc/cpuinfo | grep -E "^(processor|model name|BogoMIPS|Hardware)" | head -10
echo ""

# 8. Kernel messages about USB/UVC
echo "--- Recent USB/UVC Kernel Messages ---"
dmesg 2>/dev/null | grep -iE "usb|uvc|video" | tail -20 || echo "(dmesg not available)"
echo ""

echo "=== Diagnostics Complete ==="
echo ""
echo "Recommendations:"
echo "  - If usbfs_memory_mb < 32, increase with:"
echo "    echo 32 > /sys/module/usbcore/parameters/usbfs_memory_mb"
echo ""
echo "  - If camera power/control is 'auto', disable autosuspend:"
echo "    echo 'on' > /sys/bus/usb/devices/DEVICE/power/control"
echo ""
echo "  - To enable UVC bandwidth tracing:"
echo "    echo 0x400 > /sys/module/uvcvideo/parameters/trace"
