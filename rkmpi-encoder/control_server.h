/*
 * Control HTTP Server
 *
 * HTTP server on the control port (default 8081) providing:
 * - Web UI for settings management (/control)
 * - REST API for stats, config, camera controls
 * - Timelapse file management
 * - Touch injection
 * - ACProxyCam FLV proxy coordination
 *
 * Runs in its own thread, uses the same socket patterns as http_server.c.
 */

#ifndef CONTROL_SERVER_H
#define CONTROL_SERVER_H

#include "config.h"
#include "cpu_monitor.h"
#include "camera_detect.h"
#include "process_manager.h"
#include <stdint.h>
#include <pthread.h>
#include <time.h>

/* Maximum POST body size */
#define CTRL_MAX_POST_BODY  8192

/* Maximum template size after substitution */
#define CTRL_MAX_TEMPLATE   (128 * 1024)

/* Maximum key-value pairs in form data */
#define CTRL_MAX_FORM_PARAMS 64

/* Template directory (same directory as binary, or /tmp for development) */
#define CTRL_TEMPLATE_DIR_DEFAULT "/useremain/home/rinkhals/apps/29-h264-streamer"

/* Timelapse paths */
#define TIMELAPSE_DIR_INTERNAL  "/useremain/app/gk/Time-lapse-Video"
#define TIMELAPSE_DIR_USB       "/mnt/udisk/Time-lapse-Video"

/* ffmpeg/ffprobe paths */
#define FFMPEG_PATH     "/ac_lib/lib/third_bin/ffmpeg"
#define FFPROBE_PATH    "/ac_lib/lib/third_bin/ffprobe"

/* Control server state */
typedef struct {
    int listen_fd;
    int port;
    volatile int running;
    pthread_t thread;

    /* Shared config reference (owned by main) */
    AppConfig *config;

    /* CPU monitor */
    CPUMonitor cpu_monitor;

    /* Stats from encoder (read from ctrl file) */
    float encoder_mjpeg_fps;
    float encoder_h264_fps;
    int encoder_mjpeg_clients;
    int encoder_flv_clients;
    int encoder_display_clients;
    int max_camera_fps;

    /* ACProxyCam state */
    char acproxycam_flv_url[256];
    time_t acproxycam_last_seen;
    int flv_proxy_clients;

    /* Session ID (unique per startup) */
    char session_id[40];

    /* Template directory path */
    char template_dir[256];

    /* Multi-camera state (set by main, read by API) */
    CameraInfo *cameras;
    int num_cameras;
    ManagedProcess *managed_procs;
    int num_managed;

    /* Callback: apply config changes to encoder */
    void (*on_config_changed)(AppConfig *cfg);

    /* Callback: restart the entire application */
    void (*on_restart)(void);
} ControlServer;

/* Global control server instance */
extern ControlServer g_control_server;

/* Initialize and start control server.
 * cfg: shared config (must remain valid for lifetime of server)
 * port: control port (0 = use config value or default 8081)
 * template_dir: path to HTML templates (NULL = use default)
 * Returns: 0 on success, -1 on error */
int control_server_start(AppConfig *cfg, int port, const char *template_dir);

/* Stop control server */
void control_server_stop(void);

/* Update encoder stats (called from main loop or stats reader) */
void control_server_update_stats(float mjpeg_fps, float h264_fps,
                                  int mjpeg_clients, int flv_clients,
                                  int display_clients, int max_camera_fps);

/* Set config-changed callback */
void control_server_set_config_callback(void (*cb)(AppConfig *cfg));

/* Set restart callback */
void control_server_set_restart_callback(void (*cb)(void));

/* Set multi-camera info (called after camera detection) */
void control_server_set_cameras(CameraInfo *cameras, int num_cameras,
                                 ManagedProcess *procs, int num_managed);

/* Provision cameras to Moonraker (HTTP POST to Moonraker API).
 * Uses per-camera settings from config cameras_json. */
void control_server_provision_moonraker(ControlServer *srv);

/* Forward declaration for moonraker client */
struct MoonrakerClient;

/* Set Moonraker client reference for status API.
 * Pass NULL to clear (e.g. when moonraker client is stopped). */
void control_server_set_moonraker(struct MoonrakerClient *mc);

#endif /* CONTROL_SERVER_H */
