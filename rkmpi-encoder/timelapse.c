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
 * Find the next available sequence number for a given gcode name.
 * Checks existing files in output directory to avoid overwriting.
 */
static int find_next_sequence(const char *gcode_name) {
    DIR *dir = opendir(TIMELAPSE_OUTPUT_DIR);
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

int timelapse_init(const char *gcode_filepath) {
    if (!gcode_filepath || strlen(gcode_filepath) == 0) {
        timelapse_log("Init failed: no gcode filepath\n");
        return -1;
    }

    /* Cancel any existing timelapse */
    if (g_timelapse.active) {
        timelapse_log("Canceling existing timelapse\n");
        timelapse_cancel();
    }

    /* Extract gcode name */
    extract_gcode_name(gcode_filepath, g_timelapse.gcode_name,
                       sizeof(g_timelapse.gcode_name));

    if (strlen(g_timelapse.gcode_name) == 0) {
        timelapse_log("Init failed: could not extract gcode name\n");
        return -1;
    }

    /* Find next sequence number */
    g_timelapse.sequence_num = find_next_sequence(g_timelapse.gcode_name);

    /* Create unique temp directory */
    snprintf(g_timelapse.temp_dir, sizeof(g_timelapse.temp_dir),
             "%s_%d", TIMELAPSE_TEMP_DIR, getpid());

    if (ensure_directory(g_timelapse.temp_dir) != 0) {
        return -1;
    }

    /* Ensure output directory exists */
    if (ensure_directory(TIMELAPSE_OUTPUT_DIR) != 0) {
        cleanup_temp_dir(g_timelapse.temp_dir);
        return -1;
    }

    /* Initialize state */
    g_timelapse.frame_count = 0;
    g_timelapse.active = 1;

    timelapse_log("Started: %s (seq %02d), frames -> %s\n",
                  g_timelapse.gcode_name, g_timelapse.sequence_num,
                  g_timelapse.temp_dir);

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

    /* Build output filenames */
    char output_mp4[TIMELAPSE_PATH_MAX];
    char output_thumb[TIMELAPSE_PATH_MAX];
    char last_frame[TIMELAPSE_PATH_MAX];

    snprintf(output_mp4, sizeof(output_mp4), "%s%s_%02d.mp4",
             TIMELAPSE_OUTPUT_DIR, g_timelapse.gcode_name,
             g_timelapse.sequence_num);

    snprintf(output_thumb, sizeof(output_thumb), "%s%s_%02d_%d.jpg",
             TIMELAPSE_OUTPUT_DIR, g_timelapse.gcode_name,
             g_timelapse.sequence_num, g_timelapse.frame_count);

    snprintf(last_frame, sizeof(last_frame), "%s/frame_%04d.jpg",
             g_timelapse.temp_dir, g_timelapse.frame_count - 1);

    /* Assemble MP4 using ffmpeg */
    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
             "ffmpeg -y -framerate 10 -i '%s/frame_%%04d.jpg' "
             "-c:v libx264 -pix_fmt yuv420p -preset fast "
             "-crf 23 '%s' >/dev/null 2>&1",
             g_timelapse.temp_dir, output_mp4);

    timelapse_log("Running ffmpeg...\n");
    int ret = system(cmd);

    if (ret != 0) {
        timelapse_log("ffmpeg failed with code %d\n", ret);
        /* Try without libx264 (use mpeg4 as fallback) */
        snprintf(cmd, sizeof(cmd),
                 "ffmpeg -y -framerate 10 -i '%s/frame_%%04d.jpg' "
                 "-c:v mpeg4 -q:v 5 '%s' >/dev/null 2>&1",
                 g_timelapse.temp_dir, output_mp4);
        timelapse_log("Retrying with mpeg4 codec...\n");
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

    /* Reset state */
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
