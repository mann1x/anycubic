#!/bin/sh
#
# fb_status Demo Script (Pipe Mode)
# Showcases all capabilities for video recording
#
# Usage: ./demo.sh [delay]
#   delay: seconds to wait before starting (default: 30)
#

DELAY=${1:-30}
FB=/tmp/fb_status

echo "================================"
echo "  fb_status Capability Demo"
echo "================================"
echo ""
echo "Starting in $DELAY seconds..."
echo "Position your camera now!"
echo ""

# Countdown
i=$DELAY
while [ $i -gt 0 ]; do
    echo -n "$i "
    sleep 1
    i=$((i - 1))
done
echo ""
echo "Starting demo..."

# Start pipe mode - saves screen automatically
exec 3> >($FB pipe -q)
sleep 1

#############################################
# Part 1: Basic Colors
#############################################
echo "position center" >&3
echo "size 28" >&3
echo "bg 333333" >&3
echo "color white" >&3
echo "show Demo: Named Colors" >&3
sleep 3

echo "bg 222222" >&3
echo "size 32" >&3
echo "position bottom" >&3

for color in green red yellow blue cyan magenta orange pink purple; do
    echo "color $color" >&3
    echo "show Color: $color" >&3
    sleep 1
done

#############################################
# Part 2: Hex Colors
#############################################
echo "position center" >&3
echo "size 28" >&3
echo "bg 333333" >&3
echo "color white" >&3
echo "show Demo: Hex RGB Colors" >&3
sleep 3

echo "bg 222222" >&3
echo "size 32" >&3
echo "position bottom" >&3

echo "color FF0000" >&3
echo "show Hex: #FF0000 (Red)" >&3
sleep 1
echo "color 00FF00" >&3
echo "show Hex: #00FF00 (Green)" >&3
sleep 1
echo "color 0000FF" >&3
echo "show Hex: #0000FF (Blue)" >&3
sleep 1
echo "color FF6B35" >&3
echo "show Hex: #FF6B35 (Coral)" >&3
sleep 1
echo "color 9B59B6" >&3
echo "show Hex: #9B59B6 (Purple)" >&3
sleep 1
echo "color 1ABC9C" >&3
echo "show Hex: #1ABC9C (Teal)" >&3
sleep 1

#############################################
# Part 3: Positions
#############################################
echo "position center" >&3
echo "size 28" >&3
echo "bg 333333" >&3
echo "color white" >&3
echo "show Demo: Positions" >&3
sleep 3

echo "bg 222222" >&3
echo "size 40" >&3

echo "color yellow" >&3
echo "position top" >&3
echo "show Position: TOP" >&3
sleep 2

echo "color cyan" >&3
echo "position center" >&3
echo "show Position: CENTER" >&3
sleep 2

echo "color green" >&3
echo "position bottom" >&3
echo "show Position: BOTTOM" >&3
sleep 2

#############################################
# Part 4: Font Sizes
#############################################
echo "position center" >&3
echo "size 28" >&3
echo "bg 333333" >&3
echo "color white" >&3
echo "show Demo: Font Sizes" >&3
sleep 3

echo "bg 222222" >&3

echo "size 20" >&3
echo "show Size: 20 pixels" >&3
sleep 1

echo "size 32" >&3
echo "show Size: 32 pixels (default)" >&3
sleep 1

echo "size 48" >&3
echo "show Size: 48 pixels" >&3
sleep 1

echo "size 64" >&3
echo "show Size: 64 pixels" >&3
sleep 1

echo "size 96" >&3
echo "color yellow" >&3
echo "show BIG!" >&3
sleep 2

#############################################
# Part 5: Font Styles
#############################################
echo "position center" >&3
echo "size 28" >&3
echo "bg 333333" >&3
echo "color white" >&3
echo "show Demo: Font Styles" >&3
sleep 3

echo "bg 222222" >&3
echo "size 40" >&3

echo "regular" >&3
echo "show Regular Style" >&3
sleep 2

echo "bold" >&3
echo "show Bold Style" >&3
sleep 2

echo "regular" >&3

#############################################
# Part 6: Background Colors
#############################################
echo "position center" >&3
echo "size 28" >&3
echo "bg 333333" >&3
echo "color white" >&3
echo "show Demo: Background Colors" >&3
sleep 3

echo "size 32" >&3
echo "position bottom" >&3

echo "bg 333333" >&3
echo "show Dark Gray Background" >&3
sleep 1

echo "color green" >&3
echo "bg 000000" >&3
echo "show Black Background" >&3
sleep 1

echo "color white" >&3
echo "bg 000066" >&3
echo "show Dark Blue Background" >&3
sleep 1

echo "bg 660000" >&3
echo "show Dark Red Background" >&3
sleep 1

echo "bg 006600" >&3
echo "show Dark Green Background" >&3
sleep 1

#############################################
# Part 7: Multiline Text
#############################################
echo "position center" >&3
echo "size 28" >&3
echo "bg 333333" >&3
echo "color white" >&3
echo "show Demo: Multiline Text" >&3
sleep 3

echo "bg 222222" >&3
echo "size 32" >&3
echo "position bottom" >&3

echo "color green" >&3
echo "show Single Line Message" >&3
sleep 2

echo "color cyan" >&3
echo "show Line One\nLine Two" >&3
sleep 2

echo "color yellow" >&3
echo "show First Line\nSecond Line\nThird Line" >&3
sleep 2

echo "color green" >&3
echo "size 26" >&3
echo "show BedMesh Calibration\nStep 3 of 5\nProbing point 45/100" >&3
sleep 3

echo "color red" >&3
echo "bg 330000" >&3
echo "size 28" >&3
echo "show ERROR!\nConnection Lost\nRetrying in 5s..." >&3
sleep 3

#############################################
# Part 8: Combined Features
#############################################
echo "position center" >&3
echo "size 28" >&3
echo "bg 333333" >&3
echo "color white" >&3
echo "show Demo: Combined Features" >&3
sleep 3

echo "bold" >&3
echo "size 36" >&3
echo "color yellow" >&3
echo "bg 222244" >&3
echo "position top" >&3
echo "show Large Bold Title\nWith subtitle below" >&3
sleep 3

echo "regular" >&3
echo "size 48" >&3
echo "color cyan" >&3
echo "bg 000000" >&3
echo "position center" >&3
echo "show Centered\nMultiline\nMessage" >&3
sleep 3

echo "size 28" >&3
echo "color green" >&3
echo "bg 002200" >&3
echo "position bottom" >&3
echo "show Status: OK\nProgress: 75%\nETA: 2 minutes" >&3
sleep 3

#############################################
# Part 9: Simulated Calibration
#############################################
echo "position center" >&3
echo "size 28" >&3
echo "bg 333333" >&3
echo "color white" >&3
echo "show Demo: Calibration Simulation" >&3
sleep 3

echo "bg 222222" >&3
echo "position bottom" >&3

echo "color yellow" >&3
echo "show BedMesh Calibration\nPreheating to 60C..." >&3
sleep 2

echo "show BedMesh Calibration\nHeat soaking: 5:00" >&3
sleep 2

echo "show BedMesh Calibration\nHeat soaking: 4:30" >&3
sleep 1

echo "show BedMesh Calibration\nHeat soaking: 4:00" >&3
sleep 1

echo "color cyan" >&3
echo "show BedMesh Calibration\nProbing bed mesh...\nPoint 1/25" >&3
sleep 1

echo "show BedMesh Calibration\nProbing bed mesh...\nPoint 5/25" >&3
sleep 1

echo "show BedMesh Calibration\nProbing bed mesh...\nPoint 15/25" >&3
sleep 1

echo "show BedMesh Calibration\nProbing bed mesh...\nPoint 25/25" >&3
sleep 1

echo "show BedMesh Calibration\nSaving configuration..." >&3
sleep 2

echo "color green" >&3
echo "bg 002200" >&3
echo "show Calibration Complete!\nRestarting printer..." >&3
sleep 3

#############################################
# Part 10: Error States
#############################################
echo "position center" >&3
echo "size 28" >&3
echo "bg 333333" >&3
echo "color white" >&3
echo "show Demo: Error States" >&3
sleep 3

echo "size 32" >&3
echo "color red" >&3
echo "bg 220000" >&3
echo "show ERROR\nPrinter not responding\nCheck connection" >&3
sleep 3

echo "color orange" >&3
echo "bg 222200" >&3
echo "show WARNING\nLow filament detected\nPrint may fail" >&3
sleep 3

echo "color yellow" >&3
echo "bg 222222" >&3
echo "show Firmware Update\nDo not power off!\nProgress: 67%" >&3
sleep 3

#############################################
# Finale
#############################################
echo "size 36" >&3
echo "color green" >&3
echo "bg 002200" >&3
echo "position center" >&3
echo "show Demo Complete!\nThank you for watching" >&3
sleep 4

# Close and restore screen
echo "close" >&3
exec 3>&-

echo ""
echo "================================"
echo "  Demo complete!"
echo "================================"
