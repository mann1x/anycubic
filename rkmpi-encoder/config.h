/*
 * Configuration Management
 *
 * Persistent JSON configuration for h264-streamer settings.
 * Loads/saves from /useremain/home/rinkhals/apps/29-h264-streamer.config
 */

#ifndef CONFIG_H
#define CONFIG_H

#include <stdint.h>

/* Default config file path */
#define CONFIG_DEFAULT_PATH "/useremain/home/rinkhals/apps/29-h264-streamer.config"

/* Maximum cameras supported */
#define MAX_CAMERAS 4

/* Per-camera settings (keyed by unique_id in JSON) */
typedef struct {
    char unique_id[128];        /* USB unique ID, e.g. "usb-xhci_0-1.3-video" */
    char name[64];              /* Display name, e.g. "USB Camera" */
    int brightness;
    int contrast;
    int saturation;
    int hue;
    int gamma;
    int sharpness;
    int gain;
    int backlight;
    int wb_auto;
    int wb_temp;
    int exposure_auto;
    int exposure;
    int exposure_priority;
    int power_line;
} CameraSettings;

/* Application configuration */
typedef struct {
    /* Encoder settings */
    char encoder_type[16];          /* "rkmpi" or "rkmpi-yuyv" */
    int h264_enabled;
    int auto_skip;
    int skip_ratio;
    int target_cpu;
    int bitrate;
    int mjpeg_fps;
    int jpeg_quality;
    char h264_resolution[16];       /* "1280x720", "960x540", etc. */

    /* Display */
    int display_enabled;
    int display_fps;

    /* Ports */
    int streaming_port;
    int control_port;

    /* Modes */
    char mode[20];                  /* "go-klipper" or "vanilla-klipper" */
    int autolanmode;
    int logging;
    int log_max_size;               /* Max log file size in KB (100-5120) */
    int acproxycam_flv_proxy;

    /* Internal USB port for camera detection */
    char internal_usb_port[32];     /* e.g. "1.3" */

    /* Timelapse */
    int timelapse_enabled;
    char timelapse_mode[16];        /* "layer" or "hyperlapse" */
    int timelapse_hyperlapse_interval;
    char timelapse_storage[16];     /* "internal" or "usb" */
    char timelapse_usb_path[256];
    int timelapse_output_fps;
    int timelapse_variable_fps;
    int timelapse_target_length;
    int timelapse_variable_fps_min;
    int timelapse_variable_fps_max;
    int timelapse_crf;
    int timelapse_duplicate_last_frame;
    float timelapse_stream_delay;
    int timelapse_flip_x;
    int timelapse_flip_y;
    float timelapse_end_delay;
    char moonraker_host[64];
    int moonraker_port;
    char moonraker_camera_ip[16];   /* "auto", "localhost", "eth0", "eth1" */

    /* Primary camera controls (CAM#1) */
    int cam_brightness;
    int cam_contrast;
    int cam_saturation;
    int cam_hue;
    int cam_gamma;
    int cam_sharpness;
    int cam_gain;
    int cam_backlight;
    int cam_wb_auto;
    int cam_wb_temp;
    int cam_exposure_auto;
    int cam_exposure;
    int cam_exposure_priority;
    int cam_power_line;

    /* Per-camera settings (JSON "cameras" dict, keyed by unique_id) */
    /* Stored as raw JSON string to preserve unknown camera IDs */
    char cameras_json[4096];

    /* Fault Detection */
    int fault_detect_enabled;
    int fault_detect_cnn_enabled;
    int fault_detect_proto_enabled;
    int fault_detect_multi_enabled;
    char fault_detect_strategy[32];
    int fault_detect_interval;
    int fault_detect_verify_interval;
    char fault_detect_model_set[64];    /* Selected model set directory name */
    int fault_detect_min_free_mem;
    int fault_detect_pace_ms;
    int heatmap_enabled;                /* Spatial heatmap on fault detection */
    int fd_debug_logging;               /* Extra FD diagnostic logging (heatmap split, EMA) */
    int fd_beep_pattern;                /* Buzzer alert on fault: 0=none, 1-5=patterns */
    char fd_thresholds_json[2048];      /* Per-set threshold config JSON */

    /* Prototype Management */
    char proto_active_set[64];          /* Name of active prototype set */
    char proto_dataset_url[256];        /* URL for default dataset download */

    /* Fault Detection Setup Wizard */
    int fd_setup_status;                /* 0=NONE, 1=INPROGRESS, 2=OK */
    int64_t fd_setup_timestamp;         /* Unix epoch when last completed */
    float fd_setup_corners[16];         /* Normalized [0..1]: 8 points x2 coords, clockwise: TL,TM,TR,MR,BR,BM,BL,ML */
    char fd_setup_mask_hex[128];        /* 392-bit hex mask: "w6:w5:...:w0", 1=active, 0=masked */
    int fd_bed_size_x;                  /* Bed width mm (default 220) */
    int fd_bed_size_y;                  /* Bed depth mm (default 220) */
    char fd_setup_results_json[2048];   /* Per-step verification results JSON */
    char fd_z_masks_json[4096];         /* JSON: [[z_mm, mask], ...] for Z-dependent masks */

    /* Runtime: config file path (not persisted) */
    char config_file[256];
} AppConfig;

/* Fault Detection Setup status values */
#define FD_SETUP_NONE       0
#define FD_SETUP_INPROGRESS 1
#define FD_SETUP_OK         2

/* Set all config fields to sensible defaults */
void config_set_defaults(AppConfig *cfg);

/* Load configuration from JSON file. Returns 0 on success, -1 on error.
 * On error, cfg retains its previous values (call config_set_defaults first). */
int config_load(AppConfig *cfg, const char *path);

/* Save configuration to JSON file. Returns 0 on success, -1 on error.
 * Merges with existing file content to preserve unknown keys. */
int config_save(const AppConfig *cfg, const char *path);

#endif /* CONFIG_H */
