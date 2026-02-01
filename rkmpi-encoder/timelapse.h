/*
 * Timelapse Recording Support
 *
 * Captures JPEG frames during printing and assembles them into MP4 video.
 * Triggered via RPC commands from the printer firmware.
 */

#ifndef TIMELAPSE_H
#define TIMELAPSE_H

#include <stdint.h>
#include <stddef.h>

/* Output directory for timelapse videos */
#define TIMELAPSE_OUTPUT_DIR    "/useremain/app/gk/Time-lapse-Video/"
#define TIMELAPSE_TEMP_DIR      "/tmp/timelapse_frames"

/* Maximum path lengths */
#define TIMELAPSE_PATH_MAX      512
#define TIMELAPSE_NAME_MAX      256

/* Timelapse state */
typedef struct {
    int active;                             /* Timelapse recording in progress */
    char gcode_name[TIMELAPSE_NAME_MAX];    /* Base name (from gcode filepath) */
    char temp_dir[TIMELAPSE_PATH_MAX];      /* Temp frame storage directory */
    int frame_count;                        /* Number of frames captured */
    int sequence_num;                       /* Sequence number (_01, _02, etc.) */
} TimelapseState;

/* Global timelapse state */
extern TimelapseState g_timelapse;

/*
 * Initialize timelapse recording.
 * Called when openDelayCamera RPC is received.
 *
 * @param gcode_filepath Full path to gcode file (used for output naming)
 * @return 0 on success, -1 on error
 */
int timelapse_init(const char *gcode_filepath);

/*
 * Capture a timelapse frame.
 * Called when startLanCapture RPC is received during active timelapse.
 *
 * @return 0 on success, -1 on error
 */
int timelapse_capture_frame(void);

/*
 * Finalize timelapse recording.
 * Assembles captured frames into MP4 video.
 * Called when print completes (print_stats.state == "complete").
 *
 * @return 0 on success, -1 on error
 */
int timelapse_finalize(void);

/*
 * Cancel/abort timelapse recording.
 * Cleans up temp files without creating video.
 * Called on print cancel or error.
 */
void timelapse_cancel(void);

/*
 * Check if timelapse is currently active.
 *
 * @return 1 if active, 0 if not
 */
int timelapse_is_active(void);

#endif /* TIMELAPSE_H */
