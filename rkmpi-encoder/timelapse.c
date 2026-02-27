/*
 * Timelapse Recording Implementation
 *
 * Primary: Hardware VENC H.264 encoding with minimp4 muxing (no dependencies)
 * Fallback: FFmpeg with libx264 software encoding
 */

/* Disable format-truncation warnings - snprintf truncation is intentional and safe */
#pragma GCC diagnostic ignored "-Wformat-truncation"

#include "timelapse.h"
#include "timelapse_venc.h"
#include "frame_buffer.h"
#include "turbojpeg.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <pthread.h>

/* Global timelapse state */
TimelapseState g_timelapse = {0};

/* Default configuration values */
#define DEFAULT_OUTPUT_FPS          30
#define DEFAULT_CRF                 23
#define DEFAULT_VARIABLE_FPS_MIN    5
#define DEFAULT_VARIABLE_FPS_MAX    60
#define DEFAULT_TARGET_LENGTH       10

/* USB recovery frames directory (fallback when encoding fails) */
#define TIMELAPSE_USB_RECOVERY_DIR  "/mnt/udisk/Time-lapse-Frames-Recovery"

/* Logging helper */
static void timelapse_log(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    fprintf(stderr, "TIMELAPSE: ");
    vfprintf(stderr, fmt, args);
    va_end(args);
    fflush(stderr);
}

/*
 * Validate a path: reject shell metacharacters and path traversal.
 * Returns 0 if valid, -1 if invalid.
 */
static int sanitize_path(const char *path) {
    if (!path || strlen(path) == 0) return -1;
    /* Reject shell metacharacters */
    for (const char *p = path; *p; p++) {
        if (*p == '\'' || *p == '"' || *p == ';' || *p == '`' ||
            *p == '$' || *p == '|' || *p == '&' || *p == '\n' || *p == '\r')
            return -1;
    }
    /* Reject path traversal (.. components) */
    if (strstr(path, "..") != NULL) return -1;
    return 0;
}

/*
 * Validate a filename: no path separators or metacharacters.
 * Returns 0 if valid, -1 if invalid.
 */
static int sanitize_filename(const char *name) {
    if (sanitize_path(name) != 0) return -1;
    if (strchr(name, '/') != NULL) return -1;
    return 0;
}

/*
 * Copy a file using direct I/O (no shell).
 * Returns 0 on success, -1 on error.
 */
static int copy_file(const char *src, const char *dst) {
    int src_fd = open(src, O_RDONLY);
    if (src_fd < 0) {
        timelapse_log("copy_file: cannot open source %s: %s\n", src, strerror(errno));
        return -1;
    }

    int dst_fd = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (dst_fd < 0) {
        timelapse_log("copy_file: cannot open dest %s: %s\n", dst, strerror(errno));
        close(src_fd);
        return -1;
    }

    char buf[8192];
    ssize_t nread;
    while ((nread = read(src_fd, buf, sizeof(buf))) > 0) {
        ssize_t written = 0;
        while (written < nread) {
            ssize_t n = write(dst_fd, buf + written, nread - written);
            if (n < 0) {
                timelapse_log("copy_file: write error: %s\n", strerror(errno));
                close(src_fd);
                close(dst_fd);
                unlink(dst);
                return -1;
            }
            written += n;
        }
    }

    close(src_fd);
    close(dst_fd);

    if (nread < 0) {
        timelapse_log("copy_file: read error: %s\n", strerror(errno));
        unlink(dst);
        return -1;
    }

    return 0;
}

/*
 * Run ffmpeg using fork/execv (no shell).
 * Returns ffmpeg exit code, or -1 on fork/exec error.
 */
static int run_ffmpeg(const char *ffmpeg_path, const char *ld_library_path,
                      int fps, const char *input_pattern, const char *vf_filter,
                      const char *codec, int crf, int q_v,
                      const char *output_path) {
    pid_t pid = fork();
    if (pid < 0) {
        timelapse_log("run_ffmpeg: fork failed: %s\n", strerror(errno));
        return -1;
    }

    if (pid == 0) {
        /* Child process */

        /* Set LD_LIBRARY_PATH if needed (for stock ffmpeg fallback) */
        if (ld_library_path) {
            const char *existing = getenv("LD_LIBRARY_PATH");
            if (existing) {
                char new_path[1024];
                snprintf(new_path, sizeof(new_path), "%s:%s", ld_library_path, existing);
                setenv("LD_LIBRARY_PATH", new_path, 1);
            } else {
                setenv("LD_LIBRARY_PATH", ld_library_path, 1);
            }
        }

        /* Redirect stdout/stderr to /dev/null */
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }

        /* Build argv */
        char fps_str[16];
        snprintf(fps_str, sizeof(fps_str), "%d", fps);

        char crf_str[16];
        snprintf(crf_str, sizeof(crf_str), "%d", crf);

        char qv_str[16];
        snprintf(qv_str, sizeof(qv_str), "%d", q_v);

        /* Max 30 args should be plenty */
        const char *argv[30];
        int argc = 0;

        argv[argc++] = ffmpeg_path;
        argv[argc++] = "-y";
        argv[argc++] = "-framerate";
        argv[argc++] = fps_str;
        argv[argc++] = "-i";
        argv[argc++] = input_pattern;

        if (vf_filter && vf_filter[0] != '\0') {
            argv[argc++] = "-vf";
            argv[argc++] = vf_filter;
        }

        argv[argc++] = "-c:v";
        argv[argc++] = codec;

        if (strcmp(codec, "libx264") == 0) {
            argv[argc++] = "-preset";
            argv[argc++] = "ultrafast";
            argv[argc++] = "-tune";
            argv[argc++] = "zerolatency";
            argv[argc++] = "-x264-params";
            argv[argc++] = "keyint=30:min-keyint=10:scenecut=0:bframes=0:ref=1:rc-lookahead=0:threads=1";
            argv[argc++] = "-crf";
            argv[argc++] = crf_str;
            argv[argc++] = "-pix_fmt";
            argv[argc++] = "yuv420p";
        } else if (strcmp(codec, "mpeg4") == 0) {
            argv[argc++] = "-q:v";
            argv[argc++] = qv_str;
        }

        argv[argc++] = output_path;
        argv[argc] = NULL;

        execv(ffmpeg_path, (char *const *)argv);
        _exit(127);  /* exec failed */
    }

    /* Parent: wait for child (retry on EINTR from signals) */
    int status;
    while (1) {
        pid_t ret = waitpid(pid, &status, 0);
        if (ret < 0) {
            if (errno == EINTR) continue;  /* Signal interrupted, retry */
            timelapse_log("run_ffmpeg: waitpid failed: %s\n", strerror(errno));
            return -1;
        }
        break;
    }

    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    return -1;
}

/*
 * Quick JPEG validation without full decode.
 * Checks markers to detect corruption during capture.
 * Returns 1 if valid, 0 if corrupt.
 */
static int validate_jpeg_full(const uint8_t *data, size_t size) {
    /* Minimum JPEG size */
    if (!data || size < 100) {
        return 0;
    }

    /* Check SOI marker (Start of Image): FFD8 */
    if (data[0] != 0xFF || data[1] != 0xD8) {
        return 0;
    }

    /* Check EOI marker (End of Image): FFD9 at end */
    if (data[size - 2] != 0xFF || data[size - 1] != 0xD9) {
        return 0;
    }

    /* Full scan for multiple SOI markers (concatenated frames)
     * This catches corruption that the quick scan misses */
    int soi_count = 0;
    int eoi_count = 0;
    for (size_t i = 0; i < size - 1; i++) {
        if (data[i] == 0xFF) {
            uint8_t marker = data[i + 1];
            if (marker == 0xD8) {  /* SOI */
                soi_count++;
                if (soi_count > 1) {
                    return 0;  /* Multiple SOI = concatenated frames */
                }
            } else if (marker == 0xD9) {  /* EOI */
                eoi_count++;
                /* EOI should only appear at the very end */
                if (i != size - 2) {
                    return 0;  /* Premature EOI = truncated or concatenated */
                }
            }
        }
    }

    /* Sanity check: exactly one SOI and one EOI */
    if (soi_count != 1 || eoi_count != 1) {
        return 0;
    }

    return 1;  /* Valid JPEG structure */
}

/*
 * Clean up temporary frame JPEGs after encoding.
 */
static void cleanup_temp_frames(void) {
    if (g_timelapse.temp_dir[0] == '\0') {
        return;
    }

    timelapse_log("Cleaning up temp frames in %s\n", g_timelapse.temp_dir);

    DIR *dir = opendir(g_timelapse.temp_dir);
    if (!dir) {
        return;
    }

    struct dirent *entry;
    char filepath[TIMELAPSE_PATH_MAX];

    while ((entry = readdir(dir)) != NULL) {
        if (strncmp(entry->d_name, "frame_", 6) == 0 &&
            strstr(entry->d_name, ".jpg") != NULL) {
            snprintf(filepath, sizeof(filepath), "%s/%s",
                     g_timelapse.temp_dir, entry->d_name);
            unlink(filepath);
        }
    }

    closedir(dir);

    /* Try to remove the temp directory (will fail if not empty, which is fine) */
    rmdir(g_timelapse.temp_dir);
}

/*
 * Reset configuration to defaults.
 */
void timelapse_reset_config(void) {
    g_timelapse.config.output_fps = DEFAULT_OUTPUT_FPS;
    g_timelapse.config.crf = DEFAULT_CRF;
    g_timelapse.config.variable_fps = 0;
    g_timelapse.config.target_length = DEFAULT_TARGET_LENGTH;
    g_timelapse.config.variable_fps_min = DEFAULT_VARIABLE_FPS_MIN;
    g_timelapse.config.variable_fps_max = DEFAULT_VARIABLE_FPS_MAX;
    g_timelapse.config.duplicate_last_frame = 0;
    g_timelapse.config.flip_x = 0;
    g_timelapse.config.flip_y = 0;
    g_timelapse.config.output_dir[0] = '\0';
    g_timelapse.use_venc = 1;  /* Default to hardware VENC encoding */
}

void timelapse_set_use_venc(int enabled) {
    g_timelapse.use_venc = enabled ? 1 : 0;
    timelapse_log("Set use_venc: %d\n", g_timelapse.use_venc);
}

/*
 * Configuration setters.
 */
void timelapse_set_fps(int fps) {
    if (fps >= 1 && fps <= 120) {
        g_timelapse.config.output_fps = fps;
        timelapse_log("Set output FPS: %d\n", fps);
    }
}

void timelapse_set_crf(int crf) {
    if (crf >= 0 && crf <= 51) {
        g_timelapse.config.crf = crf;
        timelapse_log("Set CRF: %d\n", crf);
    }
}

void timelapse_set_variable_fps(int min_fps, int max_fps, int target_length) {
    g_timelapse.config.variable_fps = 1;
    g_timelapse.config.variable_fps_min = (min_fps >= 1) ? min_fps : DEFAULT_VARIABLE_FPS_MIN;
    g_timelapse.config.variable_fps_max = (max_fps >= 1) ? max_fps : DEFAULT_VARIABLE_FPS_MAX;
    g_timelapse.config.target_length = (target_length >= 1) ? target_length : DEFAULT_TARGET_LENGTH;
    timelapse_log("Set variable FPS: min=%d, max=%d, target=%ds\n",
                  g_timelapse.config.variable_fps_min,
                  g_timelapse.config.variable_fps_max,
                  g_timelapse.config.target_length);
}

void timelapse_set_duplicate_last(int count) {
    if (count >= 0 && count <= 60) {
        g_timelapse.config.duplicate_last_frame = count;
        timelapse_log("Set duplicate last frame: %d\n", count);
    }
}

void timelapse_set_flip(int flip_x, int flip_y) {
    g_timelapse.config.flip_x = flip_x ? 1 : 0;
    g_timelapse.config.flip_y = flip_y ? 1 : 0;
    timelapse_log("Set flip: x=%d, y=%d\n", g_timelapse.config.flip_x, g_timelapse.config.flip_y);
}

void timelapse_set_output_dir(const char *dir) {
    if (dir && strlen(dir) > 0 && strlen(dir) < TIMELAPSE_PATH_MAX) {
        if (sanitize_path(dir) != 0) {
            timelapse_log("Rejected output dir (invalid chars): %s\n", dir);
            return;
        }
        strncpy(g_timelapse.config.output_dir, dir, TIMELAPSE_PATH_MAX - 1);
        g_timelapse.config.output_dir[TIMELAPSE_PATH_MAX - 1] = '\0';

        /* Strip trailing slashes to prevent double slashes in paths */
        size_t len = strlen(g_timelapse.config.output_dir);
        while (len > 1 && g_timelapse.config.output_dir[len - 1] == '/') {
            g_timelapse.config.output_dir[--len] = '\0';
        }

        timelapse_log("Set output directory: %s\n", g_timelapse.config.output_dir);
    }
}

void timelapse_set_temp_dir(const char *dir) {
    if (dir && strlen(dir) > 0 && strlen(dir) < TIMELAPSE_PATH_MAX) {
        if (sanitize_path(dir) != 0) {
            timelapse_log("Rejected temp dir (invalid chars): %s\n", dir);
            return;
        }
        strncpy(g_timelapse.config.temp_dir_base, dir, TIMELAPSE_PATH_MAX - 1);
        g_timelapse.config.temp_dir_base[TIMELAPSE_PATH_MAX - 1] = '\0';
        timelapse_log("Set temp directory base: %s\n", g_timelapse.config.temp_dir_base);
    }
}

/*
 * Get the effective temp directory base.
 */
static const char* get_temp_dir_base(void) {
    if (g_timelapse.config.temp_dir_base[0] != '\0') {
        return g_timelapse.config.temp_dir_base;
    }
    return TIMELAPSE_TEMP_DIR;
}

/*
 * Get the effective output directory.
 */
static const char* get_output_dir(void) {
    if (g_timelapse.config.output_dir[0] != '\0') {
        return g_timelapse.config.output_dir;
    }
    return TIMELAPSE_OUTPUT_DIR;
}

/*
 * Find the next available sequence number for a given gcode name.
 * Checks existing files in output directory to avoid overwriting.
 */
static int find_next_sequence(const char *gcode_name) {
    const char *output_dir = get_output_dir();
    DIR *dir = opendir(output_dir);
    if (!dir) {
        return 1;  /* Directory doesn't exist, start at 1 */
    }

    int max_seq = 0;
    struct dirent *entry;
    char pattern[TIMELAPSE_NAME_MAX + 8];
    snprintf(pattern, sizeof(pattern), "%s_", gcode_name);
    size_t pattern_len = strlen(pattern);

    while ((entry = readdir(dir)) != NULL) {
        /* Check if filename starts with our pattern */
        if (strncmp(entry->d_name, pattern, pattern_len) == 0) {
            /* Extract sequence number: name_XX.mp4 or name_XX_frames.jpg */
            const char *seq_start = entry->d_name + pattern_len;
            int seq = atoi(seq_start);
            if (seq > max_seq) {
                max_seq = seq;
            }
        }
    }

    closedir(dir);
    return max_seq + 1;
}

/*
 * Extract base name from gcode filepath.
 * "/useremain/app/gk/gcodes/Foo_PLA_0.2_1h.gcode" -> "Foo_PLA_0.2_1h"
 */
static void extract_gcode_name(const char *filepath, char *name, size_t name_size) {
    /* Find last slash */
    const char *basename = strrchr(filepath, '/');
    basename = basename ? basename + 1 : filepath;

    /* Copy to output buffer */
    strncpy(name, basename, name_size - 1);
    name[name_size - 1] = '\0';

    /* Remove .gcode extension */
    char *ext = strstr(name, ".gcode");
    if (ext) {
        *ext = '\0';
    }
}

/*
 * Create directory if it doesn't exist.
 */
static int ensure_directory(const char *path) {
    struct stat st;
    if (stat(path, &st) == 0) {
        return S_ISDIR(st.st_mode) ? 0 : -1;
    }

    if (mkdir(path, 0755) != 0 && errno != EEXIST) {
        timelapse_log("Failed to create directory %s: %s\n", path, strerror(errno));
        return -1;
    }
    return 0;
}

/*
 * Remove all files in a directory and the directory itself.
 */
static void cleanup_temp_dir(const char *path) {
    DIR *dir = opendir(path);
    if (!dir) return;

    struct dirent *entry;
    char filepath[TIMELAPSE_PATH_MAX];

    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        snprintf(filepath, sizeof(filepath), "%s/%s", path, entry->d_name);
        unlink(filepath);
    }

    closedir(dir);
    rmdir(path);
}

int timelapse_init(const char *gcode_name, const char *output_dir) {
    if (!gcode_name || strlen(gcode_name) == 0) {
        timelapse_log("Init failed: no gcode name\n");
        return -1;
    }

    /* Validate gcode_name (filename only, no path separators) */
    if (sanitize_filename(gcode_name) != 0) {
        timelapse_log("Init failed: invalid gcode name\n");
        return -1;
    }

    /* Validate output_dir if provided */
    if (output_dir && strlen(output_dir) > 0) {
        if (sanitize_path(output_dir) != 0) {
            timelapse_log("Init failed: invalid output dir\n");
            return -1;
        }
    }

    /* Cancel any existing timelapse */
    if (g_timelapse.active) {
        timelapse_log("Canceling existing timelapse\n");
        timelapse_cancel();
    }

    /* Apply defaults for any unset config values (0 means not configured) */
    if (g_timelapse.config.output_fps <= 0) {
        g_timelapse.config.output_fps = DEFAULT_OUTPUT_FPS;
    }
    if (g_timelapse.config.crf <= 0) {
        g_timelapse.config.crf = DEFAULT_CRF;
    }

    /* This is a custom timelapse init from h264_server */
    g_timelapse.custom_mode = 1;

    /* Use provided output dir or keep configured/default */
    if (output_dir && strlen(output_dir) > 0) {
        timelapse_set_output_dir(output_dir);
    }

    /* Copy gcode name */
    strncpy(g_timelapse.gcode_name, gcode_name, TIMELAPSE_NAME_MAX - 1);
    g_timelapse.gcode_name[TIMELAPSE_NAME_MAX - 1] = '\0';

    /* Find next sequence number */
    g_timelapse.sequence_num = find_next_sequence(g_timelapse.gcode_name);

    /* Create unique temp directory */
    snprintf(g_timelapse.temp_dir, sizeof(g_timelapse.temp_dir),
             "%s_%d", get_temp_dir_base(), getpid());

    if (ensure_directory(g_timelapse.temp_dir) != 0) {
        return -1;
    }

    /* Ensure output directory exists */
    if (ensure_directory(get_output_dir()) != 0) {
        cleanup_temp_dir(g_timelapse.temp_dir);
        return -1;
    }

    /* Initialize state */
    g_timelapse.frame_count = 0;
    g_timelapse.active = 1;
    g_timelapse.venc_initialized = 0;
    g_timelapse.frame_width = 0;
    g_timelapse.frame_height = 0;

    /* Default to hardware VENC encoding (can be overridden via control file) */
    if (g_timelapse.use_venc == 0) {
        g_timelapse.use_venc = 1;  /* Enable VENC by default */
    }

    timelapse_log("Started: %s (seq %02d), frames -> %s, output -> %s\n",
                  g_timelapse.gcode_name, g_timelapse.sequence_num,
                  g_timelapse.temp_dir, get_output_dir());
    timelapse_log("Config: fps=%d, crf=%d, variable=%d, flip=%d/%d, use_venc=%d\n",
                  g_timelapse.config.output_fps, g_timelapse.config.crf,
                  g_timelapse.config.variable_fps,
                  g_timelapse.config.flip_x, g_timelapse.config.flip_y,
                  g_timelapse.use_venc);

    return 0;
}

int timelapse_init_legacy(const char *gcode_filepath) {
    /* If custom timelapse mode is enabled (h264_server controls timelapse),
     * ignore RPC-initiated timelapse commands */
    if (g_timelapse.custom_mode) {
        timelapse_log("Ignoring RPC timelapse init - custom mode enabled\n");
        return 0;  /* Return success to avoid RPC errors */
    }

    char name[TIMELAPSE_NAME_MAX];
    extract_gcode_name(gcode_filepath, name, sizeof(name));

    /* Reset custom mode for RPC-initiated timelapse */
    g_timelapse.custom_mode = 0;

    /* Use internal init without setting custom_mode */
    if (!gcode_filepath || strlen(gcode_filepath) == 0) {
        timelapse_log("Init failed: no gcode filepath\n");
        return -1;
    }

    /* Cancel any existing timelapse */
    if (g_timelapse.active) {
        timelapse_log("Canceling existing timelapse\n");
        timelapse_cancel();
    }

    /* Apply defaults for any unset config values (0 means not configured) */
    if (g_timelapse.config.output_fps <= 0) {
        g_timelapse.config.output_fps = DEFAULT_OUTPUT_FPS;
    }
    if (g_timelapse.config.crf <= 0) {
        g_timelapse.config.crf = DEFAULT_CRF;
    }

    /* Copy gcode name */
    strncpy(g_timelapse.gcode_name, name, TIMELAPSE_NAME_MAX - 1);
    g_timelapse.gcode_name[TIMELAPSE_NAME_MAX - 1] = '\0';

    /* Find next sequence number */
    g_timelapse.sequence_num = find_next_sequence(g_timelapse.gcode_name);

    /* Create unique temp directory */
    snprintf(g_timelapse.temp_dir, sizeof(g_timelapse.temp_dir),
             "%s_%d", get_temp_dir_base(), getpid());

    if (ensure_directory(g_timelapse.temp_dir) != 0) {
        return -1;
    }

    /* Ensure output directory exists */
    if (ensure_directory(get_output_dir()) != 0) {
        cleanup_temp_dir(g_timelapse.temp_dir);
        return -1;
    }

    /* Initialize state */
    g_timelapse.frame_count = 0;
    g_timelapse.active = 1;
    g_timelapse.custom_mode = 0;  /* RPC mode, not custom */

    timelapse_log("Started (RPC): %s (seq %02d), frames -> %s, output -> %s\n",
                  g_timelapse.gcode_name, g_timelapse.sequence_num,
                  g_timelapse.temp_dir, get_output_dir());

    return 0;
}

int timelapse_capture_frame(void) {
    if (!g_timelapse.active) {
        return -1;
    }

    /*
     * DEFERRED ENCODING: Always save JPEGs to disk during print.
     * Both VENC and FFmpeg paths use the same capture flow.
     * Encoding happens at finalize time, not during print.
     *
     * This avoids CPU spikes during print (JPEG decode is expensive).
     * Frame capture is just a memory copy + disk write (~1% CPU).
     */

    /* Allocate buffer for JPEG data */
    uint8_t *jpeg_buf = malloc(FRAME_BUFFER_MAX_JPEG);
    if (!jpeg_buf) {
        timelapse_log("Frame %d: malloc failed\n", g_timelapse.frame_count);
        return -1;
    }

    /* Copy JPEG from frame buffer */
    uint64_t sequence;
    static uint64_t last_sequence = 0;

    size_t jpeg_size = frame_buffer_copy(&g_jpeg_buffer, jpeg_buf,
                                          FRAME_BUFFER_MAX_JPEG,
                                          &sequence, NULL, NULL);

    if (jpeg_size == 0) {
        timelapse_log("Frame %d: no JPEG data available\n", g_timelapse.frame_count);
        free(jpeg_buf);
        return -1;
    }

    /* Check if we're getting the same frame again (stale data) */
    if (sequence == last_sequence) {
        timelapse_log("Frame %d: skipping duplicate (seq %llu)\n",
                      g_timelapse.frame_count, (unsigned long long)sequence);
        free(jpeg_buf);
        return -1;  /* Skip duplicate frame */
    }
    last_sequence = sequence;

    /* Validate JPEG before saving (full marker scan, no decode) */
    if (!validate_jpeg_full(jpeg_buf, jpeg_size)) {
        timelapse_log("Frame %d: corrupt JPEG (seq %llu, %zu bytes), skipping\n",
                      g_timelapse.frame_count, (unsigned long long)sequence, jpeg_size);
        free(jpeg_buf);
        return -1;  /* Skip corrupt frame */
    }

    /* Build frame filename */
    char filename[TIMELAPSE_PATH_MAX];
    snprintf(filename, sizeof(filename), "%s/frame_%04d.jpg",
             g_timelapse.temp_dir, g_timelapse.frame_count);

    /* Write frame to file */
    FILE *f = fopen(filename, "wb");
    if (!f) {
        timelapse_log("Frame %d: failed to open %s: %s\n",
                      g_timelapse.frame_count, filename, strerror(errno));
        free(jpeg_buf);
        return -1;
    }

    size_t written = fwrite(jpeg_buf, 1, jpeg_size, f);
    fclose(f);
    free(jpeg_buf);

    if (written != jpeg_size) {
        timelapse_log("Frame %d: write incomplete (%zu/%zu)\n",
                      g_timelapse.frame_count, written, jpeg_size);
        unlink(filename);
        return -1;
    }

    g_timelapse.frame_count++;

    /* Log every 10 frames to reduce log spam */
    if (g_timelapse.frame_count % 10 == 0 || g_timelapse.frame_count == 1) {
        timelapse_log("Captured frame %d (%zu bytes)\n",
                      g_timelapse.frame_count, jpeg_size);
    }

    return 0;
}

/*
 * Calculate effective output FPS based on configuration.
 */
static int calculate_output_fps(int frame_count) {
    if (!g_timelapse.config.variable_fps || frame_count <= 0) {
        return g_timelapse.config.output_fps;
    }

    /* Calculate FPS to achieve target video length */
    int target_fps = frame_count / g_timelapse.config.target_length;

    /* Clamp to min/max */
    if (target_fps < g_timelapse.config.variable_fps_min) {
        target_fps = g_timelapse.config.variable_fps_min;
    }
    if (target_fps > g_timelapse.config.variable_fps_max) {
        target_fps = g_timelapse.config.variable_fps_max;
    }

    timelapse_log("Variable FPS: %d frames / %ds target = %d fps (clamped to %d-%d)\n",
                  frame_count, g_timelapse.config.target_length, target_fps,
                  g_timelapse.config.variable_fps_min, g_timelapse.config.variable_fps_max);

    return target_fps;
}

/*
 * Build ffmpeg video filter string.
 */
static void build_video_filter(char *filter, size_t filter_size) {
    filter[0] = '\0';
    int has_filter = 0;

    /* Flip filters */
    if (g_timelapse.config.flip_x && g_timelapse.config.flip_y) {
        strncat(filter, "hflip,vflip", filter_size - strlen(filter) - 1);
        has_filter = 1;
    } else if (g_timelapse.config.flip_x) {
        strncat(filter, "hflip", filter_size - strlen(filter) - 1);
        has_filter = 1;
    } else if (g_timelapse.config.flip_y) {
        strncat(filter, "vflip", filter_size - strlen(filter) - 1);
        has_filter = 1;
    }

    /* If no filter, just return empty string */
    (void)has_filter;  /* Suppress unused warning */
}

int timelapse_finalize(void) {
    if (!g_timelapse.active) {
        timelapse_log("Finalize: not active\n");
        return -1;
    }

    if (g_timelapse.frame_count == 0) {
        timelapse_log("Finalize: no frames captured\n");
        timelapse_cancel();
        return -1;
    }

    timelapse_log("Finalizing %d frames...\n", g_timelapse.frame_count);
    g_timelapse.encode_status = TL_ENCODE_RUNNING;
    snprintf(g_timelapse.encode_detail, sizeof(g_timelapse.encode_detail),
             "Encoding %d frames", g_timelapse.frame_count);

    /* Build output filenames */
    const char *output_dir = get_output_dir();
    char output_mp4[TIMELAPSE_PATH_MAX];
    char output_thumb[TIMELAPSE_PATH_MAX];

    snprintf(output_mp4, sizeof(output_mp4), "%s/%s_%02d.mp4",
             output_dir, g_timelapse.gcode_name,
             g_timelapse.sequence_num);

    snprintf(output_thumb, sizeof(output_thumb), "%s/%s_%02d_%d.jpg",
             output_dir, g_timelapse.gcode_name,
             g_timelapse.sequence_num, g_timelapse.frame_count);

    /* Ensure output directory exists */
    ensure_directory(output_dir);

    /*
     * DEFERRED ENCODING: All JPEGs are saved to disk during print.
     * Now encode them all at once. This avoids CPU spikes during print.
     *
     * VENC path: Hardware H.264 encoding (preferred)
     * FFmpeg path: Software fallback
     */

    /* VENC path: encode all saved JPEGs with hardware encoder */
    if (g_timelapse.use_venc) {
        timelapse_log("Encoder: using VENC hardware encoder, %d frames -> %s\n", g_timelapse.frame_count, output_mp4);

        /* Read first frame to get dimensions */
        char first_frame_path[TIMELAPSE_PATH_MAX];
        snprintf(first_frame_path, sizeof(first_frame_path), "%s/frame_%04d.jpg",
                 g_timelapse.temp_dir, 0);

        FILE *ff = fopen(first_frame_path, "rb");
        if (!ff) {
            timelapse_log("VENC: cannot read first frame, falling back to ffmpeg\n");
            g_timelapse.use_venc = 0;
            goto ffmpeg_path;
        }

        fseek(ff, 0, SEEK_END);
        size_t first_size = ftell(ff);
        fseek(ff, 0, SEEK_SET);

        uint8_t *first_jpeg = malloc(first_size);
        if (!first_jpeg) {
            fclose(ff);
            g_timelapse.use_venc = 0;
            goto ffmpeg_path;
        }

        fread(first_jpeg, 1, first_size, ff);
        fclose(ff);

        /* Parse header for dimensions */
        tjhandle tj = tjInitDecompress();
        if (!tj) {
            free(first_jpeg);
            g_timelapse.use_venc = 0;
            goto ffmpeg_path;
        }

        int width, height, subsamp, colorspace;
        if (tjDecompressHeader3(tj, first_jpeg, first_size, &width, &height,
                                &subsamp, &colorspace) != 0) {
            timelapse_log("VENC: failed to parse JPEG header: %s\n", tjGetErrorStr());
            tjDestroy(tj);
            free(first_jpeg);
            g_timelapse.use_venc = 0;
            goto ffmpeg_path;
        }
        tjDestroy(tj);
        free(first_jpeg);

        g_timelapse.frame_width = width;
        g_timelapse.frame_height = height;

        /* Calculate output FPS */
        int output_fps = calculate_output_fps(g_timelapse.frame_count);

        /* Initialize VENC */
        if (timelapse_venc_init(width, height, output_fps) != 0) {
            timelapse_log("VENC init failed, falling back to ffmpeg\n");
            g_timelapse.use_venc = 0;
            goto ffmpeg_path;
        }
        g_timelapse.venc_initialized = 1;

        timelapse_log("VENC encoding %d frames at %dx%d @ %dfps...\n",
                      g_timelapse.frame_count, width, height, output_fps);

        /* Encode all frames */
        int venc_errors = 0;
        uint8_t *jpeg_buf = malloc(FRAME_BUFFER_MAX_JPEG);
        if (!jpeg_buf) {
            timelapse_venc_finish(NULL);  /* Cleanup */
            g_timelapse.venc_initialized = 0;
            g_timelapse.use_venc = 0;
            goto ffmpeg_path;
        }

        for (int i = 0; i < g_timelapse.frame_count; i++) {
            char frame_path[TIMELAPSE_PATH_MAX];
            snprintf(frame_path, sizeof(frame_path), "%s/frame_%04d.jpg",
                     g_timelapse.temp_dir, i);

            FILE *f = fopen(frame_path, "rb");
            if (!f) {
                timelapse_log("VENC: cannot read frame %d\n", i);
                venc_errors++;
                continue;
            }

            fseek(f, 0, SEEK_END);
            size_t jpeg_size = ftell(f);
            fseek(f, 0, SEEK_SET);

            if (jpeg_size > FRAME_BUFFER_MAX_JPEG) {
                timelapse_log("VENC: frame %d too large (%zu bytes)\n", i, jpeg_size);
                fclose(f);
                venc_errors++;
                continue;
            }

            size_t bytes_read = fread(jpeg_buf, 1, jpeg_size, f);
            fclose(f);

            if (bytes_read != jpeg_size) {
                timelapse_log("VENC: frame %d read incomplete (%zu/%zu)\n",
                              i, bytes_read, jpeg_size);
                venc_errors++;
                continue;
            }

            /* Validate JPEG before encoding */
            if (!validate_jpeg_full(jpeg_buf, jpeg_size)) {
                timelapse_log("VENC: frame %d on disk is corrupt (%zu bytes, SOI=%02x%02x, EOI=%02x%02x)\n",
                              i, jpeg_size, jpeg_buf[0], jpeg_buf[1],
                              jpeg_buf[jpeg_size-2], jpeg_buf[jpeg_size-1]);
                venc_errors++;
                continue;
            }

            if (timelapse_venc_add_frame(jpeg_buf, jpeg_size) != 0) {
                venc_errors++;
            }

            /* Progress logging */
            if ((i + 1) % 50 == 0 || i == g_timelapse.frame_count - 1) {
                timelapse_log("VENC: encoded %d/%d frames (%d errors)\n",
                              i + 1, g_timelapse.frame_count, venc_errors);
            }
        }

        free(jpeg_buf);

        /* Finalize MP4 */
        int ret = timelapse_venc_finish(output_mp4);
        g_timelapse.venc_initialized = 0;

        if (ret == 0) {
            timelapse_log("Encoder: VENC created %s (%d errors during encode)\n", output_mp4, venc_errors);

            /* Use last saved JPEG as thumbnail */
            char last_frame_path[TIMELAPSE_PATH_MAX];
            snprintf(last_frame_path, sizeof(last_frame_path), "%s/frame_%04d.jpg",
                     g_timelapse.temp_dir, g_timelapse.frame_count - 1);

            if (copy_file(last_frame_path, output_thumb) == 0) {
                timelapse_log("Created thumbnail: %s\n", output_thumb);
            } else {
                timelapse_log("Failed to create thumbnail: %s\n", output_thumb);
            }

            /* Clean up frame JPEGs */
            cleanup_temp_frames();

            /* Set success status */
            g_timelapse.encode_status = TL_ENCODE_SUCCESS;
            snprintf(g_timelapse.encode_detail, sizeof(g_timelapse.encode_detail),
                     "VENC OK (%d frames)", g_timelapse.frame_count);

            /* Reset state */
            g_timelapse.active = 0;
            g_timelapse.frame_count = 0;
            memset(g_timelapse.gcode_name, 0, sizeof(g_timelapse.gcode_name));
            memset(g_timelapse.temp_dir, 0, sizeof(g_timelapse.temp_dir));
            return 0;
        } else {
            timelapse_log("VENC finalize failed, falling back to ffmpeg\n");
            g_timelapse.use_venc = 0;
            /* Fall through to ffmpeg path - frames are still on disk */
        }
    }

ffmpeg_path:
    /* FFmpeg path: assemble JPEGs from disk */

    /* Duplicate last frame if configured */
    if (g_timelapse.config.duplicate_last_frame > 0) {
        char last_frame_path[TIMELAPSE_PATH_MAX];
        snprintf(last_frame_path, sizeof(last_frame_path), "%s/frame_%04d.jpg",
                 g_timelapse.temp_dir, g_timelapse.frame_count - 1);

        for (int i = 0; i < g_timelapse.config.duplicate_last_frame; i++) {
            char dup_frame[TIMELAPSE_PATH_MAX];
            snprintf(dup_frame, sizeof(dup_frame), "%s/frame_%04d.jpg",
                     g_timelapse.temp_dir, g_timelapse.frame_count + i);

            if (copy_file(last_frame_path, dup_frame) != 0) {
                timelapse_log("Failed to duplicate frame to %s\n", dup_frame);
            }
        }
        g_timelapse.frame_count += g_timelapse.config.duplicate_last_frame;
        timelapse_log("Duplicated last frame %d times (total: %d frames)\n",
                      g_timelapse.config.duplicate_last_frame, g_timelapse.frame_count);
    }

    /* Calculate output FPS */
    int output_fps = calculate_output_fps(g_timelapse.frame_count);

    /* Update output filenames (frame count may have changed from duplicates) */
    snprintf(output_thumb, sizeof(output_thumb), "%s/%s_%02d_%d.jpg",
             output_dir, g_timelapse.gcode_name,
             g_timelapse.sequence_num, g_timelapse.frame_count);

    char last_frame[TIMELAPSE_PATH_MAX];
    snprintf(last_frame, sizeof(last_frame), "%s/frame_%04d.jpg",
             g_timelapse.temp_dir, g_timelapse.frame_count - 1);

    /* Build video filter */
    char vf_filter[128];
    build_video_filter(vf_filter, sizeof(vf_filter));

    /* Assemble MP4 using ffmpeg with libx264 (minimal memory settings) */
    int crf = (g_timelapse.config.crf > 0) ? g_timelapse.config.crf : 23;

    /* Build input pattern for ffmpeg */
    char input_pattern[TIMELAPSE_PATH_MAX];
    snprintf(input_pattern, sizeof(input_pattern), "%s/frame_%%04d.jpg", g_timelapse.temp_dir);

    timelapse_log("Encoder: using ffmpeg fallback (fps=%d, crf=%d)...\n", output_fps, crf);
    int ret = run_ffmpeg(TIMELAPSE_FFMPEG_PATH, NULL,
                         output_fps, input_pattern, vf_filter,
                         "libx264", crf, 0, output_mp4);

    /* Fallback 1: Try stock ffmpeg with LD_LIBRARY_PATH if static failed */
    if (ret != 0) {
        timelapse_log("Static ffmpeg failed (code %d), trying stock ffmpeg...\n", ret);
        ret = run_ffmpeg(TIMELAPSE_FFMPEG_STOCK, TIMELAPSE_FFMPEG_LIBS,
                         output_fps, input_pattern, vf_filter,
                         "libx264", crf, 0, output_mp4);
    }

    /* Fallback 2: Try mpeg4 codec if libx264 fails (e.g., OOM) */
    if (ret != 0) {
        timelapse_log("libx264 failed (code %d), trying mpeg4...\n", ret);
        ret = run_ffmpeg(TIMELAPSE_FFMPEG_STOCK, TIMELAPSE_FFMPEG_LIBS,
                         output_fps, input_pattern, vf_filter,
                         "mpeg4", 0, 5, output_mp4);
    }

    if (ret == 0) {
        timelapse_log("Encoder: ffmpeg created %s\n", output_mp4);

        /* Copy last frame as thumbnail */
        if (copy_file(last_frame, output_thumb) == 0) {
            timelapse_log("Created thumbnail %s\n", output_thumb);
        } else {
            timelapse_log("Failed to create thumbnail %s\n", output_thumb);
        }

        /* Set success status */
        g_timelapse.encode_status = TL_ENCODE_SUCCESS;
        snprintf(g_timelapse.encode_detail, sizeof(g_timelapse.encode_detail),
                 "ffmpeg OK (%d frames)", g_timelapse.frame_count);
    } else {
        timelapse_log("Failed to create MP4 (ffmpeg returned %d)\n", ret);
        g_timelapse.encode_status = TL_ENCODE_FAILED;
        snprintf(g_timelapse.encode_detail, sizeof(g_timelapse.encode_detail),
                 "Encoding failed (%d frames)", g_timelapse.frame_count);
    }

    /* Cleanup temp directory */
    cleanup_temp_dir(g_timelapse.temp_dir);

    /* Reset state but keep config for next recording */
    g_timelapse.active = 0;
    g_timelapse.frame_count = 0;
    memset(g_timelapse.gcode_name, 0, sizeof(g_timelapse.gcode_name));
    memset(g_timelapse.temp_dir, 0, sizeof(g_timelapse.temp_dir));

    return ret == 0 ? 0 : -1;
}

void timelapse_cancel(void) {
    if (!g_timelapse.active) {
        return;
    }

    timelapse_log("Canceling (had %d frames)\n", g_timelapse.frame_count);

    /* Cleanup VENC if it was initialized */
    if (g_timelapse.venc_initialized) {
        timelapse_venc_cancel();
        g_timelapse.venc_initialized = 0;
    }

    /* Cleanup temp directory if it exists */
    if (strlen(g_timelapse.temp_dir) > 0) {
        cleanup_temp_dir(g_timelapse.temp_dir);
    }

    /* Reset state */
    g_timelapse.active = 0;
    g_timelapse.frame_count = 0;
    g_timelapse.frame_width = 0;
    g_timelapse.frame_height = 0;
    memset(g_timelapse.gcode_name, 0, sizeof(g_timelapse.gcode_name));
    memset(g_timelapse.temp_dir, 0, sizeof(g_timelapse.temp_dir));
}

int timelapse_is_active(void) {
    return g_timelapse.active;
}

int timelapse_is_custom_mode(void) {
    return g_timelapse.custom_mode;
}

void timelapse_set_custom_mode(int enabled) {
    g_timelapse.custom_mode = enabled ? 1 : 0;
    timelapse_log("Custom mode %s\n", g_timelapse.custom_mode ? "enabled" : "disabled");
}

int timelapse_get_frame_count(void) {
    return g_timelapse.active ? g_timelapse.frame_count : 0;
}

TimelapseEncodeStatus timelapse_get_encode_status(void) {
    return g_timelapse.encode_status;
}

const char *timelapse_get_encode_detail(void) {
    return g_timelapse.encode_detail;
}

/*
 * Count sequential frame_NNNN.jpg files in a directory.
 * Returns the count (0 if none found).
 */
static int count_frames_in_dir(const char *dir_path) {
    int count = 0;
    char path[TIMELAPSE_PATH_MAX];

    while (1) {
        snprintf(path, sizeof(path), "%s/frame_%04d.jpg", dir_path, count);
        if (access(path, F_OK) != 0) break;
        count++;
    }

    return count;
}

/*
 * Copy orphaned frames to USB stick for manual recovery.
 * Creates a timestamped subdirectory in TIMELAPSE_USB_RECOVERY_DIR.
 * Returns 0 on success, -1 on failure.
 */
static int copy_frames_to_usb(const char *orphan_dir, int frame_count) {
    /* Check if USB is mounted */
    struct stat st;
    if (stat("/mnt/udisk", &st) != 0 || !S_ISDIR(st.st_mode)) {
        timelapse_log("Recovery: USB not available, cannot preserve frames\n");
        return -1;
    }

    /* Create recovery base dir */
    if (ensure_directory(TIMELAPSE_USB_RECOVERY_DIR) != 0) {
        timelapse_log("Recovery: cannot create %s\n", TIMELAPSE_USB_RECOVERY_DIR);
        return -1;
    }

    /* Create timestamped subdirectory */
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    char timestamp[32];
    strftime(timestamp, sizeof(timestamp), "%Y%m%d_%H%M%S", tm);

    char dest_dir[TIMELAPSE_PATH_MAX];
    snprintf(dest_dir, sizeof(dest_dir), "%s/frames_%s_%d",
             TIMELAPSE_USB_RECOVERY_DIR, timestamp, frame_count);

    if (ensure_directory(dest_dir) != 0) {
        return -1;
    }

    /* Copy all frame files */
    int copied = 0;
    for (int i = 0; i < frame_count; i++) {
        char src[TIMELAPSE_PATH_MAX], dst[TIMELAPSE_PATH_MAX];
        snprintf(src, sizeof(src), "%s/frame_%04d.jpg", orphan_dir, i);
        snprintf(dst, sizeof(dst), "%s/frame_%04d.jpg", dest_dir, i);

        if (copy_file(src, dst) == 0) {
            copied++;
        }

        /* Progress logging every 100 frames */
        if ((i + 1) % 100 == 0 || i == frame_count - 1) {
            timelapse_log("Recovery: copied %d/%d frames to USB\n", copied, frame_count);
        }
    }

    if (copied > 0) {
        timelapse_log("Recovery: preserved %d frames in %s\n", copied, dest_dir);
        return 0;
    }

    /* Clean up empty dir on failure */
    rmdir(dest_dir);
    return -1;
}

/*
 * Attempt to recover an orphaned timelapse from saved frames.
 * Tries VENC hardware encoder first, then ffmpeg fallback.
 * Returns 0 on success, -1 on failure.
 */
static int recover_orphaned_frames(const char *orphan_dir, int frame_count) {
    const char *output_dir = get_output_dir();

    /* Ensure output directory exists */
    if (ensure_directory(output_dir) != 0) {
        return -1;
    }

    /* Generate output filename with timestamp for uniqueness */
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    char timestamp[32];
    strftime(timestamp, sizeof(timestamp), "%Y%m%d_%H%M%S", tm);

    char output_mp4[TIMELAPSE_PATH_MAX];
    snprintf(output_mp4, sizeof(output_mp4), "%s/recovered_%s.mp4",
             output_dir, timestamp);

    char output_thumb[TIMELAPSE_PATH_MAX];
    snprintf(output_thumb, sizeof(output_thumb), "%s/recovered_%s_%d.jpg",
             output_dir, timestamp, frame_count);

    /* Use default encoding settings */
    int fps = DEFAULT_OUTPUT_FPS;
    int crf = DEFAULT_CRF;
    int ret = -1;

    timelapse_log("Recovery: encoding %d frames -> %s (fps=%d, crf=%d)\n",
                  frame_count, output_mp4, fps, crf);

    /* === Try VENC hardware encoder first === */
    timelapse_log("Recovery: trying VENC hardware encoder...\n");
    snprintf(g_timelapse.encode_detail, sizeof(g_timelapse.encode_detail),
             "VENC recovery %d frames", frame_count);
    do {
        /* Read first frame to get dimensions */
        char first_frame_path[TIMELAPSE_PATH_MAX];
        snprintf(first_frame_path, sizeof(first_frame_path), "%s/frame_%04d.jpg",
                 orphan_dir, 0);

        FILE *ff = fopen(first_frame_path, "rb");
        if (!ff) {
            timelapse_log("Recovery: VENC: cannot read first frame, skipping VENC\n");
            break;
        }

        fseek(ff, 0, SEEK_END);
        size_t first_size = ftell(ff);
        fseek(ff, 0, SEEK_SET);

        uint8_t *first_jpeg = malloc(first_size);
        if (!first_jpeg) {
            fclose(ff);
            break;
        }

        fread(first_jpeg, 1, first_size, ff);
        fclose(ff);

        /* Parse header for dimensions */
        tjhandle tj = tjInitDecompress();
        if (!tj) {
            free(first_jpeg);
            break;
        }

        int width, height, subsamp, colorspace;
        if (tjDecompressHeader3(tj, first_jpeg, first_size, &width, &height,
                                &subsamp, &colorspace) != 0) {
            timelapse_log("Recovery: VENC: failed to parse JPEG header: %s\n", tjGetErrorStr());
            tjDestroy(tj);
            free(first_jpeg);
            break;
        }
        tjDestroy(tj);
        free(first_jpeg);

        /* Initialize VENC */
        if (timelapse_venc_init(width, height, fps) != 0) {
            timelapse_log("Recovery: VENC init failed (%dx%d), falling back to ffmpeg\n",
                          width, height);
            break;
        }

        timelapse_log("Recovery: VENC encoding %d frames at %dx%d @ %dfps...\n",
                      frame_count, width, height, fps);

        /* Encode all frames */
        int venc_errors = 0;
        uint8_t *jpeg_buf = malloc(FRAME_BUFFER_MAX_JPEG);
        if (!jpeg_buf) {
            timelapse_venc_finish(NULL);
            break;
        }

        for (int i = 0; i < frame_count; i++) {
            char frame_path[TIMELAPSE_PATH_MAX];
            snprintf(frame_path, sizeof(frame_path), "%s/frame_%04d.jpg",
                     orphan_dir, i);

            FILE *f = fopen(frame_path, "rb");
            if (!f) { venc_errors++; continue; }

            fseek(f, 0, SEEK_END);
            size_t jpeg_size = ftell(f);
            fseek(f, 0, SEEK_SET);

            if (jpeg_size > FRAME_BUFFER_MAX_JPEG) {
                fclose(f);
                venc_errors++;
                continue;
            }

            size_t bytes_read = fread(jpeg_buf, 1, jpeg_size, f);
            fclose(f);

            if (bytes_read != jpeg_size) { venc_errors++; continue; }

            if (!validate_jpeg_full(jpeg_buf, jpeg_size)) {
                venc_errors++;
                continue;
            }

            if (timelapse_venc_add_frame(jpeg_buf, jpeg_size) != 0) {
                venc_errors++;
            }

            if ((i + 1) % 50 == 0 || i == frame_count - 1) {
                timelapse_log("Recovery: VENC encoded %d/%d frames (%d errors)\n",
                              i + 1, frame_count, venc_errors);
            }
        }

        free(jpeg_buf);

        ret = timelapse_venc_finish(output_mp4);
        if (ret == 0) {
            timelapse_log("Recovery: VENC created %s (%d errors)\n", output_mp4, venc_errors);
        } else {
            timelapse_log("Recovery: VENC finalize failed, falling back to ffmpeg\n");
        }
    } while (0);

    /* === FFmpeg fallback === */
    if (ret != 0) {
        char input_pattern[TIMELAPSE_PATH_MAX];
        snprintf(input_pattern, sizeof(input_pattern), "%s/frame_%%04d.jpg", orphan_dir);

        snprintf(g_timelapse.encode_detail, sizeof(g_timelapse.encode_detail),
                 "ffmpeg recovery %d frames", frame_count);

        /* Try bundled ffmpeg first */
        timelapse_log("Recovery: trying bundled ffmpeg (libx264)...\n");
        ret = run_ffmpeg(TIMELAPSE_FFMPEG_PATH, NULL,
                         fps, input_pattern, "",
                         "libx264", crf, 0, output_mp4);

        /* Fallback to stock ffmpeg */
        if (ret != 0) {
            timelapse_log("Recovery: trying stock ffmpeg (libx264)...\n");
            ret = run_ffmpeg(TIMELAPSE_FFMPEG_STOCK, TIMELAPSE_FFMPEG_LIBS,
                             fps, input_pattern, "",
                             "libx264", crf, 0, output_mp4);
        }

        /* Fallback to mpeg4 codec */
        if (ret != 0) {
            timelapse_log("Recovery: trying stock ffmpeg (mpeg4)...\n");
            ret = run_ffmpeg(TIMELAPSE_FFMPEG_STOCK, TIMELAPSE_FFMPEG_LIBS,
                             fps, input_pattern, "",
                             "mpeg4", 0, 5, output_mp4);
        }
    }

    if (ret == 0) {
        timelapse_log("Recovery: created %s\n", output_mp4);

        /* Copy last frame as thumbnail */
        char last_frame[TIMELAPSE_PATH_MAX];
        snprintf(last_frame, sizeof(last_frame), "%s/frame_%04d.jpg",
                 orphan_dir, frame_count - 1);
        if (copy_file(last_frame, output_thumb) == 0) {
            timelapse_log("Recovery: created thumbnail %s\n", output_thumb);
        }
    } else {
        timelapse_log("Recovery: all encoding methods failed for %d frames\n", frame_count);

        /* Preserve frames on USB instead of deleting */
        timelapse_log("Recovery: copying frames to USB for manual recovery...\n");
        if (copy_frames_to_usb(orphan_dir, frame_count) == 0) {
            timelapse_log("Recovery: frames preserved on USB\n");
        } else {
            timelapse_log("Recovery: WARNING - could not preserve frames, they will be lost!\n");
        }
    }

    return ret == 0 ? 0 : -1;
}

/*
 * Background thread for orphaned timelapse recovery.
 * Runs at low priority to avoid impacting normal operation.
 */
static void *recovery_thread_func(void *arg) {
    (void)arg;

    /* Lower thread priority to avoid impacting main loop */
    nice(19);

    const char *base = get_temp_dir_base();

    /* Split base into parent dir and prefix
     * base = "/useremain/home/rinkhals/.timelapse_frames"
     * parent = "/useremain/home/rinkhals"
     * prefix = ".timelapse_frames_"
     */
    char parent_dir[TIMELAPSE_PATH_MAX];
    char prefix[256];

    const char *last_slash = strrchr(base, '/');
    if (!last_slash || last_slash == base) return NULL;

    size_t parent_len = last_slash - base;
    if (parent_len >= sizeof(parent_dir)) return NULL;
    memcpy(parent_dir, base, parent_len);
    parent_dir[parent_len] = '\0';

    snprintf(prefix, sizeof(prefix), "%s_", last_slash + 1);
    size_t prefix_len = strlen(prefix);

    DIR *dir = opendir(parent_dir);
    if (!dir) {
        g_timelapse.encode_status = TL_ENCODE_IDLE;
        return NULL;
    }

    pid_t my_pid = getpid();
    struct dirent *entry;
    int recovered = 0;
    int failed = 0;

    while ((entry = readdir(dir)) != NULL) {
        if (strncmp(entry->d_name, prefix, prefix_len) != 0)
            continue;

        /* Extract PID from suffix */
        const char *pid_str = entry->d_name + prefix_len;
        int pid = atoi(pid_str);
        if (pid <= 0) continue;

        /* Skip our own temp dir */
        if (pid == (int)my_pid) continue;

        /* Check if the owning process is still running */
        if (kill(pid, 0) == 0) {
            timelapse_log("Recovery: skipping %s (PID %d still alive)\n",
                          entry->d_name, pid);
            continue;
        }

        /* Build full path */
        char orphan_dir[TIMELAPSE_PATH_MAX];
        snprintf(orphan_dir, sizeof(orphan_dir), "%s/%s", parent_dir, entry->d_name);

        /* Count sequential frames */
        int frame_count = count_frames_in_dir(orphan_dir);

        if (frame_count == 0) {
            timelapse_log("Recovery: removing empty orphaned dir %s\n", orphan_dir);
            cleanup_temp_dir(orphan_dir);
            continue;
        }

        timelapse_log("Recovery: found %d orphaned frames in %s\n",
                      frame_count, orphan_dir);

        /* Update status to running */
        g_timelapse.encode_status = TL_ENCODE_RUNNING;

        /* Try to encode the recovered frames */
        int ret = recover_orphaned_frames(orphan_dir, frame_count);

        /* Clean up original frames (copies are on USB if encoding failed) */
        cleanup_temp_dir(orphan_dir);

        if (ret == 0) {
            recovered++;
        } else {
            failed++;
        }
    }

    closedir(dir);

    if (recovered > 0 || failed > 0) {
        timelapse_log("Recovery: processed %d dir(s): %d recovered, %d failed\n",
                      recovered + failed, recovered, failed);
    }

    /* Set final status */
    if (failed > 0) {
        g_timelapse.encode_status = TL_ENCODE_FAILED;
        snprintf(g_timelapse.encode_detail, sizeof(g_timelapse.encode_detail),
                 "Recovery failed (%d/%d)", failed, recovered + failed);
    } else if (recovered > 0) {
        g_timelapse.encode_status = TL_ENCODE_SUCCESS;
        snprintf(g_timelapse.encode_detail, sizeof(g_timelapse.encode_detail),
                 "Recovery OK (%d)", recovered);
    } else {
        g_timelapse.encode_status = TL_ENCODE_IDLE;
        g_timelapse.encode_detail[0] = '\0';
    }

    return NULL;
}

void timelapse_recover_orphaned(void) {
    /* Quick check: any orphan dirs to process? */
    const char *base = get_temp_dir_base();
    const char *last_slash = strrchr(base, '/');
    if (!last_slash || last_slash == base) return;

    char parent_dir[TIMELAPSE_PATH_MAX];
    size_t parent_len = last_slash - base;
    if (parent_len >= sizeof(parent_dir)) return;
    memcpy(parent_dir, base, parent_len);
    parent_dir[parent_len] = '\0';

    char prefix[256];
    snprintf(prefix, sizeof(prefix), "%s_", last_slash + 1);
    size_t prefix_len = strlen(prefix);

    DIR *dir = opendir(parent_dir);
    if (!dir) return;

    pid_t my_pid = getpid();
    int found = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strncmp(entry->d_name, prefix, prefix_len) != 0) continue;
        int pid = atoi(entry->d_name + prefix_len);
        if (pid <= 0 || pid == (int)my_pid) continue;
        if (kill(pid, 0) == 0) continue;  /* Still alive */
        found = 1;
        break;
    }
    closedir(dir);

    if (!found) return;

    /* Set pending status and launch background thread */
    g_timelapse.encode_status = TL_ENCODE_PENDING;
    snprintf(g_timelapse.encode_detail, sizeof(g_timelapse.encode_detail),
             "Recovery pending");
    timelapse_log("Recovery: launching background thread for orphaned frames\n");

    pthread_t thread;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

    if (pthread_create(&thread, &attr, recovery_thread_func, NULL) != 0) {
        timelapse_log("Recovery: failed to create thread: %s, running synchronously\n",
                      strerror(errno));
        pthread_attr_destroy(&attr);
        /* Fallback: run synchronously */
        recovery_thread_func(NULL);
        return;
    }

    pthread_attr_destroy(&attr);
}
