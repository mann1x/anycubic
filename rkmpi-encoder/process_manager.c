/*
 * Process Manager
 *
 * Manages fork/exec of secondary rkmpi_enc instances for multi-camera support.
 * Secondary cameras run in MJPEG-only mode (no H.264, no MQTT/RPC).
 */

#include "process_manager.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <errno.h>

/*
 * Build command-line arguments for a secondary encoder process.
 * Secondary cameras run in server mode with MJPEG only:
 *  - No H.264 encoding (--no-h264)
 *  - No FLV server (--no-flv)
 *  - No MQTT/RPC (secondary doesn't need them)
 *  - Own streaming port
 *  - Own cmd/ctrl files
 */
static int build_secondary_args(char **argv, int max_args,
                                 const CameraInfo *cam,
                                 const AppConfig *cfg,
                                 const ManagedProcess *proc,
                                 const char *binary_path,
                                 const char *cmd_file,
                                 const char *ctrl_file) {
    int argc = 0;

    /* Static string buffers for argument values (persist until exec) */
    static char port_str[12];
    static char width_str[12];
    static char height_str[12];
    static char fps_str[12];
    static char bitrate_str[12];
    static char quality_str[12];

    /* Use per-camera overrides if set, otherwise default 640x480 */
    int width = (proc->override_width > 0) ? proc->override_width : 640;
    int height = (proc->override_height > 0) ? proc->override_height : 480;

    snprintf(port_str, sizeof(port_str), "%d", cam->streaming_port);
    snprintf(width_str, sizeof(width_str), "%d", width);
    snprintf(height_str, sizeof(height_str), "%d", height);
    int fps = proc->override_fps > 0 ? proc->override_fps :
              (cfg->mjpeg_fps > 0 ? cfg->mjpeg_fps : 10);
    snprintf(fps_str, sizeof(fps_str), "%d", fps);
    snprintf(bitrate_str, sizeof(bitrate_str), "%d", cfg->bitrate > 0 ? cfg->bitrate : 512);
    snprintf(quality_str, sizeof(quality_str), "%d", cfg->jpeg_quality > 0 ? cfg->jpeg_quality : 85);

    argv[argc++] = (char *)binary_path;
    argv[argc++] = "-S";           /* Server mode */
    argv[argc++] = "-N";           /* No stdout */
    argv[argc++] = "-d";
    argv[argc++] = (char *)cam->device;
    argv[argc++] = "-w";
    argv[argc++] = width_str;
    argv[argc++] = "-h";
    argv[argc++] = height_str;
    argv[argc++] = "-f";
    argv[argc++] = fps_str;
    argv[argc++] = "-b";
    argv[argc++] = bitrate_str;

    /* YUYV mode by default for secondary cameras (saves USB bandwidth).
     * Can be overridden to MJPEG via per-camera settings. */
    if (!proc->force_mjpeg) {
        argv[argc++] = "--yuyv";
        argv[argc++] = "--jpeg-quality";
        argv[argc++] = quality_str;
    }

    /* No H.264 encoding for secondary cameras */
    argv[argc++] = "--no-h264";

    /* No FLV server (primary handles H.264/FLV) */
    argv[argc++] = "--no-flv";

    /* Skip MQTT/RPC (secondary doesn't need them) */
    argv[argc++] = "--mode";
    argv[argc++] = "vanilla-klipper";

    /* Streaming port */
    argv[argc++] = "--streaming-port";
    argv[argc++] = port_str;

    /* Command and control files */
    argv[argc++] = "--cmd-file";
    argv[argc++] = (char *)cmd_file;
    argv[argc++] = "--ctrl-file";
    argv[argc++] = (char *)ctrl_file;

    /* Verbose */
    argv[argc++] = "-v";

    argv[argc] = NULL;
    return argc;
}

int procmgr_start_camera(ManagedProcess *proc, const CameraInfo *cam,
                          const AppConfig *cfg, const char *binary_path) {
    if (!proc || !cam || !cfg || !binary_path) return -1;
    if (proc->pid > 0) return 0;  /* Already running */

    /* Set up cmd/ctrl file paths */
    if (cam->camera_id == 1) {
        snprintf(proc->cmd_file, sizeof(proc->cmd_file), "/tmp/h264_cmd");
        snprintf(proc->ctrl_file, sizeof(proc->ctrl_file), "/tmp/h264_ctrl");
    } else {
        snprintf(proc->cmd_file, sizeof(proc->cmd_file),
                 "/tmp/h264_cmd_%d", cam->camera_id);
        snprintf(proc->ctrl_file, sizeof(proc->ctrl_file),
                 "/tmp/h264_ctrl_%d", cam->camera_id);
    }

    /* Build argv */
    char *argv[PROCMGR_MAX_ARGS];
    build_secondary_args(argv, PROCMGR_MAX_ARGS, cam, cfg, proc,
                         binary_path, proc->cmd_file, proc->ctrl_file);

    /* Log the command */
    fprintf(stderr, "ProcMgr: Starting CAM#%d: %s on port %d\n",
            cam->camera_id, cam->device, cam->streaming_port);
    fprintf(stderr, "ProcMgr:   cmd:");
    for (int i = 0; argv[i]; i++)
        fprintf(stderr, " %s", argv[i]);
    fprintf(stderr, "\n");

    pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, "ProcMgr: fork() failed: %s\n", strerror(errno));
        return -1;
    }

    if (pid == 0) {
        /* Child process */
        /* Close parent's file descriptors (stdin is fine to keep) */

        /* Redirect stdout/stderr to log */
        /* Child will log to its own stderr (inherited from parent) */

        execv(binary_path, argv);

        /* If execv returns, it failed */
        fprintf(stderr, "ProcMgr: execv(%s) failed: %s\n",
                binary_path, strerror(errno));
        _exit(127);
    }

    /* Parent process */
    proc->pid = pid;
    proc->camera_id = cam->camera_id;
    snprintf(proc->device, sizeof(proc->device), "%s", cam->device);
    proc->streaming_port = cam->streaming_port;
    proc->last_start = time(NULL);
    proc->enabled = 1;

    fprintf(stderr, "ProcMgr: CAM#%d started (PID %d)\n",
            cam->camera_id, pid);

    return 0;
}

void procmgr_stop_camera(ManagedProcess *proc) {
    if (!proc || proc->pid <= 0) return;

    fprintf(stderr, "ProcMgr: Stopping CAM#%d (PID %d)\n",
            proc->camera_id, proc->pid);

    /* Send SIGTERM first */
    kill(proc->pid, SIGTERM);

    /* Wait up to 2 seconds for graceful exit */
    for (int i = 0; i < 20; i++) {
        int status;
        pid_t result = waitpid(proc->pid, &status, WNOHANG);
        if (result == proc->pid) {
            fprintf(stderr, "ProcMgr: CAM#%d exited (status %d)\n",
                    proc->camera_id, WEXITSTATUS(status));
            proc->pid = 0;
            return;
        }
        usleep(100000);  /* 100ms */
    }

    /* Force kill */
    fprintf(stderr, "ProcMgr: Force killing CAM#%d (PID %d)\n",
            proc->camera_id, proc->pid);
    kill(proc->pid, SIGKILL);
    waitpid(proc->pid, NULL, 0);
    proc->pid = 0;
}

void procmgr_stop_all(ManagedProcess *procs, int count) {
    /* Send SIGTERM to all first */
    for (int i = 0; i < count; i++) {
        if (procs[i].pid > 0) {
            kill(procs[i].pid, SIGTERM);
        }
    }

    /* Wait briefly for graceful exit */
    usleep(500000);  /* 500ms */

    /* Then stop each one (will force kill if needed) */
    for (int i = 0; i < count; i++) {
        if (procs[i].pid > 0) {
            int status;
            pid_t result = waitpid(procs[i].pid, &status, WNOHANG);
            if (result == procs[i].pid) {
                procs[i].pid = 0;
            } else {
                /* Still running, force kill */
                kill(procs[i].pid, SIGKILL);
                waitpid(procs[i].pid, NULL, 0);
                procs[i].pid = 0;
            }
        }
    }
}

int procmgr_check_children(ManagedProcess *procs, int count,
                            const AppConfig *cfg, const char *binary_path,
                            const CameraInfo *cameras) {
    int restarted = 0;

    for (int i = 0; i < count; i++) {
        ManagedProcess *proc = &procs[i];
        if (proc->pid <= 0 || !proc->enabled) continue;

        int status;
        pid_t result = waitpid(proc->pid, &status, WNOHANG);

        if (result == proc->pid) {
            /* Child exited */
            if (WIFEXITED(status)) {
                fprintf(stderr, "ProcMgr: CAM#%d (PID %d) exited with status %d\n",
                        proc->camera_id, proc->pid, WEXITSTATUS(status));
            } else if (WIFSIGNALED(status)) {
                fprintf(stderr, "ProcMgr: CAM#%d (PID %d) killed by signal %d\n",
                        proc->camera_id, proc->pid, WTERMSIG(status));
            }
            proc->pid = 0;

            /* Check restart backoff */
            time_t now = time(NULL);
            if (now - proc->restart_window_start >= 60) {
                /* New minute window */
                proc->restart_count = 0;
                proc->restart_window_start = now;
            }

            if (proc->restart_count >= PROCMGR_MAX_RESTARTS_PER_MIN) {
                fprintf(stderr, "ProcMgr: CAM#%d exceeded restart limit, disabling\n",
                        proc->camera_id);
                proc->enabled = 0;
                continue;
            }

            /* Find matching camera info */
            const CameraInfo *cam = NULL;
            for (int j = 0; j < CAMERA_MAX; j++) {
                if (cameras[j].camera_id == proc->camera_id) {
                    cam = &cameras[j];
                    break;
                }
            }

            if (cam) {
                /* Delay before restart (exponential: 1s, 2s, 4s) */
                int delay = 1 << proc->restart_count;
                if (delay > 4) delay = 4;
                fprintf(stderr, "ProcMgr: Restarting CAM#%d in %ds...\n",
                        proc->camera_id, delay);
                sleep(delay);

                proc->restart_count++;
                if (procmgr_start_camera(proc, cam, cfg, binary_path) == 0) {
                    restarted++;
                }
            }
        } else if (result < 0 && errno == ECHILD) {
            /* Process doesn't exist anymore */
            proc->pid = 0;
        }
    }

    return restarted;
}

void procmgr_signal_all(ManagedProcess *procs, int count, int sig) {
    for (int i = 0; i < count; i++) {
        if (procs[i].pid > 0) {
            kill(procs[i].pid, sig);
        }
    }
}
