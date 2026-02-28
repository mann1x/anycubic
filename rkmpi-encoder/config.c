/*
 * Configuration Management
 *
 * JSON-based persistent configuration using cJSON.
 * Config values are stored as strings in the JSON file (matching Python behavior).
 */

#include "config.h"
#include "cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

/* Helper: get string from cJSON object, or return default */
static const char *json_get_str(const cJSON *obj, const char *key, const char *def) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (item && cJSON_IsString(item) && item->valuestring) {
        return item->valuestring;
    }
    return def;
}

/* Helper: get int from cJSON (stored as string) */
static int json_get_int(const cJSON *obj, const char *key, int def) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (item) {
        if (cJSON_IsString(item) && item->valuestring) {
            return atoi(item->valuestring);
        }
        if (cJSON_IsNumber(item)) {
            return item->valueint;
        }
    }
    return def;
}

/* Helper: get float from cJSON (stored as string) */
static float json_get_float(const cJSON *obj, const char *key, float def) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (item) {
        if (cJSON_IsString(item) && item->valuestring) {
            return (float)atof(item->valuestring);
        }
        if (cJSON_IsNumber(item)) {
            return (float)item->valuedouble;
        }
    }
    return def;
}

/* Helper: get bool from cJSON (stored as "true"/"false" string) */
static int json_get_bool(const cJSON *obj, const char *key, int def) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (item && cJSON_IsString(item) && item->valuestring) {
        return strcmp(item->valuestring, "true") == 0 ? 1 : 0;
    }
    if (item && cJSON_IsBool(item)) {
        return cJSON_IsTrue(item) ? 1 : 0;
    }
    return def;
}

/* Clamp integer to range */
static int clamp_int(int val, int min, int max) {
    if (val < min) return min;
    if (val > max) return max;
    return val;
}

/* Clamp float to range */
static float clamp_float(float val, float min, float max) {
    if (val < min) return min;
    if (val > max) return max;
    return val;
}

void config_set_defaults(AppConfig *cfg) {
    memset(cfg, 0, sizeof(*cfg));

    /* Encoder */
    strncpy(cfg->encoder_type, "rkmpi-yuyv", sizeof(cfg->encoder_type) - 1);
    cfg->h264_enabled = 1;
    cfg->auto_skip = 1;
    cfg->skip_ratio = 4;
    cfg->target_cpu = 25;
    cfg->bitrate = 512;
    cfg->mjpeg_fps = 10;
    cfg->jpeg_quality = 85;
    strncpy(cfg->h264_resolution, "1280x720", sizeof(cfg->h264_resolution) - 1);

    /* Display */
    cfg->display_enabled = 0;
    cfg->display_fps = 5;

    /* Ports */
    cfg->streaming_port = 8080;
    cfg->control_port = 8081;

    /* Modes */
    strncpy(cfg->mode, "go-klipper", sizeof(cfg->mode) - 1);
    cfg->autolanmode = 1;
    cfg->logging = 0;
    cfg->log_max_size = 1024;
    cfg->acproxycam_flv_proxy = 0;

    /* Internal USB port */
    strncpy(cfg->internal_usb_port, "1.3", sizeof(cfg->internal_usb_port) - 1);

    /* Timelapse */
    cfg->timelapse_enabled = 0;
    strncpy(cfg->timelapse_mode, "layer", sizeof(cfg->timelapse_mode) - 1);
    cfg->timelapse_hyperlapse_interval = 30;
    strncpy(cfg->timelapse_storage, "internal", sizeof(cfg->timelapse_storage) - 1);
    strncpy(cfg->timelapse_usb_path, "/mnt/udisk/timelapse", sizeof(cfg->timelapse_usb_path) - 1);
    cfg->timelapse_output_fps = 30;
    cfg->timelapse_variable_fps = 0;
    cfg->timelapse_target_length = 10;
    cfg->timelapse_variable_fps_min = 5;
    cfg->timelapse_variable_fps_max = 60;
    cfg->timelapse_crf = 23;
    cfg->timelapse_duplicate_last_frame = 0;
    cfg->timelapse_stream_delay = 0.05f;
    cfg->timelapse_flip_x = 0;
    cfg->timelapse_flip_y = 0;
    cfg->timelapse_end_delay = 5.0f;
    strncpy(cfg->moonraker_host, "127.0.0.1", sizeof(cfg->moonraker_host) - 1);
    cfg->moonraker_port = 7125;
    strncpy(cfg->moonraker_camera_ip, "auto", sizeof(cfg->moonraker_camera_ip) - 1);

    /* Camera controls */
    cfg->cam_brightness = 0;
    cfg->cam_contrast = 32;
    cfg->cam_saturation = 85;
    cfg->cam_hue = 0;
    cfg->cam_gamma = 100;
    cfg->cam_sharpness = 3;
    cfg->cam_gain = 1;
    cfg->cam_backlight = 0;
    cfg->cam_wb_auto = 1;
    cfg->cam_wb_temp = 4000;
    cfg->cam_exposure_auto = 3;
    cfg->cam_exposure = 156;
    cfg->cam_exposure_priority = 0;
    cfg->cam_power_line = 1;

    cfg->cameras_json[0] = '\0';

    /* Fault Detection */
    cfg->fault_detect_enabled = 0;
    cfg->fault_detect_cnn_enabled = 0;
    cfg->fault_detect_proto_enabled = 0;
    cfg->fault_detect_multi_enabled = 0;
    strncpy(cfg->fault_detect_strategy, "and", sizeof(cfg->fault_detect_strategy) - 1);
    cfg->fault_detect_interval = 5;
    cfg->fault_detect_verify_interval = 2;
    cfg->fault_detect_model_set[0] = '\0';
    cfg->fault_detect_min_free_mem = 20;
    cfg->fault_detect_pace_ms = 150;
    cfg->heatmap_enabled = 0;
    cfg->fd_beep_pattern = 0;
    cfg->fd_thresholds_json[0] = '\0';

    /* Prototype Management */
    cfg->proto_active_set[0] = '\0';
    snprintf(cfg->proto_dataset_url, sizeof(cfg->proto_dataset_url),
             "https://github.com/mann1x/anycubic/releases/download/datasets/ks1-v7/ks1_default_dataset.tar.gz");

    /* Fault Detection Setup */
    cfg->fd_setup_status = FD_SETUP_NONE;
    cfg->fd_setup_timestamp = 0;
    memset(cfg->fd_setup_corners, 0, sizeof(cfg->fd_setup_corners));
    /* All 392 bits set for 14x28 grid */
    strncpy(cfg->fd_setup_mask_hex,
        "00000000000000ff:ffffffffffffffff:ffffffffffffffff:ffffffffffffffff:ffffffffffffffff:ffffffffffffffff:ffffffffffffffff",
        sizeof(cfg->fd_setup_mask_hex) - 1);
    cfg->fd_bed_size_x = 220;
    cfg->fd_bed_size_y = 220;
    cfg->fd_setup_results_json[0] = '\0';
    cfg->fd_z_masks_json[0] = '\0';
}

int config_load(AppConfig *cfg, const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "Config: Cannot open %s: %s\n", path, strerror(errno));
        return -1;
    }

    /* Read entire file */
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (fsize <= 0 || fsize > 64 * 1024) {
        fclose(f);
        fprintf(stderr, "Config: Invalid file size: %ld\n", fsize);
        return -1;
    }

    char *buf = malloc(fsize + 1);
    if (!buf) {
        fclose(f);
        return -1;
    }

    size_t nread = fread(buf, 1, fsize, f);
    fclose(f);
    buf[nread] = '\0';

    cJSON *root = cJSON_Parse(buf);
    free(buf);

    if (!root) {
        fprintf(stderr, "Config: JSON parse error\n");
        return -1;
    }

    /* Encoder settings */
    const char *enc = json_get_str(root, "encoder_type", cfg->encoder_type);
    if (strcmp(enc, "rkmpi") == 0 || strcmp(enc, "rkmpi-yuyv") == 0) {
        strncpy(cfg->encoder_type, enc, sizeof(cfg->encoder_type) - 1);
    }

    const char *mode = json_get_str(root, "mode", cfg->mode);
    if (strcmp(mode, "go-klipper") == 0 || strcmp(mode, "vanilla-klipper") == 0) {
        strncpy(cfg->mode, mode, sizeof(cfg->mode) - 1);
    }

    cfg->h264_enabled = json_get_bool(root, "h264_enabled", cfg->h264_enabled);
    cfg->auto_skip = json_get_bool(root, "auto_skip", cfg->auto_skip);
    cfg->skip_ratio = clamp_int(json_get_int(root, "skip_ratio", cfg->skip_ratio), 1, 20);
    cfg->target_cpu = clamp_int(json_get_int(root, "target_cpu", cfg->target_cpu), 25, 90);
    cfg->bitrate = clamp_int(json_get_int(root, "bitrate", cfg->bitrate), 100, 4000);
    cfg->mjpeg_fps = clamp_int(json_get_int(root, "mjpeg_fps", cfg->mjpeg_fps), 2, 30);
    cfg->jpeg_quality = clamp_int(json_get_int(root, "jpeg_quality", cfg->jpeg_quality), 1, 99);
    cfg->streaming_port = json_get_int(root, "streaming_port", cfg->streaming_port);
    cfg->control_port = json_get_int(root, "control_port", cfg->control_port);

    const char *h264_res = json_get_str(root, "h264_resolution", cfg->h264_resolution);
    strncpy(cfg->h264_resolution, h264_res, sizeof(cfg->h264_resolution) - 1);

    /* Display */
    cfg->display_enabled = json_get_bool(root, "display_enabled", cfg->display_enabled);
    cfg->display_fps = clamp_int(json_get_int(root, "display_fps", cfg->display_fps), 1, 10);

    /* Modes */
    cfg->autolanmode = json_get_bool(root, "autolanmode", cfg->autolanmode);
    cfg->logging = json_get_bool(root, "logging", cfg->logging);
    cfg->log_max_size = clamp_int(json_get_int(root, "log_max_size", cfg->log_max_size), 100, 5120);
    cfg->acproxycam_flv_proxy = json_get_bool(root, "acproxycam_flv_proxy", cfg->acproxycam_flv_proxy);

    /* Internal USB port */
    const char *usb = json_get_str(root, "internal_usb_port", cfg->internal_usb_port);
    strncpy(cfg->internal_usb_port, usb, sizeof(cfg->internal_usb_port) - 1);

    /* Camera controls */
    cfg->cam_brightness = json_get_int(root, "cam_brightness", cfg->cam_brightness);
    cfg->cam_contrast = json_get_int(root, "cam_contrast", cfg->cam_contrast);
    cfg->cam_saturation = json_get_int(root, "cam_saturation", cfg->cam_saturation);
    cfg->cam_hue = json_get_int(root, "cam_hue", cfg->cam_hue);
    cfg->cam_gamma = json_get_int(root, "cam_gamma", cfg->cam_gamma);
    cfg->cam_sharpness = json_get_int(root, "cam_sharpness", cfg->cam_sharpness);
    cfg->cam_gain = json_get_int(root, "cam_gain", cfg->cam_gain);
    cfg->cam_backlight = json_get_int(root, "cam_backlight", cfg->cam_backlight);
    cfg->cam_wb_auto = json_get_int(root, "cam_wb_auto", cfg->cam_wb_auto);
    cfg->cam_wb_temp = json_get_int(root, "cam_wb_temp", cfg->cam_wb_temp);
    cfg->cam_exposure_auto = json_get_int(root, "cam_exposure_auto", cfg->cam_exposure_auto);
    cfg->cam_exposure = json_get_int(root, "cam_exposure", cfg->cam_exposure);
    cfg->cam_exposure_priority = json_get_int(root, "cam_exposure_priority", cfg->cam_exposure_priority);
    cfg->cam_power_line = json_get_int(root, "cam_power_line", cfg->cam_power_line);

    /* Timelapse */
    cfg->timelapse_enabled = json_get_bool(root, "timelapse_enabled", cfg->timelapse_enabled);
    const char *tl_mode = json_get_str(root, "timelapse_mode", cfg->timelapse_mode);
    strncpy(cfg->timelapse_mode, tl_mode, sizeof(cfg->timelapse_mode) - 1);
    cfg->timelapse_hyperlapse_interval = clamp_int(
        json_get_int(root, "timelapse_hyperlapse_interval", cfg->timelapse_hyperlapse_interval), 5, 300);
    const char *tl_stor = json_get_str(root, "timelapse_storage", cfg->timelapse_storage);
    strncpy(cfg->timelapse_storage, tl_stor, sizeof(cfg->timelapse_storage) - 1);
    const char *tl_usb = json_get_str(root, "timelapse_usb_path", cfg->timelapse_usb_path);
    strncpy(cfg->timelapse_usb_path, tl_usb, sizeof(cfg->timelapse_usb_path) - 1);
    cfg->timelapse_output_fps = clamp_int(
        json_get_int(root, "timelapse_output_fps", cfg->timelapse_output_fps), 1, 120);
    cfg->timelapse_variable_fps = json_get_bool(root, "timelapse_variable_fps", cfg->timelapse_variable_fps);
    cfg->timelapse_target_length = clamp_int(
        json_get_int(root, "timelapse_target_length", cfg->timelapse_target_length), 1, 300);
    cfg->timelapse_variable_fps_min = clamp_int(
        json_get_int(root, "timelapse_variable_fps_min", cfg->timelapse_variable_fps_min), 1, 60);
    cfg->timelapse_variable_fps_max = clamp_int(
        json_get_int(root, "timelapse_variable_fps_max", cfg->timelapse_variable_fps_max), 1, 120);
    cfg->timelapse_crf = clamp_int(
        json_get_int(root, "timelapse_crf", cfg->timelapse_crf), 0, 51);
    cfg->timelapse_duplicate_last_frame = clamp_int(
        json_get_int(root, "timelapse_duplicate_last_frame", cfg->timelapse_duplicate_last_frame), 0, 60);
    cfg->timelapse_stream_delay = clamp_float(
        json_get_float(root, "timelapse_stream_delay", cfg->timelapse_stream_delay), 0.0f, 5.0f);
    cfg->timelapse_flip_x = json_get_bool(root, "timelapse_flip_x", cfg->timelapse_flip_x);
    cfg->timelapse_flip_y = json_get_bool(root, "timelapse_flip_y", cfg->timelapse_flip_y);
    cfg->timelapse_end_delay = clamp_float(
        json_get_float(root, "timelapse_end_delay", cfg->timelapse_end_delay), 0.0f, 30.0f);
    const char *mr_host = json_get_str(root, "moonraker_host", cfg->moonraker_host);
    strncpy(cfg->moonraker_host, mr_host, sizeof(cfg->moonraker_host) - 1);
    cfg->moonraker_port = clamp_int(
        json_get_int(root, "moonraker_port", cfg->moonraker_port), 1, 65535);
    const char *mr_cam_ip = json_get_str(root, "moonraker_camera_ip", cfg->moonraker_camera_ip);
    strncpy(cfg->moonraker_camera_ip, mr_cam_ip, sizeof(cfg->moonraker_camera_ip) - 1);

    /* Per-camera settings: preserve as raw JSON string */
    const cJSON *cameras = cJSON_GetObjectItemCaseSensitive(root, "cameras");
    if (cameras && cJSON_IsObject(cameras)) {
        char *cam_str = cJSON_PrintUnformatted(cameras);
        if (cam_str) {
            strncpy(cfg->cameras_json, cam_str, sizeof(cfg->cameras_json) - 1);
            free(cam_str);
        }
    }

    /* Fault Detection */
    cfg->fault_detect_enabled = json_get_bool(root, "fault_detect_enabled", cfg->fault_detect_enabled);
    cfg->fault_detect_cnn_enabled = json_get_bool(root, "fault_detect_cnn_enabled", cfg->fault_detect_cnn_enabled);
    cfg->fault_detect_proto_enabled = json_get_bool(root, "fault_detect_proto_enabled", cfg->fault_detect_proto_enabled);
    cfg->fault_detect_multi_enabled = json_get_bool(root, "fault_detect_multi_enabled", cfg->fault_detect_multi_enabled);
    const char *fd_strat = json_get_str(root, "fault_detect_strategy", cfg->fault_detect_strategy);
    strncpy(cfg->fault_detect_strategy, fd_strat, sizeof(cfg->fault_detect_strategy) - 1);
    cfg->fault_detect_interval = clamp_int(
        json_get_int(root, "fault_detect_interval", cfg->fault_detect_interval), 1, 60);
    cfg->fault_detect_verify_interval = clamp_int(
        json_get_int(root, "fault_detect_verify_interval", cfg->fault_detect_verify_interval), 1, 30);
    const char *fd_set = json_get_str(root, "fault_detect_model_set", cfg->fault_detect_model_set);
    strncpy(cfg->fault_detect_model_set, fd_set, sizeof(cfg->fault_detect_model_set) - 1);
    cfg->fault_detect_min_free_mem = clamp_int(
        json_get_int(root, "fault_detect_min_free_mem", cfg->fault_detect_min_free_mem), 5, 100);
    cfg->fault_detect_pace_ms = clamp_int(
        json_get_int(root, "fault_detect_pace_ms", cfg->fault_detect_pace_ms), 0, 500);
    cfg->heatmap_enabled = json_get_bool(root, "heatmap_enabled", cfg->heatmap_enabled);
    cfg->fd_debug_logging = json_get_bool(root, "fd_debug_logging", cfg->fd_debug_logging);
    cfg->fd_beep_pattern = clamp_int(
        json_get_int(root, "fd_beep_pattern", cfg->fd_beep_pattern), 0, 5);

    /* Per-set threshold settings */
    const cJSON *fd_th = cJSON_GetObjectItemCaseSensitive(root, "fd_thresholds");
    if (fd_th && cJSON_IsObject(fd_th)) {
        char *th_str = cJSON_PrintUnformatted(fd_th);
        if (th_str) {
            strncpy(cfg->fd_thresholds_json, th_str, sizeof(cfg->fd_thresholds_json) - 1);
            free(th_str);
        }
    }

    /* Prototype Management */
    {
        const char *pa = json_get_str(root, "proto_active_set", cfg->proto_active_set);
        strncpy(cfg->proto_active_set, pa, sizeof(cfg->proto_active_set) - 1);
        const char *pu = json_get_str(root, "proto_dataset_url", cfg->proto_dataset_url);
        strncpy(cfg->proto_dataset_url, pu, sizeof(cfg->proto_dataset_url) - 1);
    }

    /* Fault Detection Setup */
    cfg->fd_setup_status = clamp_int(
        json_get_int(root, "fd_setup_status", cfg->fd_setup_status), 0, 2);
    {
        const cJSON *ts_item = cJSON_GetObjectItemCaseSensitive(root, "fd_setup_timestamp");
        if (ts_item && cJSON_IsNumber(ts_item))
            cfg->fd_setup_timestamp = (int64_t)ts_item->valuedouble;
    }
    {
        const cJSON *corners = cJSON_GetObjectItemCaseSensitive(root, "fd_setup_corners");
        int ncorners = corners ? cJSON_GetArraySize(corners) : 0;
        /* Support old 4-point (8 floats) and new 8-point (16 floats) configs */
        if (corners && cJSON_IsArray(corners) && (ncorners == 8 || ncorners == 16)) {
            for (int i = 0; i < ncorners; i++) {
                const cJSON *v = cJSON_GetArrayItem(corners, i);
                if (v && cJSON_IsNumber(v))
                    cfg->fd_setup_corners[i] = clamp_float((float)v->valuedouble, 0.0f, 1.0f);
            }
        }
    }
    {
        /* Try new hex string first, then fall back to old numeric mask */
        const char *hex_mask = json_get_str(root, "fd_setup_mask_hex", NULL);
        if (hex_mask) {
            strncpy(cfg->fd_setup_mask_hex, hex_mask, sizeof(cfg->fd_setup_mask_hex) - 1);
        } else {
            const cJSON *mask_item = cJSON_GetObjectItemCaseSensitive(root, "fd_setup_mask");
            if (mask_item && cJSON_IsNumber(mask_item)) {
                /* Convert old uint64_t to hex format */
                uint64_t old_mask = (uint64_t)mask_item->valuedouble;
                snprintf(cfg->fd_setup_mask_hex, sizeof(cfg->fd_setup_mask_hex),
                         "0000000000000000:0000000000000000:0000000000000000:%016llx",
                         (unsigned long long)old_mask);
            }
        }

        /* Migrate old 4-word mask (14x14 grid) to 7-word (14x28 grid) */
        int colons = 0;
        for (const char *p = cfg->fd_setup_mask_hex; *p; p++)
            if (*p == ':') colons++;
        if (colons == 3) {
            fprintf(stderr, "Config: Migrating old 4-word mask to 7-word (14x28 grid)\n");
            if (cfg->fd_setup_status > FD_SETUP_NONE) {
                fprintf(stderr, "Config: Grid changed, resetting setup status to NONE\n");
                cfg->fd_setup_status = FD_SETUP_NONE;
            }
            /* Reset to full-grid default */
            strncpy(cfg->fd_setup_mask_hex,
                "00000000000000ff:ffffffffffffffff:ffffffffffffffff:ffffffffffffffff:ffffffffffffffff:ffffffffffffffff:ffffffffffffffff",
                sizeof(cfg->fd_setup_mask_hex) - 1);
        }
    }
    cfg->fd_bed_size_x = clamp_int(
        json_get_int(root, "fd_bed_size_x", cfg->fd_bed_size_x), 100, 500);
    cfg->fd_bed_size_y = clamp_int(
        json_get_int(root, "fd_bed_size_y", cfg->fd_bed_size_y), 100, 500);
    {
        const cJSON *res = cJSON_GetObjectItemCaseSensitive(root, "fd_setup_results");
        if (res && cJSON_IsObject(res)) {
            char *res_str = cJSON_PrintUnformatted(res);
            if (res_str) {
                strncpy(cfg->fd_setup_results_json, res_str, sizeof(cfg->fd_setup_results_json) - 1);
                free(res_str);
            }
        }
    }

    /* Z-dependent masks */
    {
        const cJSON *zm = cJSON_GetObjectItemCaseSensitive(root, "fd_z_masks");
        if (zm && cJSON_IsArray(zm)) {
            char *zm_str = cJSON_PrintUnformatted(zm);
            if (zm_str) {
                strncpy(cfg->fd_z_masks_json, zm_str, sizeof(cfg->fd_z_masks_json) - 1);
                free(zm_str);
            }
        }
    }

    cJSON_Delete(root);

    fprintf(stderr, "Config: Loaded from %s (encoder=%s, bitrate=%d, fps=%d)\n",
            path, cfg->encoder_type, cfg->bitrate, cfg->mjpeg_fps);
    return 0;
}

/* Helper: set string or number as string value in cJSON object */
static void json_set_str(cJSON *obj, const char *key, const char *val) {
    cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (item) {
        cJSON_SetValuestring(item, val);
    } else {
        cJSON_AddStringToObject(obj, key, val);
    }
}

static void json_set_int(cJSON *obj, const char *key, int val) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%d", val);
    json_set_str(obj, key, buf);
}

static void json_set_bool(cJSON *obj, const char *key, int val) {
    json_set_str(obj, key, val ? "true" : "false");
}

static void json_set_float(cJSON *obj, const char *key, float val) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%.2f", val);
    json_set_str(obj, key, buf);
}

int config_save(const AppConfig *cfg, const char *path) {
    /* Read existing config to preserve unknown keys */
    cJSON *root = NULL;

    FILE *f = fopen(path, "r");
    if (f) {
        fseek(f, 0, SEEK_END);
        long fsize = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (fsize > 0 && fsize < 64 * 1024) {
            char *buf = malloc(fsize + 1);
            if (buf) {
                size_t nread = fread(buf, 1, fsize, f);
                buf[nread] = '\0';
                root = cJSON_Parse(buf);
                free(buf);
            }
        }
        fclose(f);
    }

    if (!root) {
        root = cJSON_CreateObject();
    }

    /* Encoder */
    json_set_str(root, "mode", cfg->mode);
    json_set_str(root, "encoder_type", cfg->encoder_type);
    json_set_bool(root, "h264_enabled", cfg->h264_enabled);
    json_set_bool(root, "auto_skip", cfg->auto_skip);
    json_set_int(root, "skip_ratio", cfg->skip_ratio);
    json_set_int(root, "target_cpu", cfg->target_cpu);
    json_set_int(root, "bitrate", cfg->bitrate);
    json_set_int(root, "mjpeg_fps", cfg->mjpeg_fps);
    json_set_int(root, "streaming_port", cfg->streaming_port);
    json_set_int(root, "control_port", cfg->control_port);
    json_set_str(root, "h264_resolution", cfg->h264_resolution);

    /* Display */
    json_set_bool(root, "display_enabled", cfg->display_enabled);
    json_set_int(root, "display_fps", cfg->display_fps);

    /* Modes */
    json_set_bool(root, "autolanmode", cfg->autolanmode);
    json_set_bool(root, "logging", cfg->logging);
    json_set_int(root, "log_max_size", cfg->log_max_size);
    json_set_bool(root, "acproxycam_flv_proxy", cfg->acproxycam_flv_proxy);

    /* Internal USB port */
    json_set_str(root, "internal_usb_port", cfg->internal_usb_port);

    /* Camera controls */
    json_set_int(root, "cam_brightness", cfg->cam_brightness);
    json_set_int(root, "cam_contrast", cfg->cam_contrast);
    json_set_int(root, "cam_saturation", cfg->cam_saturation);
    json_set_int(root, "cam_hue", cfg->cam_hue);
    json_set_int(root, "cam_gamma", cfg->cam_gamma);
    json_set_int(root, "cam_sharpness", cfg->cam_sharpness);
    json_set_int(root, "cam_gain", cfg->cam_gain);
    json_set_int(root, "cam_backlight", cfg->cam_backlight);
    json_set_int(root, "cam_wb_auto", cfg->cam_wb_auto);
    json_set_int(root, "cam_wb_temp", cfg->cam_wb_temp);
    json_set_int(root, "cam_exposure_auto", cfg->cam_exposure_auto);
    json_set_int(root, "cam_exposure", cfg->cam_exposure);
    json_set_int(root, "cam_exposure_priority", cfg->cam_exposure_priority);
    json_set_int(root, "cam_power_line", cfg->cam_power_line);

    /* Timelapse */
    json_set_bool(root, "timelapse_enabled", cfg->timelapse_enabled);
    json_set_str(root, "timelapse_mode", cfg->timelapse_mode);
    json_set_int(root, "timelapse_hyperlapse_interval", cfg->timelapse_hyperlapse_interval);
    json_set_str(root, "timelapse_storage", cfg->timelapse_storage);
    json_set_str(root, "timelapse_usb_path", cfg->timelapse_usb_path);
    json_set_str(root, "moonraker_host", cfg->moonraker_host);
    json_set_int(root, "moonraker_port", cfg->moonraker_port);
    json_set_str(root, "moonraker_camera_ip", cfg->moonraker_camera_ip);
    json_set_int(root, "timelapse_output_fps", cfg->timelapse_output_fps);
    json_set_bool(root, "timelapse_variable_fps", cfg->timelapse_variable_fps);
    json_set_int(root, "timelapse_target_length", cfg->timelapse_target_length);
    json_set_int(root, "timelapse_variable_fps_min", cfg->timelapse_variable_fps_min);
    json_set_int(root, "timelapse_variable_fps_max", cfg->timelapse_variable_fps_max);
    json_set_int(root, "timelapse_crf", cfg->timelapse_crf);
    json_set_int(root, "timelapse_duplicate_last_frame", cfg->timelapse_duplicate_last_frame);
    json_set_float(root, "timelapse_stream_delay", cfg->timelapse_stream_delay);
    json_set_bool(root, "timelapse_flip_x", cfg->timelapse_flip_x);
    json_set_bool(root, "timelapse_flip_y", cfg->timelapse_flip_y);
    json_set_float(root, "timelapse_end_delay", cfg->timelapse_end_delay);

    /* Fault Detection */
    json_set_bool(root, "fault_detect_enabled", cfg->fault_detect_enabled);
    json_set_bool(root, "fault_detect_cnn_enabled", cfg->fault_detect_cnn_enabled);
    json_set_bool(root, "fault_detect_proto_enabled", cfg->fault_detect_proto_enabled);
    json_set_bool(root, "fault_detect_multi_enabled", cfg->fault_detect_multi_enabled);
    json_set_str(root, "fault_detect_strategy", cfg->fault_detect_strategy);
    json_set_int(root, "fault_detect_interval", cfg->fault_detect_interval);
    json_set_int(root, "fault_detect_verify_interval", cfg->fault_detect_verify_interval);
    json_set_str(root, "fault_detect_model_set", cfg->fault_detect_model_set);
    json_set_int(root, "fault_detect_min_free_mem", cfg->fault_detect_min_free_mem);
    json_set_int(root, "fault_detect_pace_ms", cfg->fault_detect_pace_ms);
    json_set_bool(root, "heatmap_enabled", cfg->heatmap_enabled);
    json_set_bool(root, "fd_debug_logging", cfg->fd_debug_logging);
    json_set_int(root, "fd_beep_pattern", cfg->fd_beep_pattern);

    /* Per-set threshold settings */
    if (cfg->fd_thresholds_json[0]) {
        cJSON *fd_th = cJSON_Parse(cfg->fd_thresholds_json);
        if (fd_th) {
            cJSON_DeleteItemFromObjectCaseSensitive(root, "fd_thresholds");
            cJSON_AddItemToObject(root, "fd_thresholds", fd_th);
        }
    }

    /* Prototype Management */
    json_set_str(root, "proto_active_set", cfg->proto_active_set);
    if (cfg->proto_dataset_url[0])
        json_set_str(root, "proto_dataset_url", cfg->proto_dataset_url);

    /* Fault Detection Setup */
    json_set_int(root, "fd_setup_status", cfg->fd_setup_status);
    {
        /* Store timestamp as JSON number (safe up to 2^53) */
        cJSON *ts = cJSON_GetObjectItemCaseSensitive(root, "fd_setup_timestamp");
        if (ts)
            cJSON_SetNumberValue(ts, (double)cfg->fd_setup_timestamp);
        else
            cJSON_AddNumberToObject(root, "fd_setup_timestamp", (double)cfg->fd_setup_timestamp);
    }
    {
        cJSON_DeleteItemFromObjectCaseSensitive(root, "fd_setup_corners");
        cJSON *arr = cJSON_CreateArray();
        for (int i = 0; i < 16; i++)
            cJSON_AddItemToArray(arr, cJSON_CreateNumber(cfg->fd_setup_corners[i]));
        cJSON_AddItemToObject(root, "fd_setup_corners", arr);
    }
    {
        /* Write hex mask string, remove old numeric key if present */
        cJSON_DeleteItemFromObjectCaseSensitive(root, "fd_setup_mask");
        json_set_str(root, "fd_setup_mask_hex", cfg->fd_setup_mask_hex);
    }
    json_set_int(root, "fd_bed_size_x", cfg->fd_bed_size_x);
    json_set_int(root, "fd_bed_size_y", cfg->fd_bed_size_y);
    if (cfg->fd_setup_results_json[0]) {
        cJSON *res = cJSON_Parse(cfg->fd_setup_results_json);
        if (res) {
            cJSON_DeleteItemFromObjectCaseSensitive(root, "fd_setup_results");
            cJSON_AddItemToObject(root, "fd_setup_results", res);
        }
    }

    /* Z-dependent masks */
    if (cfg->fd_z_masks_json[0]) {
        cJSON *zm = cJSON_Parse(cfg->fd_z_masks_json);
        if (zm) {
            cJSON_DeleteItemFromObjectCaseSensitive(root, "fd_z_masks");
            cJSON_AddItemToObject(root, "fd_z_masks", zm);
        }
    } else {
        cJSON_DeleteItemFromObjectCaseSensitive(root, "fd_z_masks");
    }

    /* Per-camera settings */
    if (cfg->cameras_json[0]) {
        cJSON *cameras = cJSON_Parse(cfg->cameras_json);
        if (cameras) {
            cJSON_DeleteItemFromObjectCaseSensitive(root, "cameras");
            cJSON_AddItemToObject(root, "cameras", cameras);
        }
    }

    /* Write to file */
    char *json_str = cJSON_Print(root);
    cJSON_Delete(root);

    if (!json_str) {
        fprintf(stderr, "Config: JSON serialization failed\n");
        return -1;
    }

    f = fopen(path, "w");
    if (!f) {
        fprintf(stderr, "Config: Cannot write %s: %s\n", path, strerror(errno));
        free(json_str);
        return -1;
    }

    size_t len = strlen(json_str);
    size_t written = fwrite(json_str, 1, len, f);
    fflush(f);
    fsync(fileno(f));
    fclose(f);
    free(json_str);

    /* System-wide sync */
    sync();

    if (written != len) {
        fprintf(stderr, "Config: Short write to %s\n", path);
        return -1;
    }

    fprintf(stderr, "Config: Saved to %s\n", path);
    return 0;
}
