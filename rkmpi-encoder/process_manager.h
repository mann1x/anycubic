/*
 * Process Manager
 *
 * Manages fork/exec of secondary rkmpi_enc instances for multi-camera support.
 * Handles lifecycle, crash detection, restart with backoff, and cleanup.
 */

#ifndef PROCESS_MANAGER_H
#define PROCESS_MANAGER_H

#include "camera_detect.h"
#include "config.h"
#include <sys/types.h>
#include <time.h>

/* Maximum args for execv */
#define PROCMGR_MAX_ARGS  48

/* Maximum restarts per minute before giving up */
#define PROCMGR_MAX_RESTARTS_PER_MIN  3

/* Managed process state */
typedef struct {
    pid_t pid;                  /* 0 = not running */
    int camera_id;              /* 1-based */
    char device[32];            /* Camera device path */
    int streaming_port;         /* Assigned streaming port */
    char cmd_file[256];         /* Command file path */
    char ctrl_file[256];        /* Control file path */
    int restart_count;          /* Restarts within current minute */
    time_t restart_window_start; /* Start of current restart window */
    time_t last_start;          /* When process was last started */
    int enabled;                /* Whether process should be running */
    /* Per-camera overrides (0 = use defaults) */
    int override_width;         /* 0 = default (640) */
    int override_height;        /* 0 = default (480) */
    int force_mjpeg;            /* 1 = use MJPEG instead of YUYV */
} ManagedProcess;

/*
 * Start a secondary encoder process for a camera.
 * Builds argument list from camera info and config, then fork/exec.
 *
 * proc: managed process slot to fill
 * cam: camera info (device, port, ID)
 * cfg: app config (for encoder settings)
 * binary_path: path to rkmpi_enc binary
 *
 * Returns: 0 on success (pid set), -1 on error
 */
int procmgr_start_camera(ManagedProcess *proc, const CameraInfo *cam,
                          const AppConfig *cfg, const char *binary_path);

/*
 * Stop a single managed process.
 * Sends SIGTERM, waits briefly, then SIGKILL if needed.
 */
void procmgr_stop_camera(ManagedProcess *proc);

/*
 * Stop all managed processes.
 */
void procmgr_stop_all(ManagedProcess *procs, int count);

/*
 * Check child processes (non-blocking).
 * Uses waitpid(WNOHANG) to detect exited children.
 * Restarts crashed processes with backoff.
 *
 * procs: array of managed processes
 * count: number of slots
 * cfg: app config (for restart)
 * binary_path: path to rkmpi_enc binary
 * cameras: camera info array (for restart)
 *
 * Returns: number of processes restarted
 */
int procmgr_check_children(ManagedProcess *procs, int count,
                            const AppConfig *cfg, const char *binary_path,
                            const CameraInfo *cameras);

/*
 * Forward a signal to all managed processes.
 */
void procmgr_signal_all(ManagedProcess *procs, int count, int sig);

#endif /* PROCESS_MANAGER_H */
