/*
 * Timelapse Recording Implementation
 *
 * Captures JPEG frames from the encoder's frame buffer and assembles
 * them into MP4 video using ffmpeg.
 */

#include "timelapse.h"
#include "frame_buffer.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>

/* Global timelapse state */
TimelapseState g_timelapse = {0};

/* Default configuration values */
#define DEFAULT_OUTPUT_FPS          30
#define DEFAULT_CRF                 23
#define DEFAULT_VARIABLE_FPS_MIN    5
#define DEFAULT_VARIABLE_FPS_MAX    60
#define DEFAULT_TARGET_LENGTH       10

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
        strncpy(g_timelapse.config.output_dir, dir, TIMELAPSE_PATH_MAX - 1);
        g_timelapse.config.output_dir[TIMELAPSE_PATH_MAX - 1] = '\0';
        timelapse_log("Set output directory: %s\n", g_timelapse.config.output_dir);
    }
}

void timelapse_set_temp_dir(const char *dir) {
    if (dir && strlen(dir) > 0 && strlen(dir) < TIMELAPSE_PATH_MAX) {
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

    /* Cancel any existing timelapse */
    if (g_timelapse.active) {
        timelapse_log("Canceling existing timelapse\n");
        timelapse_cancel();
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

    timelapse_log("Started: %s (seq %02d), frames -> %s, output -> %s\n",
                  g_timelapse.gcode_name, g_timelapse.sequence_num,
                  g_timelapse.temp_dir, get_output_dir());
    timelapse_log("Config: fps=%d, crf=%d, variable=%d, flip=%d/%d\n",
                  g_timelapse.config.output_fps, g_timelapse.config.crf,
                  g_timelapse.config.variable_fps,
                  g_timelapse.config.flip_x, g_timelapse.config.flip_y);

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

    /* Allocate buffer for JPEG data */
    uint8_t *jpeg_buf = malloc(FRAME_BUFFER_MAX_JPEG);
    if (!jpeg_buf) {
        timelapse_log("Frame %d: malloc failed\n", g_timelapse.frame_count);
        return -1;
    }

    /* Copy current JPEG frame from encoder buffer */
    uint64_t sequence;
    size_t jpeg_size = frame_buffer_copy(&g_jpeg_buffer, jpeg_buf,
                                          FRAME_BUFFER_MAX_JPEG,
                                          &sequence, NULL);

    if (jpeg_size == 0) {
        timelapse_log("Frame %d: no JPEG data available\n", g_timelapse.frame_count);
        free(jpeg_buf);
        return -1;
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
    timelapse_log("Captured frame %d (%zu bytes)\n",
                  g_timelapse.frame_count, jpeg_size);

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

    /* Duplicate last frame if configured */
    if (g_timelapse.config.duplicate_last_frame > 0) {
        char last_frame[TIMELAPSE_PATH_MAX];
        snprintf(last_frame, sizeof(last_frame), "%s/frame_%04d.jpg",
                 g_timelapse.temp_dir, g_timelapse.frame_count - 1);

        for (int i = 0; i < g_timelapse.config.duplicate_last_frame; i++) {
            char dup_frame[TIMELAPSE_PATH_MAX];
            snprintf(dup_frame, sizeof(dup_frame), "%s/frame_%04d.jpg",
                     g_timelapse.temp_dir, g_timelapse.frame_count + i);

            char cmd[TIMELAPSE_PATH_MAX * 2 + 32];
            snprintf(cmd, sizeof(cmd), "cp '%s' '%s'", last_frame, dup_frame);
            system(cmd);
        }
        g_timelapse.frame_count += g_timelapse.config.duplicate_last_frame;
        timelapse_log("Duplicated last frame %d times (total: %d frames)\n",
                      g_timelapse.config.duplicate_last_frame, g_timelapse.frame_count);
    }

    /* Calculate output FPS */
    int output_fps = calculate_output_fps(g_timelapse.frame_count);

    /* Build output filenames */
    const char *output_dir = get_output_dir();
    char output_mp4[TIMELAPSE_PATH_MAX];
    char output_thumb[TIMELAPSE_PATH_MAX];
    char last_frame[TIMELAPSE_PATH_MAX];

    snprintf(output_mp4, sizeof(output_mp4), "%s/%s_%02d.mp4",
             output_dir, g_timelapse.gcode_name,
             g_timelapse.sequence_num);

    snprintf(output_thumb, sizeof(output_thumb), "%s/%s_%02d_%d.jpg",
             output_dir, g_timelapse.gcode_name,
             g_timelapse.sequence_num, g_timelapse.frame_count);

    snprintf(last_frame, sizeof(last_frame), "%s/frame_%04d.jpg",
             g_timelapse.temp_dir, g_timelapse.frame_count - 1);

    /* Build video filter */
    char vf_filter[128];
    build_video_filter(vf_filter, sizeof(vf_filter));

    /* Assemble MP4 using ffmpeg with libx264 (minimal memory settings) */
    char cmd[2048];
    int crf = (g_timelapse.config.crf > 0) ? g_timelapse.config.crf : 23;

    if (vf_filter[0] != '\0') {
        snprintf(cmd, sizeof(cmd),
                 TIMELAPSE_FFMPEG_CMD " -y -framerate %d -i '%s/frame_%%04d.jpg' "
                 "-vf '%s' "
                 "-c:v libx264 -preset ultrafast -tune zerolatency "
                 "-x264-params keyint=30:min-keyint=10:scenecut=0:bframes=0:ref=1:rc-lookahead=0:threads=1 "
                 "-crf %d -pix_fmt yuv420p '%s' >/dev/null 2>&1",
                 output_fps, g_timelapse.temp_dir, vf_filter, crf, output_mp4);
    } else {
        snprintf(cmd, sizeof(cmd),
                 TIMELAPSE_FFMPEG_CMD " -y -framerate %d -i '%s/frame_%%04d.jpg' "
                 "-c:v libx264 -preset ultrafast -tune zerolatency "
                 "-x264-params keyint=30:min-keyint=10:scenecut=0:bframes=0:ref=1:rc-lookahead=0:threads=1 "
                 "-crf %d -pix_fmt yuv420p '%s' >/dev/null 2>&1",
                 output_fps, g_timelapse.temp_dir, crf, output_mp4);
    }

    timelapse_log("Running ffmpeg (fps=%d, crf=%d)...\n", output_fps, crf);
    int ret = system(cmd);

    /* Fallback to mpeg4 if libx264 fails (e.g., OOM) */
    if (ret != 0) {
        timelapse_log("libx264 failed (code %d), trying mpeg4...\n", ret);
        if (vf_filter[0] != '\0') {
            snprintf(cmd, sizeof(cmd),
                     TIMELAPSE_FFMPEG_CMD " -y -framerate %d -i '%s/frame_%%04d.jpg' "
                     "-vf '%s' -c:v mpeg4 -q:v 5 '%s' >/dev/null 2>&1",
                     output_fps, g_timelapse.temp_dir, vf_filter, output_mp4);
        } else {
            snprintf(cmd, sizeof(cmd),
                     TIMELAPSE_FFMPEG_CMD " -y -framerate %d -i '%s/frame_%%04d.jpg' "
                     "-c:v mpeg4 -q:v 5 '%s' >/dev/null 2>&1",
                     output_fps, g_timelapse.temp_dir, output_mp4);
        }
        ret = system(cmd);
    }

    if (ret == 0) {
        timelapse_log("Created %s\n", output_mp4);

        /* Copy last frame as thumbnail */
        snprintf(cmd, sizeof(cmd), "cp '%s' '%s'", last_frame, output_thumb);
        system(cmd);
        timelapse_log("Created thumbnail %s\n", output_thumb);
    } else {
        timelapse_log("Failed to create MP4 (ffmpeg returned %d)\n", ret);
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

    /* Cleanup temp directory if it exists */
    if (strlen(g_timelapse.temp_dir) > 0) {
        cleanup_temp_dir(g_timelapse.temp_dir);
    }

    /* Reset state */
    g_timelapse.active = 0;
    g_timelapse.frame_count = 0;
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
