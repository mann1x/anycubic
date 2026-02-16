/*
 * Timelapse Recording Support
 *
 * Captures JPEG frames during printing and assembles them into MP4 video.
 * Triggered via ctrl file commands or RPC commands from the printer firmware.
 */

#ifndef TIMELAPSE_H
#define TIMELAPSE_H

#include <stdint.h>
#include <stddef.h>

/* Default output directory for timelapse videos */
#define TIMELAPSE_OUTPUT_DIR    "/useremain/app/gk/Time-lapse-Video/"
#define TIMELAPSE_TEMP_DIR      "/tmp/timelapse_frames"

/* FFmpeg configuration
 * Primary: Our bundled static ffmpeg (no dependencies)
 * Fallback: Stock ffmpeg with LD_LIBRARY_PATH set
 */
#define TIMELAPSE_FFMPEG_PATH   "/useremain/home/rinkhals/apps/29-h264-streamer/ffmpeg"
#define TIMELAPSE_FFMPEG_STOCK  "/ac_lib/lib/third_bin/ffmpeg"
#define TIMELAPSE_FFMPEG_LIBS   "/ac_lib/lib/third_lib"

/* Maximum path lengths */
#define TIMELAPSE_PATH_MAX      1024
#define TIMELAPSE_NAME_MAX      256

/* Timelapse configuration */
typedef struct {
    int output_fps;                 /* Playback framerate (default: 30) */
    int crf;                        /* x264 quality factor 0-51 (default: 23) */
    int variable_fps;               /* Use variable FPS based on target length */
    int target_length;              /* Target video length in seconds (variable FPS) */
    int variable_fps_min;           /* Minimum FPS for variable calculation */
    int variable_fps_max;           /* Maximum FPS for variable calculation */
    int duplicate_last_frame;       /* Number of times to repeat final frame */
    int flip_x;                     /* Horizontal flip (mirror) */
    int flip_y;                     /* Vertical flip */
    char output_dir[TIMELAPSE_PATH_MAX];  /* Custom output directory */
    char temp_dir_base[TIMELAPSE_PATH_MAX]; /* Base directory for temp frames */
} TimelapseConfig;

/* Timelapse state */
typedef struct {
    int active;                             /* Timelapse recording in progress */
    int custom_mode;                        /* 1 if initiated by h264_server (ignore RPC) */
    char gcode_name[TIMELAPSE_NAME_MAX];    /* Base name (from gcode filepath) */
    char temp_dir[TIMELAPSE_PATH_MAX];      /* Temp frame storage directory */
    int frame_count;                        /* Number of frames captured */
    int sequence_num;                       /* Sequence number (_01, _02, etc.) */
    TimelapseConfig config;                 /* Current configuration */

    /* Hardware VENC encoding state */
    int use_venc;                           /* 1 to use hardware VENC, 0 for ffmpeg */
    int venc_initialized;                   /* 1 if VENC encoder is ready */
    int frame_width;                        /* Frame width (from first frame) */
    int frame_height;                       /* Frame height (from first frame) */
} TimelapseState;

/* Global timelapse state */
extern TimelapseState g_timelapse;

/*
 * Set timelapse configuration option.
 * Call before timelapse_init() to configure the next recording.
 */
void timelapse_set_fps(int fps);
void timelapse_set_crf(int crf);
void timelapse_set_variable_fps(int min_fps, int max_fps, int target_length);
void timelapse_set_duplicate_last(int count);
void timelapse_set_flip(int flip_x, int flip_y);
void timelapse_set_output_dir(const char *dir);
void timelapse_set_temp_dir(const char *dir);

/*
 * Enable or disable hardware VENC encoding.
 * When enabled, uses RV1106 hardware H.264 encoder instead of ffmpeg.
 * Must be called before timelapse_init().
 *
 * @param enabled 1 to use VENC (default), 0 to use ffmpeg
 */
void timelapse_set_use_venc(int enabled);

/*
 * Reset configuration to defaults.
 */
void timelapse_reset_config(void);

/*
 * Initialize timelapse recording.
 * Called when openDelayCamera RPC is received or timelapse_init ctrl command.
 *
 * @param gcode_name Base name for output files (without .gcode extension)
 * @param output_dir Optional output directory (NULL = use default or configured)
 * @return 0 on success, -1 on error
 */
int timelapse_init(const char *gcode_name, const char *output_dir);

/*
 * Initialize timelapse recording (legacy API).
 * Extracts name from full gcode filepath.
 *
 * @param gcode_filepath Full path to gcode file (used for output naming)
 * @return 0 on success, -1 on error
 */
int timelapse_init_legacy(const char *gcode_filepath);

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

/*
 * Check if custom timelapse mode is active.
 * When custom mode is enabled (via h264_server), RPC timelapse commands
 * should be ignored.
 *
 * @return 1 if custom mode active, 0 if not
 */
int timelapse_is_custom_mode(void);

/*
 * Enable or disable custom timelapse mode.
 * When enabled, RPC timelapse commands (openDelayCamera, startLanCapture)
 * will be ignored.
 *
 * @param enabled 1 to enable, 0 to disable
 */
void timelapse_set_custom_mode(int enabled);

/*
 * Get current frame count.
 *
 * @return Number of frames captured, 0 if not active
 */
int timelapse_get_frame_count(void);

#endif /* TIMELAPSE_H */
