# Credits and Acknowledgments

## moonraker-timelapse

The timelapse configuration options in h264-streamer are inspired by
[moonraker-timelapse](https://github.com/mainsail-crew/moonraker-timelapse)
by the Mainsail Crew.

moonraker-timelapse is licensed under the GNU General Public License v3.0.

### Inspired Features

- Layer-based and hyperlapse timelapse modes
- Variable FPS calculation based on target video length
- Configurable output FPS, CRF quality, and frame duplication
- Flip/mirror options for frame capture

### Implementation Notes

This implementation uses Moonraker's WebSocket API to monitor print progress
and trigger frame captures independently of the slicer settings. The timelapse
frame capture and video assembly is handled natively by the rkmpi-encoder
binary using ffmpeg.
