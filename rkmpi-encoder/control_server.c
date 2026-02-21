/*
 * Control HTTP Server Implementation
 *
 * HTTP server on the control port providing web UI and REST API.
 * Provides web UI and REST API for settings management.
 */

#define _GNU_SOURCE
#include "control_server.h"
#include "moonraker_client.h"
#include "mqtt_client.h"
#include "config.h"
#include "cpu_monitor.h"
#include "http_server.h"
#include "lan_mode.h"
#include "touch_inject.h"
#include "fault_detect.h"
#include "cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <time.h>
#include <signal.h>

/* Global instance */
ControlServer g_control_server;

/* Moonraker client reference (set by main, read by API) */
static MoonrakerClient *g_moonraker_client = NULL;

/* External globals from rkmpi_enc.c */
extern volatile sig_atomic_t g_running;
extern int g_verbose;
extern const char g_encoder_version[];

/* Forward declarations */
static void *control_server_thread(void *arg);
static void handle_client(ControlServer *srv, int client_fd, struct sockaddr_in *addr);

/* ============================================================================
 * Utility Functions
 * ============================================================================ */

/* Safe string copy: copies src into dst of size dst_size, always null-terminates.
 * Uses strlen+memcpy to avoid both strncpy and snprintf truncation warnings. */
static inline void safe_strcpy(char *dst, size_t dst_size, const char *src) {
    if (dst_size == 0) return;
    size_t len = strlen(src);
    if (len >= dst_size) len = dst_size - 1;
    memcpy(dst, src, len);
    dst[len] = '\0';
}

/* Send data to socket (handles partial writes) */
static int ctrl_send(int fd, const void *data, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(fd, (const char *)data + sent, len - sent, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                usleep(1000);
                continue;
            }
            return -1;
        }
        sent += n;
    }
    return 0;
}

/* Send HTTP response with headers */
static void send_http_response(int fd, int status_code, const char *content_type,
                                const char *body, size_t body_len,
                                const char *extra_headers) {
    const char *status_text = "OK";
    if (status_code == 404) status_text = "Not Found";
    else if (status_code == 400) status_text = "Bad Request";
    else if (status_code == 500) status_text = "Internal Server Error";
    else if (status_code == 302) status_text = "Found";

    char headers[1024];
    int hlen = snprintf(headers, sizeof(headers),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "%s"
        "\r\n",
        status_code, status_text, content_type, body_len,
        extra_headers ? extra_headers : "");

    ctrl_send(fd, headers, hlen);
    if (body && body_len > 0) {
        ctrl_send(fd, body, body_len);
    }
}

/* Send JSON response */
static void send_json_response(int fd, int status_code, cJSON *json) {
    char *body = cJSON_PrintUnformatted(json);
    if (!body) {
        send_http_response(fd, 500, "text/plain", "JSON error", 10, NULL);
        return;
    }
    send_http_response(fd, status_code, "application/json", body, strlen(body),
                       "Access-Control-Allow-Origin: *\r\n");
    free(body);
}

/* Send 404 response */
static void send_404(int fd) {
    send_http_response(fd, 404, "text/plain", "Not Found", 9, NULL);
}

static void send_json_error(int fd, int status_code, const char *message) {
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "error", message);
    send_json_response(fd, status_code, root);
    cJSON_Delete(root);
}

/* Send redirect */
static void send_redirect(int fd, const char *url) {
    char headers[512];
    snprintf(headers, sizeof(headers), "Location: %s\r\n", url);
    send_http_response(fd, 302, "text/plain", "Redirecting", 11, headers);
}

/* URL decode (in-place) */
static void url_decode(char *str) {
    char *src = str, *dst = str;
    while (*src) {
        if (*src == '%' && src[1] && src[2]) {
            char hex[3] = { src[1], src[2], 0 };
            *dst = (char)strtol(hex, NULL, 16);
            src += 3;
            dst++;
        } else if (*src == '+') {
            *dst++ = ' ';
            src++;
        } else {
            *dst++ = *src++;
        }
    }
    *dst = '\0';
}

/* Parse form-encoded POST body into key-value pairs */
typedef struct {
    char key[64];
    char value[512];
} FormParam;

static int parse_form_body(const char *body, FormParam *params, int max_params) {
    if (!body || !*body) return 0;

    int count = 0;
    char buf[CTRL_MAX_POST_BODY];
    strncpy(buf, body, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    char *saveptr;
    char *pair = strtok_r(buf, "&", &saveptr);
    while (pair && count < max_params) {
        char *eq = strchr(pair, '=');
        if (eq) {
            *eq = '\0';
            strncpy(params[count].key, pair, sizeof(params[count].key) - 1);
            strncpy(params[count].value, eq + 1, sizeof(params[count].value) - 1);
            url_decode(params[count].key);
            url_decode(params[count].value);
            count++;
        }
        pair = strtok_r(NULL, "&", &saveptr);
    }
    return count;
}

/* Find value in form params */
static const char *form_get(const FormParam *params, int count, const char *key) {
    for (int i = 0; i < count; i++) {
        if (strcmp(params[i].key, key) == 0) {
            return params[i].value;
        }
    }
    return NULL;
}

/* Check if form param exists (for checkboxes) */
static int form_has(const FormParam *params, int count, const char *key) {
    return form_get(params, count, key) != NULL;
}

/* Get integer from form params with default */
static int form_get_int(const FormParam *params, int count, const char *key, int def) {
    const char *val = form_get(params, count, key);
    return val ? atoi(val) : def;
}

/* Read a template file and return malloc'd content (caller must free) */
static char *load_template(const char *dir, const char *filename) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", dir, filename);

    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "Control: Cannot open template %s: %s\n", path, strerror(errno));
        return NULL;
    }

    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (fsize <= 0 || fsize > CTRL_MAX_TEMPLATE) {
        fclose(f);
        return NULL;
    }

    char *content = malloc(fsize + 1);
    if (!content) {
        fclose(f);
        return NULL;
    }

    size_t nread = fread(content, 1, fsize, f);
    fclose(f);
    content[nread] = '\0';
    return content;
}

/* Template substitution: replace $variable_name with values.
 * Returns malloc'd result (caller must free). */
typedef struct {
    const char *name;   /* variable name (without $) */
    const char *value;  /* replacement value */
} TemplateVar;

static char *template_substitute(const char *tmpl, const TemplateVar *vars, int nvars) {
    /* Calculate worst-case output size */
    size_t tmpl_len = strlen(tmpl);
    size_t max_expansion = 0;
    for (int i = 0; i < nvars; i++) {
        max_expansion += strlen(vars[i].value) * 100; /* crude upper bound */
    }

    size_t out_size = tmpl_len + max_expansion + 1;
    if (out_size > CTRL_MAX_TEMPLATE) out_size = CTRL_MAX_TEMPLATE;

    char *out = malloc(out_size);
    if (!out) return NULL;

    size_t out_pos = 0;
    const char *p = tmpl;

    while (*p && out_pos < out_size - 1) {
        if (*p == '$') {
            /* Try to match a variable name */
            int matched = 0;
            for (int i = 0; i < nvars; i++) {
                size_t nlen = strlen(vars[i].name);
                if (strncmp(p + 1, vars[i].name, nlen) == 0) {
                    /* Check that next char is not alphanumeric/underscore */
                    char next = p[1 + nlen];
                    if (next == '\0' || (!((next >= 'a' && next <= 'z') ||
                        (next >= 'A' && next <= 'Z') ||
                        (next >= '0' && next <= '9') || next == '_'))) {
                        /* Found match - copy value */
                        size_t vlen = strlen(vars[i].value);
                        if (out_pos + vlen < out_size) {
                            memcpy(out + out_pos, vars[i].value, vlen);
                            out_pos += vlen;
                        }
                        p += 1 + nlen;
                        matched = 1;
                        break;
                    }
                }
            }
            if (!matched) {
                out[out_pos++] = *p++;
            }
        } else {
            out[out_pos++] = *p++;
        }
    }

    out[out_pos] = '\0';
    return out;
}

/* Read encoder stats from control file (/tmp/h264_ctrl) */
static void read_encoder_stats(ControlServer *srv) {
    FILE *f = fopen("/tmp/h264_ctrl", "r");
    if (!f) return;

    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = line;
        char *val = eq + 1;
        /* Trim newline */
        char *nl = strchr(val, '\n');
        if (nl) *nl = '\0';

        if (strcmp(key, "mjpeg_fps") == 0) srv->encoder_mjpeg_fps = atof(val);
        else if (strcmp(key, "h264_fps") == 0) srv->encoder_h264_fps = atof(val);
        else if (strcmp(key, "mjpeg_clients") == 0) srv->encoder_mjpeg_clients = atoi(val);
        else if (strcmp(key, "flv_clients") == 0) srv->encoder_flv_clients = atoi(val);
        else if (strcmp(key, "display_clients") == 0) srv->encoder_display_clients = atoi(val);
        else if (strcmp(key, "camera_max_fps") == 0) srv->max_camera_fps = atoi(val);
    }
    fclose(f);
}

/* Get IP address for a specific interface */
static int get_iface_ip(const char *iface, char *buf, size_t bufsize) {
    char cmd[128];
    snprintf(cmd, sizeof(cmd),
             "ifconfig %s 2>/dev/null | grep -o 'inet addr:[0-9.]*\\|inet [0-9.]*' | head -1",
             iface);
    FILE *f = popen(cmd, "r");
    if (!f) return -1;

    char line[128];
    int found = 0;
    if (fgets(line, sizeof(line), f)) {
        char *ip_start = strstr(line, "addr:");
        if (ip_start) {
            ip_start += 5;
        } else {
            ip_start = strstr(line, "inet ");
            if (ip_start) ip_start += 5;
        }
        if (ip_start) {
            /* Trim trailing whitespace/newline */
            char *end = ip_start;
            while (*end && *end != ' ' && *end != '\n' && *end != '/') end++;
            size_t len = end - ip_start;
            if (len > 0 && len < bufsize && strncmp(ip_start, "127.", 4) != 0) {
                memcpy(buf, ip_start, len);
                buf[len] = '\0';
                found = 1;
            }
        }
    }
    pclose(f);
    return found ? 0 : -1;
}

/* Get IP address (prefer eth1, then wlan0) */
static int get_ip_address(char *buf, size_t bufsize) {
    FILE *f = popen("ifconfig 2>/dev/null", "r");
    if (!f) return -1;

    char line[256];
    const char *ifaces[] = { "eth1", "wlan0", NULL };
    int found = 0;

    /* Simple parsing - look for interface blocks with inet addr */
    char current_iface[32] = {0};
    while (fgets(line, sizeof(line), f)) {
        /* Check if this is an interface header line */
        if (line[0] != ' ' && line[0] != '\t') {
            /* Extract interface name */
            char *sp = strchr(line, ' ');
            if (sp) {
                size_t len = sp - line;
                if (len < sizeof(current_iface)) {
                    memcpy(current_iface, line, len);
                    current_iface[len] = '\0';
                }
            }
        }

        /* Check for inet addr in current block */
        char *addr_str = strstr(line, "inet addr:");
        if (!addr_str) addr_str = strstr(line, "inet ");
        if (addr_str) {
            /* Check if this is a preferred interface */
            for (int i = 0; ifaces[i]; i++) {
                if (strstr(current_iface, ifaces[i])) {
                    /* Extract IP */
                    char *ip_start = addr_str + (strncmp(addr_str, "inet addr:", 10) == 0 ? 10 : 5);
                    char *ip_end = ip_start;
                    while (*ip_end && *ip_end != ' ' && *ip_end != '\n' && *ip_end != '/') ip_end++;
                    size_t ip_len = ip_end - ip_start;
                    if (ip_len > 0 && ip_len < bufsize) {
                        memcpy(buf, ip_start, ip_len);
                        buf[ip_len] = '\0';
                        if (strcmp(buf, "127.0.0.1") != 0) {
                            found = 1;
                            break;
                        }
                    }
                }
            }
            if (found) break;
        }
    }
    pclose(f);
    return found ? 0 : -1;
}

/* ============================================================================
 * Route Handlers
 * ============================================================================ */

/* GET / - Homepage */
static void serve_homepage(ControlServer *srv, int fd) {
    char *tmpl = load_template(srv->template_dir, "index.html");
    if (!tmpl) {
        /* Fallback: simple redirect to /control */
        send_redirect(fd, "/control");
        return;
    }

    char sp[8], cp[8];
    snprintf(sp, sizeof(sp), "%d", srv->config->streaming_port);
    snprintf(cp, sizeof(cp), "%d", srv->config->control_port);

    TemplateVar vars[] = {
        { "streaming_port", sp },
        { "control_port", cp },
    };

    char *html = template_substitute(tmpl, vars, 2);
    free(tmpl);

    if (html) {
        send_http_response(fd, 200, "text/html; charset=utf-8", html, strlen(html),
                          "Cache-Control: no-cache\r\n");
        free(html);
    } else {
        send_404(fd);
    }
}

/* GET /control - Settings page */
static void serve_control_page(ControlServer *srv, int fd) {
    char *tmpl = load_template(srv->template_dir, "control.html");
    if (!tmpl) {
        send_http_response(fd, 500, "text/plain",
                          "Template not found", 18, NULL);
        return;
    }

    AppConfig *cfg = srv->config;

    /* Build template variables */
    /* We use a large array of TemplateVar - each variable corresponds to
     * a $variable_name in the template */
    char sp_str[12], cp_str[12], br_str[12], fps_str[12], sr_str[12];
    char tc_str[12], jq_str[12], dfps_str[12], mcfps_str[12];

    snprintf(sp_str, sizeof(sp_str), "%d", cfg->streaming_port);
    snprintf(cp_str, sizeof(cp_str), "%d", cfg->control_port);
    snprintf(br_str, sizeof(br_str), "%d", cfg->bitrate);
    snprintf(fps_str, sizeof(fps_str), "%d", cfg->mjpeg_fps);
    snprintf(sr_str, sizeof(sr_str), "%d", cfg->skip_ratio);
    snprintf(tc_str, sizeof(tc_str), "%d", cfg->target_cpu);
    snprintf(jq_str, sizeof(jq_str), "%d", cfg->jpeg_quality);
    snprintf(dfps_str, sizeof(dfps_str), "%d", cfg->display_fps);
    /* Use V4L2-reported max FPS (hardware capability), not runtime-measured rate */
    int hw_max_fps = 30;
    if (srv->cameras && srv->num_cameras > 0 && srv->cameras[0].max_fps > 0)
        hw_max_fps = srv->cameras[0].max_fps;
    snprintf(mcfps_str, sizeof(mcfps_str), "%d", hw_max_fps);

    /* Timelapse strings */
    char tl_hi_str[12], tl_ofps_str[12], tl_tl_str[12];
    char tl_vfmin_str[12], tl_vfmax_str[12], tl_crf_str[12];
    char tl_dlf_str[12], tl_sd_str[12], tl_ed_str[12];
    char mr_port_str[12];

    snprintf(tl_hi_str, sizeof(tl_hi_str), "%d", cfg->timelapse_hyperlapse_interval);
    snprintf(tl_ofps_str, sizeof(tl_ofps_str), "%d", cfg->timelapse_output_fps);
    snprintf(tl_tl_str, sizeof(tl_tl_str), "%d", cfg->timelapse_target_length);
    snprintf(tl_vfmin_str, sizeof(tl_vfmin_str), "%d", cfg->timelapse_variable_fps_min);
    snprintf(tl_vfmax_str, sizeof(tl_vfmax_str), "%d", cfg->timelapse_variable_fps_max);
    snprintf(tl_crf_str, sizeof(tl_crf_str), "%d", cfg->timelapse_crf);
    snprintf(tl_dlf_str, sizeof(tl_dlf_str), "%d", cfg->timelapse_duplicate_last_frame);
    snprintf(tl_sd_str, sizeof(tl_sd_str), "%.2f", cfg->timelapse_stream_delay);
    snprintf(tl_ed_str, sizeof(tl_ed_str), "%.1f", cfg->timelapse_end_delay);
    snprintf(mr_port_str, sizeof(mr_port_str), "%d", cfg->moonraker_port);

    /* Boolean checked attributes */
    const char *checked = "checked";
    const char *empty = "";

    /* Encoder type selected attributes */
    const char *enc_rkmpi_sel = strcmp(cfg->encoder_type, "rkmpi") == 0 ? "selected" : "";
    const char *enc_rkmpi_yuyv_sel = strcmp(cfg->encoder_type, "rkmpi-yuyv") == 0 ? "selected" : "";

    /* H264 resolution selected */
    const char *res_1280_sel = strcmp(cfg->h264_resolution, "1280x720") == 0 ? "selected" : "";
    const char *res_960_sel = strcmp(cfg->h264_resolution, "960x540") == 0 ? "selected" : "";
    const char *res_640_sel = strcmp(cfg->h264_resolution, "640x360") == 0 ? "selected" : "";

    /* Display FPS selected */
    char dfps_1_sel[16] = "", dfps_2_sel[16] = "", dfps_3_sel[16] = "";
    char dfps_5_sel[16] = "", dfps_10_sel[16] = "";
    if (cfg->display_fps == 1) strcpy(dfps_1_sel, "selected");
    else if (cfg->display_fps == 2) strcpy(dfps_2_sel, "selected");
    else if (cfg->display_fps == 3) strcpy(dfps_3_sel, "selected");
    else if (cfg->display_fps == 5) strcpy(dfps_5_sel, "selected");
    else if (cfg->display_fps >= 10) strcpy(dfps_10_sel, "selected");

    /* Timelapse mode selected */
    const char *tl_layer_sel = strcmp(cfg->timelapse_mode, "layer") == 0 ? "selected" : "";
    const char *tl_hyper_sel = strcmp(cfg->timelapse_mode, "hyperlapse") == 0 ? "selected" : "";

    /* Timelapse storage selected */
    const char *tl_internal_sel = strcmp(cfg->timelapse_storage, "internal") == 0 ? "selected" : "";
    const char *tl_usb_sel = strcmp(cfg->timelapse_storage, "usb") == 0 ? "selected" : "";

    /* FPS percentage for skip ratio slider */
    int fps_pct = cfg->skip_ratio <= 1 ? 100 : (100 / cfg->skip_ratio);
    if (fps_pct < 1) fps_pct = 1;
    char fps_pct_str[8];
    snprintf(fps_pct_str, sizeof(fps_pct_str), "%d", fps_pct);

    TemplateVar vars[] = {
        { "streaming_port", sp_str },
        { "control_port", cp_str },
        { "encoder_rkmpi_selected", enc_rkmpi_sel },
        { "encoder_rkmpi_yuyv_selected", enc_rkmpi_yuyv_sel },
        { "autolanmode_checked", cfg->autolanmode ? checked : empty },
        { "logging_checked", cfg->logging ? checked : empty },
        { "h264_enabled_checked", cfg->h264_enabled ? checked : empty },
        { "auto_skip_checked", cfg->auto_skip ? checked : empty },
        { "bitrate", br_str },
        { "mjpeg_fps", fps_str },
        { "skip_ratio", sr_str },
        { "target_cpu", tc_str },
        { "jpeg_quality", jq_str },
        { "h264_resolution", cfg->h264_resolution },
        { "res_1280_selected", res_1280_sel },
        { "res_960_selected", res_960_sel },
        { "res_640_selected", res_640_sel },
        { "display_enabled_checked", cfg->display_enabled ? checked : empty },
        { "display_fps", dfps_str },
        { "dfps_1_selected", dfps_1_sel },
        { "dfps_2_selected", dfps_2_sel },
        { "dfps_3_selected", dfps_3_sel },
        { "dfps_5_selected", dfps_5_sel },
        { "dfps_10_selected", dfps_10_sel },
        { "acproxycam_flv_proxy_checked", cfg->acproxycam_flv_proxy ? checked : empty },
        { "max_camera_fps", mcfps_str },
        { "fps_pct", fps_pct_str },
        { "encoder_type", cfg->encoder_type },
        { "session_id", srv->session_id },
        { "encoder_version", g_encoder_version },
        { "streamer_version", srv->streamer_version },
        /* Timelapse settings */
        { "timelapse_enabled_checked", cfg->timelapse_enabled ? checked : empty },
        { "timelapse_mode_layer_selected", tl_layer_sel },
        { "timelapse_mode_hyperlapse_selected", tl_hyper_sel },
        { "timelapse_hyperlapse_interval", tl_hi_str },
        { "timelapse_storage_internal_selected", tl_internal_sel },
        { "timelapse_storage_usb_selected", tl_usb_sel },
        { "timelapse_usb_path", cfg->timelapse_usb_path },
        { "moonraker_host", cfg->moonraker_host },
        { "moonraker_port", mr_port_str },
        { "moonraker_camera_ip", cfg->moonraker_camera_ip },
        { "timelapse_output_fps", tl_ofps_str },
        { "timelapse_variable_fps_checked", cfg->timelapse_variable_fps ? checked : empty },
        { "timelapse_target_length", tl_tl_str },
        { "timelapse_variable_fps_min", tl_vfmin_str },
        { "timelapse_variable_fps_max", tl_vfmax_str },
        { "timelapse_crf", tl_crf_str },
        { "timelapse_duplicate_last_frame", tl_dlf_str },
        { "timelapse_stream_delay", tl_sd_str },
        { "timelapse_flip_x_checked", cfg->timelapse_flip_x ? checked : empty },
        { "timelapse_flip_y_checked", cfg->timelapse_flip_y ? checked : empty },
        { "timelapse_end_delay", tl_ed_str },
        /* Fault detection */
        { "fd_npu_available", fault_detect_npu_available() ? "true" : "false" },
        { "fd_enabled_checked", cfg->fault_detect_enabled ? checked : empty },
        { "fd_cnn_enabled_checked", cfg->fault_detect_cnn_enabled ? checked : empty },
        { "fd_proto_enabled_checked", cfg->fault_detect_proto_enabled ? checked : empty },
        { "fd_multi_enabled_checked", cfg->fault_detect_multi_enabled ? checked : empty },
        { "fd_strategy", cfg->fault_detect_strategy },
    };
    int nvars = sizeof(vars) / sizeof(vars[0]);

    char *html = template_substitute(tmpl, vars, nvars);
    free(tmpl);

    if (html) {
        send_http_response(fd, 200, "text/html; charset=utf-8", html, strlen(html),
                          "Cache-Control: no-cache\r\n");
        free(html);
    } else {
        send_http_response(fd, 500, "text/plain", "Template error", 14, NULL);
    }
}

/* POST /control - Apply settings */
static void handle_control_post(ControlServer *srv, int fd,
                                 const char *body) {
    FormParam params[CTRL_MAX_FORM_PARAMS];
    int nparams = parse_form_body(body, params, CTRL_MAX_FORM_PARAMS);

    AppConfig *cfg = srv->config;

    /* Encoder type */
    const char *enc = form_get(params, nparams, "encoder_type");
    if (enc && (strcmp(enc, "rkmpi") == 0 || strcmp(enc, "rkmpi-yuyv") == 0)) {
        safe_strcpy(cfg->encoder_type, sizeof(cfg->encoder_type), enc);
    }

    /* Boolean settings (checkboxes: present=1, absent=0) */
    cfg->autolanmode = form_has(params, nparams, "autolanmode") &&
                       strcmp(form_get(params, nparams, "autolanmode"), "1") == 0;
    cfg->logging = form_has(params, nparams, "logging") &&
                   strcmp(form_get(params, nparams, "logging"), "1") == 0;

    if (form_has(params, nparams, "h264_enabled")) {
        cfg->h264_enabled = strcmp(form_get(params, nparams, "h264_enabled"), "1") == 0;
    }

    /* Auto-skip */
    int new_auto_skip = form_has(params, nparams, "auto_skip") &&
                        strcmp(form_get(params, nparams, "auto_skip"), "1") == 0;
    cfg->auto_skip = new_auto_skip;

    /* Skip ratio */
    const char *sr_val = form_get(params, nparams, "skip_ratio");
    if (sr_val) {
        int new_skip = atoi(sr_val);
        if (new_skip >= 1) cfg->skip_ratio = new_skip;
    }

    /* Target CPU */
    const char *tc_val = form_get(params, nparams, "target_cpu");
    if (tc_val) {
        int v = atoi(tc_val);
        if (v >= 25 && v <= 90) cfg->target_cpu = v;
    }

    /* Bitrate */
    const char *br_val = form_get(params, nparams, "bitrate");
    if (br_val) {
        int v = atoi(br_val);
        if (v >= 100 && v <= 4000) cfg->bitrate = v;
    }

    /* MJPEG FPS */
    const char *fps_val = form_get(params, nparams, "mjpeg_fps");
    if (fps_val) {
        int v = atoi(fps_val);
        if (v >= 2 && v <= 30) cfg->mjpeg_fps = v;
    }

    /* H264 resolution */
    const char *res_val = form_get(params, nparams, "h264_resolution");
    if (res_val && strlen(res_val) < sizeof(cfg->h264_resolution)) {
        strncpy(cfg->h264_resolution, res_val, sizeof(cfg->h264_resolution) - 1);
    }

    /* Display */
    cfg->display_enabled = form_has(params, nparams, "display_enabled");
    const char *dfps_val = form_get(params, nparams, "display_fps");
    if (dfps_val) {
        int v = atoi(dfps_val);
        if (v >= 1 && v <= 10) cfg->display_fps = v;
    }

    /* ACProxyCam FLV proxy */
    cfg->acproxycam_flv_proxy = form_has(params, nparams, "acproxycam_flv_proxy");

    /* Save config */
    config_save(cfg, CONFIG_DEFAULT_PATH);

    /* Notify encoder of changes */
    if (srv->on_config_changed) {
        srv->on_config_changed(cfg);
    }

    /* Redirect back to control page */
    send_redirect(fd, "/control");
}

/* GET /api/stats - JSON stats */
static void serve_api_stats(ControlServer *srv, int fd) {
    /* Update CPU and encoder stats */
    cpu_monitor_update(&srv->cpu_monitor);
    read_encoder_stats(srv);

    cJSON *root = cJSON_CreateObject();

    /* CPU stats */
    cJSON *cpu = cJSON_CreateObject();
    float total_cpu = cpu_monitor_get_total(&srv->cpu_monitor);
    cJSON_AddNumberToObject(cpu, "total", (int)(total_cpu + 0.5f));

    /* Encoder CPU: this process (primary rkmpi_enc) */
    float enc_cpu = cpu_monitor_get_process(&srv->cpu_monitor, getpid());
    if (enc_cpu < 0) enc_cpu = 0;
    int enc_cpu_i = (int)(enc_cpu + 0.5f);
    cJSON_AddNumberToObject(cpu, "encoder_cpu", enc_cpu_i);

    /* Secondary encoders CPU: sum of all managed child processes */
    float sec_cpu = 0;
    for (int i = 0; i < srv->num_managed; i++) {
        pid_t pid = srv->managed_procs[i].pid;
        if (pid > 0) {
            float pc = cpu_monitor_get_process(&srv->cpu_monitor, pid);
            if (pc > 0) sec_cpu += pc;
        }
    }
    int sec_cpu_i = (int)(sec_cpu + 0.5f);
    cJSON_AddNumberToObject(cpu, "streamer_cpu", sec_cpu_i);
    cJSON_AddItemToObject(root, "cpu", cpu);

    cJSON_AddNumberToObject(root, "encoder_cpu", enc_cpu_i);
    cJSON_AddNumberToObject(root, "streamer_cpu", sec_cpu_i);

    /* FPS (round to 1 decimal place) */
    cJSON *fps = cJSON_CreateObject();
    cJSON_AddNumberToObject(fps, "mjpeg",
        ((int)(srv->encoder_mjpeg_fps * 10 + 0.5f)) / 10.0);
    /* Use FLV proxy FPS when proxy is active and local encoder reports 0 */
    float h264_fps = srv->encoder_h264_fps;
    if (h264_fps < 0.1f) {
        float proxy_fps = flv_proxy_get_fps();
        if (proxy_fps > 0.1f)
            h264_fps = proxy_fps;
    }
    cJSON_AddNumberToObject(fps, "h264",
        ((int)(h264_fps * 10 + 0.5f)) / 10.0);
    cJSON_AddItemToObject(root, "fps", fps);

    /* Clients */
    cJSON *clients = cJSON_CreateObject();
    cJSON_AddNumberToObject(clients, "mjpeg", srv->encoder_mjpeg_clients);
    cJSON_AddNumberToObject(clients, "flv", srv->encoder_flv_clients);
    cJSON_AddItemToObject(root, "clients", clients);

    /* Settings */
    cJSON_AddStringToObject(root, "encoder_type", srv->config->encoder_type);
    cJSON_AddBoolToObject(root, "h264_enabled", srv->config->h264_enabled);
    /* Runtime skip_ratio (auto-adjusted) vs saved config value */
    int rt_skip = srv->runtime_skip_ratio > 0 ? srv->runtime_skip_ratio
                                               : srv->config->skip_ratio;
    cJSON_AddNumberToObject(root, "skip_ratio", rt_skip);
    cJSON_AddNumberToObject(root, "saved_skip_ratio", srv->config->skip_ratio);
    cJSON_AddBoolToObject(root, "auto_skip", srv->config->auto_skip);
    cJSON_AddNumberToObject(root, "target_cpu", srv->config->target_cpu);
    cJSON_AddBoolToObject(root, "autolanmode", srv->config->autolanmode);
    cJSON_AddNumberToObject(root, "mjpeg_fps_target", srv->config->mjpeg_fps);
    cJSON_AddNumberToObject(root, "max_camera_fps", srv->max_camera_fps);
    cJSON_AddStringToObject(root, "session_id", srv->session_id);
    cJSON_AddBoolToObject(root, "display_enabled", srv->config->display_enabled);
    cJSON_AddNumberToObject(root, "display_fps", srv->config->display_fps);
    cJSON_AddStringToObject(root, "mode", srv->config->mode);

    /* Fault detection status */
    {
        cJSON *fd_obj = cJSON_CreateObject();
        fd_state_t fd_state = fault_detect_get_state();
        static const char *fd_status_names[] = {
            "disabled", "enabled", "active", "error", "no_npu", "mem_low"
        };
        int si = (int)fd_state.status;
        if (si < 0 || si > 5) si = 3;
        cJSON_AddStringToObject(fd_obj, "status", fd_status_names[si]);
        cJSON_AddStringToObject(fd_obj, "detection",
            fd_state.last_result.result == FD_CLASS_FAULT ? "fault" : "ok");
        cJSON_AddStringToObject(fd_obj, "fault_class",
            fd_state.last_result.fault_class_name);
        cJSON_AddNumberToObject(fd_obj, "confidence",
            ((int)(fd_state.last_result.confidence * 100 + 0.5f)) / 100.0);
        cJSON_AddNumberToObject(fd_obj, "inference_ms",
            (int)(fd_state.last_result.total_ms + 0.5f));
        cJSON_AddNumberToObject(fd_obj, "cycle_count",
            (double)fd_state.cycle_count);
        cJSON_AddBoolToObject(fd_obj, "npu_available",
            fault_detect_npu_available());
        cJSON_AddItemToObject(root, "fault_detect", fd_obj);
    }

    send_json_response(fd, 200, root);
    cJSON_Delete(root);
}

/* GET /api/config - Full running config */
static void serve_api_config(ControlServer *srv, int fd) {
    AppConfig *cfg = srv->config;
    cJSON *root = cJSON_CreateObject();

    cJSON_AddStringToObject(root, "encoder_type", cfg->encoder_type);
    cJSON_AddNumberToObject(root, "streaming_port", cfg->streaming_port);
    cJSON_AddNumberToObject(root, "control_port", cfg->control_port);
    cJSON_AddBoolToObject(root, "h264_enabled", cfg->h264_enabled);
    cJSON_AddStringToObject(root, "h264_resolution", cfg->h264_resolution);
    cJSON_AddNumberToObject(root, "h264_bitrate", cfg->bitrate);
    cJSON_AddNumberToObject(root, "mjpeg_fps", cfg->mjpeg_fps);
    cJSON_AddNumberToObject(root, "jpeg_quality", cfg->jpeg_quality);
    cJSON_AddNumberToObject(root, "skip_ratio", cfg->skip_ratio);
    cJSON_AddBoolToObject(root, "auto_skip", cfg->auto_skip);
    cJSON_AddNumberToObject(root, "target_cpu", cfg->target_cpu);
    cJSON_AddBoolToObject(root, "display_enabled", cfg->display_enabled);
    cJSON_AddNumberToObject(root, "display_fps", cfg->display_fps);
    cJSON_AddBoolToObject(root, "autolanmode", cfg->autolanmode);
    cJSON_AddStringToObject(root, "mode", cfg->mode);
    cJSON_AddBoolToObject(root, "timelapse_enabled", cfg->timelapse_enabled);
    cJSON_AddStringToObject(root, "timelapse_mode", cfg->timelapse_mode);
    cJSON_AddNumberToObject(root, "timelapse_hyperlapse_interval", cfg->timelapse_hyperlapse_interval);
    cJSON_AddStringToObject(root, "timelapse_storage", cfg->timelapse_storage);
    cJSON_AddStringToObject(root, "timelapse_usb_path", cfg->timelapse_usb_path);
    cJSON_AddNumberToObject(root, "timelapse_output_fps", cfg->timelapse_output_fps);
    cJSON_AddBoolToObject(root, "timelapse_variable_fps", cfg->timelapse_variable_fps);
    cJSON_AddNumberToObject(root, "timelapse_target_length", cfg->timelapse_target_length);
    cJSON_AddNumberToObject(root, "timelapse_crf", cfg->timelapse_crf);
    cJSON_AddNumberToObject(root, "timelapse_duplicate_last_frame", cfg->timelapse_duplicate_last_frame);
    cJSON_AddNumberToObject(root, "timelapse_stream_delay", cfg->timelapse_stream_delay);
    cJSON_AddBoolToObject(root, "timelapse_flip_x", cfg->timelapse_flip_x);
    cJSON_AddBoolToObject(root, "timelapse_flip_y", cfg->timelapse_flip_y);
    cJSON_AddStringToObject(root, "session_id", srv->session_id);
    cJSON_AddBoolToObject(root, "acproxycam_flv_proxy", cfg->acproxycam_flv_proxy);

    send_json_response(fd, 200, root);
    cJSON_Delete(root);
}

/* GET /api/fault_detect/models - List available models */
static void serve_fault_detect_models(ControlServer *srv, int fd) {
    (void)srv;
    fd_model_info_t models[FD_MAX_MODELS * 3];
    int count = fault_detect_scan_models(models, FD_MAX_MODELS * 3);

    fd_config_t fd_cfg = fault_detect_get_config();

    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < count; i++) {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "name", models[i].name);
        cJSON_AddStringToObject(item, "path", models[i].path);
        const char *cls_name = models[i].cls == FD_MODEL_CNN ? "cnn" :
                               models[i].cls == FD_MODEL_PROTONET ? "protonet" :
                               "multiclass";
        cJSON_AddStringToObject(item, "class", cls_name);

        /* Check if this model is currently selected */
        int selected = 0;
        if (models[i].cls == FD_MODEL_CNN &&
            strcmp(models[i].name, fd_cfg.cnn_model) == 0)
            selected = 1;
        else if (models[i].cls == FD_MODEL_PROTONET &&
                 strcmp(models[i].name, fd_cfg.proto_model) == 0)
            selected = 1;
        else if (models[i].cls == FD_MODEL_MULTICLASS &&
                 strcmp(models[i].name, fd_cfg.multi_model) == 0)
            selected = 1;
        cJSON_AddBoolToObject(item, "selected", selected);

        cJSON_AddItemToArray(arr, item);
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddItemToObject(root, "models", arr);
    cJSON_AddBoolToObject(root, "npu_available", fault_detect_npu_available());

    send_json_response(fd, 200, root);
    cJSON_Delete(root);
}

/* POST /api/fault_detect/settings - Update fault detection settings */
static void handle_fault_detect_settings(ControlServer *srv, int fd,
                                          const char *body) {
    cJSON *root = cJSON_Parse(body);
    if (!root) {
        send_http_response(fd, 400, "application/json",
                          "{\"status\":\"error\",\"error\":\"invalid JSON\"}", 42,
                          NULL);
        return;
    }

    AppConfig *cfg = srv->config;

    /* Update config fields from JSON */
    const cJSON *item;
    item = cJSON_GetObjectItemCaseSensitive(root, "enabled");
    if (item) cfg->fault_detect_enabled = cJSON_IsTrue(item) ? 1 : 0;

    item = cJSON_GetObjectItemCaseSensitive(root, "cnn_enabled");
    if (item) cfg->fault_detect_cnn_enabled = cJSON_IsTrue(item) ? 1 : 0;

    item = cJSON_GetObjectItemCaseSensitive(root, "proto_enabled");
    if (item) cfg->fault_detect_proto_enabled = cJSON_IsTrue(item) ? 1 : 0;

    item = cJSON_GetObjectItemCaseSensitive(root, "multi_enabled");
    if (item) cfg->fault_detect_multi_enabled = cJSON_IsTrue(item) ? 1 : 0;

    item = cJSON_GetObjectItemCaseSensitive(root, "strategy");
    if (item && cJSON_IsString(item) && item->valuestring)
        safe_strcpy(cfg->fault_detect_strategy,
                    sizeof(cfg->fault_detect_strategy), item->valuestring);

    item = cJSON_GetObjectItemCaseSensitive(root, "interval");
    if (item && cJSON_IsNumber(item)) {
        int v = item->valueint;
        if (v >= 1 && v <= 60) cfg->fault_detect_interval = v;
    }

    item = cJSON_GetObjectItemCaseSensitive(root, "verify_interval");
    if (item && cJSON_IsNumber(item)) {
        int v = item->valueint;
        if (v >= 1 && v <= 30) cfg->fault_detect_verify_interval = v;
    }

    item = cJSON_GetObjectItemCaseSensitive(root, "cnn_model");
    if (item && cJSON_IsString(item) && item->valuestring)
        safe_strcpy(cfg->fault_detect_cnn_model,
                    sizeof(cfg->fault_detect_cnn_model), item->valuestring);

    item = cJSON_GetObjectItemCaseSensitive(root, "proto_model");
    if (item && cJSON_IsString(item) && item->valuestring)
        safe_strcpy(cfg->fault_detect_proto_model,
                    sizeof(cfg->fault_detect_proto_model), item->valuestring);

    item = cJSON_GetObjectItemCaseSensitive(root, "multi_model");
    if (item && cJSON_IsString(item) && item->valuestring)
        safe_strcpy(cfg->fault_detect_multi_model,
                    sizeof(cfg->fault_detect_multi_model), item->valuestring);

    item = cJSON_GetObjectItemCaseSensitive(root, "min_free_mem");
    if (item && cJSON_IsNumber(item)) {
        int v = item->valueint;
        if (v >= 5 && v <= 100) cfg->fault_detect_min_free_mem = v;
    }

    cJSON_Delete(root);

    /* Save to disk */
    config_save(cfg, cfg->config_file);

    /* Apply live update via config-changed callback */
    if (srv->on_config_changed)
        srv->on_config_changed(cfg);

    send_http_response(fd, 200, "application/json",
                      "{\"status\":\"ok\"}", 15, NULL);
}

/* GET /api/camera/controls - Camera control ranges and values */
static void serve_camera_controls(ControlServer *srv, int fd, int camera_id) {
    AppConfig *cfg = srv->config;
    cJSON *root = cJSON_CreateObject();

    /* For secondary cameras (id >= 2), return defaults since we
     * don't store their per-camera control values in config */
    int use_defaults = (camera_id >= 2);

    /* Helper macro for camera controls */
    #define ADD_CTRL(name, field, min_v, max_v, def_v) do { \
        cJSON *c = cJSON_CreateObject(); \
        cJSON_AddNumberToObject(c, "value", use_defaults ? (def_v) : cfg->field); \
        cJSON_AddNumberToObject(c, "min", min_v); \
        cJSON_AddNumberToObject(c, "max", max_v); \
        cJSON_AddNumberToObject(c, "default", def_v); \
        cJSON_AddItemToObject(root, name, c); \
    } while(0)

    ADD_CTRL("brightness", cam_brightness, 0, 255, 0);
    ADD_CTRL("contrast", cam_contrast, 0, 255, 32);
    ADD_CTRL("saturation", cam_saturation, 0, 132, 85);
    ADD_CTRL("hue", cam_hue, -180, 180, 0);
    ADD_CTRL("gamma", cam_gamma, 90, 150, 100);
    ADD_CTRL("sharpness", cam_sharpness, 0, 30, 3);
    ADD_CTRL("gain", cam_gain, 0, 1, 1);
    ADD_CTRL("backlight", cam_backlight, 0, 7, 0);
    ADD_CTRL("wb_auto", cam_wb_auto, 0, 1, 1);
    ADD_CTRL("wb_temp", cam_wb_temp, 2800, 6500, 4000);

    /* Controls with options */
    cJSON *ea = cJSON_CreateObject();
    cJSON_AddNumberToObject(ea, "value", use_defaults ? 3 : cfg->cam_exposure_auto);
    cJSON_AddNumberToObject(ea, "min", 1);
    cJSON_AddNumberToObject(ea, "max", 3);
    cJSON_AddNumberToObject(ea, "default", 3);
    cJSON *ea_opts = cJSON_CreateObject();
    cJSON_AddStringToObject(ea_opts, "1", "Manual");
    cJSON_AddStringToObject(ea_opts, "3", "Auto");
    cJSON_AddItemToObject(ea, "options", ea_opts);
    cJSON_AddItemToObject(root, "exposure_auto", ea);

    ADD_CTRL("exposure", cam_exposure, 10, 2500, 156);
    ADD_CTRL("exposure_priority", cam_exposure_priority, 0, 1, 0);

    cJSON *pl = cJSON_CreateObject();
    cJSON_AddNumberToObject(pl, "value", use_defaults ? 1 : cfg->cam_power_line);
    cJSON_AddNumberToObject(pl, "min", 0);
    cJSON_AddNumberToObject(pl, "max", 2);
    cJSON_AddNumberToObject(pl, "default", 1);
    cJSON *pl_opts = cJSON_CreateObject();
    cJSON_AddStringToObject(pl_opts, "0", "Disabled");
    cJSON_AddStringToObject(pl_opts, "1", "50 Hz");
    cJSON_AddStringToObject(pl_opts, "2", "60 Hz");
    cJSON_AddItemToObject(pl, "options", pl_opts);
    cJSON_AddItemToObject(root, "power_line", pl);

    #undef ADD_CTRL

    send_json_response(fd, 200, root);
    cJSON_Delete(root);
}

/* POST /api/camera/set - Apply camera control */
static void handle_camera_set(ControlServer *srv, int fd, const char *body) {
    cJSON *req = cJSON_Parse(body);
    if (!req) {
        /* Try form-encoded */
        FormParam params[8];
        int nparams = parse_form_body(body, params, 8);
        const char *ctrl = form_get(params, nparams, "control");
        const char *val = form_get(params, nparams, "value");

        if (!ctrl || !val) {
            send_http_response(fd, 400, "text/plain", "Bad request", 11, NULL);
            return;
        }

        /* Write to command file */
        char cmd_line[640];
        snprintf(cmd_line, sizeof(cmd_line), "cam_%s=%s\n", ctrl, val);

        FILE *f = fopen("/tmp/h264_cmd", "a");
        if (f) {
            fputs(cmd_line, f);
            fclose(f);
        }

        /* Update config */
        int v = atoi(val);
        AppConfig *cfg = srv->config;
        if (strcmp(ctrl, "brightness") == 0) cfg->cam_brightness = v;
        else if (strcmp(ctrl, "contrast") == 0) cfg->cam_contrast = v;
        else if (strcmp(ctrl, "saturation") == 0) cfg->cam_saturation = v;
        else if (strcmp(ctrl, "hue") == 0) cfg->cam_hue = v;
        else if (strcmp(ctrl, "gamma") == 0) cfg->cam_gamma = v;
        else if (strcmp(ctrl, "sharpness") == 0) cfg->cam_sharpness = v;
        else if (strcmp(ctrl, "gain") == 0) cfg->cam_gain = v;
        else if (strcmp(ctrl, "backlight") == 0) cfg->cam_backlight = v;
        else if (strcmp(ctrl, "wb_auto") == 0) cfg->cam_wb_auto = v;
        else if (strcmp(ctrl, "wb_temp") == 0) cfg->cam_wb_temp = v;
        else if (strcmp(ctrl, "exposure_auto") == 0) cfg->cam_exposure_auto = v;
        else if (strcmp(ctrl, "exposure") == 0) cfg->cam_exposure = v;
        else if (strcmp(ctrl, "exposure_priority") == 0) cfg->cam_exposure_priority = v;
        else if (strcmp(ctrl, "power_line") == 0) cfg->cam_power_line = v;

        config_save(cfg, CONFIG_DEFAULT_PATH);

        cJSON *result = cJSON_CreateObject();
        cJSON_AddStringToObject(result, "status", "ok");
        send_json_response(fd, 200, result);
        cJSON_Delete(result);
        return;
    }

    /* JSON body format */
    const cJSON *ctrl = cJSON_GetObjectItemCaseSensitive(req, "control");
    const cJSON *val = cJSON_GetObjectItemCaseSensitive(req, "value");
    const cJSON *cam_id = cJSON_GetObjectItemCaseSensitive(req, "camera_id");
    int camera_id = (cam_id && cJSON_IsNumber(cam_id)) ? cam_id->valueint : 1;

    if (ctrl && val && cJSON_IsString(ctrl)) {
        char cmd_line[128];
        if (cJSON_IsNumber(val)) {
            snprintf(cmd_line, sizeof(cmd_line), "cam_%s=%d\n", ctrl->valuestring, val->valueint);
        } else if (cJSON_IsString(val)) {
            snprintf(cmd_line, sizeof(cmd_line), "cam_%s=%s\n", ctrl->valuestring, val->valuestring);
        }

        /* Write to correct camera's command file */
        char cmd_path[64];
        if (camera_id <= 1)
            snprintf(cmd_path, sizeof(cmd_path), "/tmp/h264_cmd");
        else
            snprintf(cmd_path, sizeof(cmd_path), "/tmp/h264_cmd_%d", camera_id);

        FILE *f = fopen(cmd_path, "a");
        if (f) {
            fputs(cmd_line, f);
            fclose(f);
        }

        /* Only persist config for primary camera */
        if (camera_id <= 1 && cJSON_IsNumber(val)) {
            int v = val->valueint;
            AppConfig *cfg = srv->config;
            if (strcmp(ctrl->valuestring, "brightness") == 0) cfg->cam_brightness = v;
            else if (strcmp(ctrl->valuestring, "contrast") == 0) cfg->cam_contrast = v;
            else if (strcmp(ctrl->valuestring, "saturation") == 0) cfg->cam_saturation = v;
            else if (strcmp(ctrl->valuestring, "hue") == 0) cfg->cam_hue = v;
            else if (strcmp(ctrl->valuestring, "gamma") == 0) cfg->cam_gamma = v;
            else if (strcmp(ctrl->valuestring, "sharpness") == 0) cfg->cam_sharpness = v;
            else if (strcmp(ctrl->valuestring, "gain") == 0) cfg->cam_gain = v;
            else if (strcmp(ctrl->valuestring, "backlight") == 0) cfg->cam_backlight = v;
            else if (strcmp(ctrl->valuestring, "wb_auto") == 0) cfg->cam_wb_auto = v;
            else if (strcmp(ctrl->valuestring, "wb_temp") == 0) cfg->cam_wb_temp = v;
            else if (strcmp(ctrl->valuestring, "exposure_auto") == 0) cfg->cam_exposure_auto = v;
            else if (strcmp(ctrl->valuestring, "exposure") == 0) cfg->cam_exposure = v;
            else if (strcmp(ctrl->valuestring, "exposure_priority") == 0) cfg->cam_exposure_priority = v;
            else if (strcmp(ctrl->valuestring, "power_line") == 0) cfg->cam_power_line = v;
            config_save(cfg, CONFIG_DEFAULT_PATH);
        }
    }

    cJSON *result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "status", "ok");
    send_json_response(fd, 200, result);
    cJSON_Delete(result);
    cJSON_Delete(req);
}

/* POST /api/touch - Inject touch event */
static void handle_touch(ControlServer *srv, int fd, const char *body) {
    int x = 0, y = 0, duration = 100;

    /* Parse body (form-encoded or JSON) */
    cJSON *req = cJSON_Parse(body);
    if (req) {
        const cJSON *jx = cJSON_GetObjectItemCaseSensitive(req, "x");
        const cJSON *jy = cJSON_GetObjectItemCaseSensitive(req, "y");
        const cJSON *jd = cJSON_GetObjectItemCaseSensitive(req, "duration");
        if (jx && cJSON_IsNumber(jx)) x = jx->valueint;
        if (jy && cJSON_IsNumber(jy)) y = jy->valueint;
        if (jd && cJSON_IsNumber(jd)) duration = jd->valueint;
        cJSON_Delete(req);
    } else {
        FormParam params[4];
        int nparams = parse_form_body(body, params, 4);
        x = form_get_int(params, nparams, "x", 0);
        y = form_get_int(params, nparams, "y", 0);
        duration = form_get_int(params, nparams, "duration", 100);
    }

    int ret = touch_inject(x, y, duration);

    cJSON *result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "status", ret == 0 ? "ok" : "error");
    cJSON_AddNumberToObject(result, "x", x);
    cJSON_AddNumberToObject(result, "y", y);
    send_json_response(fd, 200, result);
    cJSON_Delete(result);
}

/* GET /status - Plain text status */
static void serve_status(ControlServer *srv, int fd) {
    read_encoder_stats(srv);
    cpu_monitor_update(&srv->cpu_monitor);

    char buf[2048];
    int len = snprintf(buf, sizeof(buf),
        "H264 Streamer Status\n"
        "====================\n"
        "Encoder: %s\n"
        "Streaming port: %d\n"
        "Control port: %d\n"
        "H.264: %s (skip=%d, auto=%s, target_cpu=%d%%)\n"
        "MJPEG FPS: %.1f (target: %d)\n"
        "H.264 FPS: %.1f\n"
        "Clients: %d MJPEG, %d FLV\n"
        "CPU: %.0f%%\n"
        "Display: %s (fps=%d)\n"
        "Timelapse: %s (mode=%s)\n"
        "LAN mode: auto=%s\n",
        srv->config->encoder_type,
        srv->config->streaming_port,
        srv->config->control_port,
        srv->config->h264_enabled ? "enabled" : "disabled",
        srv->config->skip_ratio,
        srv->config->auto_skip ? "yes" : "no",
        srv->config->target_cpu,
        srv->encoder_mjpeg_fps, srv->config->mjpeg_fps,
        srv->encoder_h264_fps,
        srv->encoder_mjpeg_clients, srv->encoder_flv_clients,
        cpu_monitor_get_total(&srv->cpu_monitor),
        srv->config->display_enabled ? "enabled" : "disabled",
        srv->config->display_fps,
        srv->config->timelapse_enabled ? "enabled" : "disabled",
        srv->config->timelapse_mode,
        srv->config->autolanmode ? "yes" : "no"
    );

    send_http_response(fd, 200, "text/plain; charset=utf-8", buf, len, NULL);
}

/* GET /timelapse - Timelapse browser page */
static void serve_timelapse_page(ControlServer *srv, int fd) {
    char *tmpl = load_template(srv->template_dir, "timelapse.html");
    if (!tmpl) {
        send_http_response(fd, 500, "text/plain",
                          "Template not found", 18, NULL);
        return;
    }

    /* No template variables needed - all data loaded via JS */
    send_http_response(fd, 200, "text/html; charset=utf-8", tmpl, strlen(tmpl),
                      "Cache-Control: no-cache\r\n");
    free(tmpl);
}

/* Helper: get timelapse directory path */
static const char *get_timelapse_dir(ControlServer *srv, const char *storage) {
    if (storage && strcmp(storage, "usb") == 0) {
        /* Use configured USB path if set, otherwise default */
        if (srv->config->timelapse_usb_path[0])
            return srv->config->timelapse_usb_path;
        return TIMELAPSE_DIR_USB;
    }
    return TIMELAPSE_DIR_INTERNAL;
}

/* GET /api/timelapse/list - JSON list of recordings.
 * Groups MP4 files with their JPG thumbnails.
 * Thumbnail naming: <base>_<frames>.jpg matches <base>.mp4 */
static void serve_timelapse_list(ControlServer *srv, int fd, const char *storage) {
    const char *dir_path = get_timelapse_dir(srv, storage);

    cJSON *root = cJSON_CreateObject();
    cJSON *recordings = cJSON_CreateArray();
    uint64_t total_size = 0;

    /* First pass: collect all filenames */
    DIR *dir = opendir(dir_path);
    if (dir) {
        /* Collect MP4 and JPG names in arrays (max 200 entries) */
        #define TL_MAX_ENTRIES 200
        char *mp4_names[TL_MAX_ENTRIES];
        char *jpg_names[TL_MAX_ENTRIES];
        int mp4_count = 0, jpg_count = 0;

        struct dirent *entry;
        while ((entry = readdir(dir)) != NULL) {
            const char *name = entry->d_name;
            size_t nlen = strlen(name);
            if (nlen < 5) continue;
            if (strcasecmp(name + nlen - 4, ".mp4") == 0 && mp4_count < TL_MAX_ENTRIES)
                mp4_names[mp4_count++] = strdup(name);
            else if (strcasecmp(name + nlen - 4, ".jpg") == 0 && jpg_count < TL_MAX_ENTRIES)
                jpg_names[jpg_count++] = strdup(name);
        }
        closedir(dir);

        /* Process each MP4 and find matching thumbnail */
        for (int i = 0; i < mp4_count; i++) {
            const char *mp4 = mp4_names[i];
            size_t mp4len = strlen(mp4);
            /* Base name = mp4 without .mp4 extension */
            size_t baselen = mp4len - 4;

            char filepath[512];
            snprintf(filepath, sizeof(filepath), "%s/%s", dir_path, mp4);

            struct stat st;
            if (stat(filepath, &st) != 0) continue;

            /* Find matching thumbnail: <base>_<number>.jpg */
            const char *thumb = NULL;
            int frames = 0;
            for (int j = 0; j < jpg_count; j++) {
                if (!jpg_names[j]) continue;
                /* Must start with base name + '_' */
                if (strncmp(jpg_names[j], mp4, baselen) == 0 && jpg_names[j][baselen] == '_') {
                    /* Extract frame count from suffix */
                    const char *num_start = jpg_names[j] + baselen + 1;
                    char *dot = strrchr(num_start, '.');
                    if (dot && dot > num_start) {
                        int f = atoi(num_start);
                        if (f > frames) {
                            frames = f;
                            thumb = jpg_names[j];
                        }
                    }
                }
            }

            cJSON *rec = cJSON_CreateObject();
            /* name = base name (used as group ID for delete) */
            char base[256];
            snprintf(base, sizeof(base), "%.*s", (int)baselen, mp4);
            cJSON_AddStringToObject(rec, "name", base);
            cJSON_AddStringToObject(rec, "mp4", mp4);
            if (thumb)
                cJSON_AddStringToObject(rec, "thumbnail", thumb);
            cJSON_AddNumberToObject(rec, "frames", frames);
            cJSON_AddNumberToObject(rec, "size", (double)st.st_size);
            cJSON_AddNumberToObject(rec, "mtime", (double)st.st_mtime);
            total_size += st.st_size;

            /* Get duration using ffprobe if available */
            char cmd[768];
            snprintf(cmd, sizeof(cmd),
                "%s -v error -show_entries format=duration "
                "-of csv=p=0 '%s' 2>/dev/null",
                FFPROBE_PATH, filepath);
            FILE *p = popen(cmd, "r");
            if (p) {
                char dur_buf[32];
                if (fgets(dur_buf, sizeof(dur_buf), p)) {
                    cJSON_AddNumberToObject(rec, "duration", atof(dur_buf));
                }
                pclose(p);
            }

            cJSON_AddItemToArray(recordings, rec);
        }

        /* Free names */
        for (int i = 0; i < mp4_count; i++) free(mp4_names[i]);
        for (int i = 0; i < jpg_count; i++) free(jpg_names[i]);
    }

    cJSON_AddItemToObject(root, "recordings", recordings);
    cJSON_AddNumberToObject(root, "total_size", (double)total_size);
    cJSON_AddStringToObject(root, "storage", storage ? storage : "internal");
    cJSON_AddStringToObject(root, "path", dir_path);

    send_json_response(fd, 200, root);
    cJSON_Delete(root);
}

/* GET /api/timelapse/thumb/<name> - Serve thumbnail JPEG.
 * If <name> is a .jpg that exists in the timelapse dir, serve it directly.
 * Otherwise try extracting first frame from video via ffmpeg. */
static void serve_timelapse_thumb(ControlServer *srv, int fd,
                                   const char *name, const char *storage) {
    const char *dir_path = get_timelapse_dir(srv, storage);
    char filepath[512];
    snprintf(filepath, sizeof(filepath), "%s/%s", dir_path, name);

    /* If the requested file is a JPG that exists, serve directly */
    size_t nlen = strlen(name);
    int is_jpg = (nlen > 4 && strcasecmp(name + nlen - 4, ".jpg") == 0);

    struct stat st;
    if (is_jpg && stat(filepath, &st) == 0 && st.st_size > 0 && st.st_size <= 512 * 1024) {
        FILE *f = fopen(filepath, "rb");
        if (f) {
            char *data = malloc(st.st_size);
            if (data) {
                fread(data, 1, st.st_size, f);
                fclose(f);
                send_http_response(fd, 200, "image/jpeg", data, st.st_size,
                                  "Cache-Control: max-age=3600\r\n");
                free(data);
                return;
            }
            fclose(f);
        }
    }

    /* Fallback: extract first frame from video via ffmpeg */
    if (stat(filepath, &st) != 0) {
        send_404(fd);
        return;
    }

    char cmd[768];
    snprintf(cmd, sizeof(cmd),
        "%s -y -i '%s' -vframes 1 -q:v 5 -f image2 /tmp/timelapse_thumb.jpg 2>/dev/null",
        FFMPEG_PATH, filepath);

    if (system(cmd) != 0) {
        send_404(fd);
        return;
    }

    FILE *f = fopen("/tmp/timelapse_thumb.jpg", "rb");
    if (!f) { send_404(fd); return; }

    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (fsize <= 0 || fsize > 512 * 1024) { fclose(f); send_404(fd); return; }

    char *jpeg_data = malloc(fsize);
    if (!jpeg_data) { fclose(f); send_404(fd); return; }

    fread(jpeg_data, 1, fsize, f);
    fclose(f);
    unlink("/tmp/timelapse_thumb.jpg");

    send_http_response(fd, 200, "image/jpeg", jpeg_data, fsize,
                      "Cache-Control: max-age=3600\r\n");
    free(jpeg_data);
}

/* GET /api/timelapse/video/<name> - Video download with Range support */
static void serve_timelapse_video(ControlServer *srv, int fd,
                                   const char *name, const char *storage,
                                   const char *request) {
    const char *dir_path = get_timelapse_dir(srv, storage);
    char filepath[512];
    snprintf(filepath, sizeof(filepath), "%s/%s", dir_path, name);

    struct stat st;
    if (stat(filepath, &st) != 0) {
        send_404(fd);
        return;
    }

    long file_size = st.st_size;

    /* Check for Range header */
    long range_start = 0;
    long range_end = file_size - 1;
    int has_range = 0;

    if (request) {
        const char *range_hdr = strstr(request, "Range: bytes=");
        if (range_hdr) {
            has_range = 1;
            range_hdr += 13; /* skip "Range: bytes=" */
            range_start = atol(range_hdr);
            const char *dash = strchr(range_hdr, '-');
            if (dash && dash[1] && dash[1] != '\r') {
                range_end = atol(dash + 1);
            }
            if (range_start < 0) range_start = 0;
            if (range_end >= file_size) range_end = file_size - 1;
            if (range_start > range_end) range_start = 0;
        }
    }

    FILE *f = fopen(filepath, "rb");
    if (!f) {
        send_404(fd);
        return;
    }

    if (range_start > 0) {
        fseek(f, range_start, SEEK_SET);
    }

    long content_length = range_end - range_start + 1;

    /* Send headers */
    char headers[512];
    if (has_range) {
        snprintf(headers, sizeof(headers),
            "HTTP/1.1 206 Partial Content\r\n"
            "Content-Type: video/mp4\r\n"
            "Content-Length: %ld\r\n"
            "Content-Range: bytes %ld-%ld/%ld\r\n"
            "Accept-Ranges: bytes\r\n"
            "Connection: close\r\n"
            "\r\n",
            content_length, range_start, range_end, file_size);
    } else {
        snprintf(headers, sizeof(headers),
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: video/mp4\r\n"
            "Content-Length: %ld\r\n"
            "Accept-Ranges: bytes\r\n"
            "Content-Disposition: inline; filename=\"%s\"\r\n"
            "Connection: close\r\n"
            "\r\n",
            file_size, name);
    }
    ctrl_send(fd, headers, strlen(headers));

    /* Stream file in chunks */
    char chunk[32768];
    long remaining = content_length;
    while (remaining > 0) {
        size_t to_read = remaining > (long)sizeof(chunk) ? sizeof(chunk) : (size_t)remaining;
        size_t nread = fread(chunk, 1, to_read, f);
        if (nread == 0) break;
        if (ctrl_send(fd, chunk, nread) < 0) break;
        remaining -= nread;
    }

    fclose(f);
}

/* DELETE /api/timelapse/delete/<name> - Delete recording.
 * <name> is the base name (without extension).
 * Deletes <name>.mp4 and any <name>_*.jpg thumbnails. */
static void handle_timelapse_delete(ControlServer *srv, int fd,
                                     const char *name, const char *storage) {
    const char *dir_path = get_timelapse_dir(srv, storage);

    cJSON *result = cJSON_CreateObject();

    /* Validate: no path traversal */
    if (strchr(name, '/') || strchr(name, '\\') || strstr(name, "..")) {
        cJSON_AddStringToObject(result, "status", "error");
        cJSON_AddStringToObject(result, "message", "Invalid filename");
        send_json_response(fd, 400, result);
        cJSON_Delete(result);
        return;
    }

    /* Delete the MP4 file */
    char filepath[512];
    snprintf(filepath, sizeof(filepath), "%s/%s.mp4", dir_path, name);
    int mp4_ok = (unlink(filepath) == 0);

    /* Delete matching thumbnail(s): <name>_*.jpg */
    size_t namelen = strlen(name);
    DIR *dir = opendir(dir_path);
    if (dir) {
        struct dirent *entry;
        while ((entry = readdir(dir)) != NULL) {
            const char *fn = entry->d_name;
            size_t fnlen = strlen(fn);
            if (fnlen < 5) continue;
            if (strcasecmp(fn + fnlen - 4, ".jpg") != 0) continue;
            if (strncmp(fn, name, namelen) == 0 && fn[namelen] == '_') {
                snprintf(filepath, sizeof(filepath), "%s/%s", dir_path, fn);
                unlink(filepath);
            }
        }
        closedir(dir);
    }

    if (mp4_ok) {
        cJSON_AddStringToObject(result, "status", "ok");
        fprintf(stderr, "Timelapse: Deleted %s.mp4 (+ thumbnails)\n", name);
    } else {
        cJSON_AddStringToObject(result, "status", "error");
        cJSON_AddStringToObject(result, "message", strerror(errno));
    }

    send_json_response(fd, 200, result);
    cJSON_Delete(result);
}

/* GET /api/timelapse/storage - Storage usage info */
static void serve_timelapse_storage(ControlServer *srv, int fd) {
    cJSON *root = cJSON_CreateObject();

    /* Internal storage */
    struct statvfs internal_stat;
    if (statvfs(TIMELAPSE_DIR_INTERNAL, &internal_stat) == 0) {
        uint64_t total = (uint64_t)internal_stat.f_blocks * internal_stat.f_frsize;
        uint64_t avail = (uint64_t)internal_stat.f_bavail * internal_stat.f_frsize;
        cJSON_AddNumberToObject(root, "internal_total_mb", (double)total / (1024 * 1024));
        cJSON_AddNumberToObject(root, "internal_free_mb", (double)avail / (1024 * 1024));
    }

    /* USB storage */
    struct stat usb_stat;
    int usb_mounted = (stat("/mnt/udisk", &usb_stat) == 0 && S_ISDIR(usb_stat.st_mode));

    /* Check if /mnt/udisk is actually a mount point (not just empty dir) */
    if (usb_mounted) {
        struct statvfs usb_vfs;
        if (statvfs("/mnt/udisk", &usb_vfs) == 0) {
            uint64_t total = (uint64_t)usb_vfs.f_blocks * usb_vfs.f_frsize;
            if (total > 0) {
                uint64_t avail = (uint64_t)usb_vfs.f_bavail * usb_vfs.f_frsize;
                cJSON_AddNumberToObject(root, "usb_total_mb", (double)total / (1024 * 1024));
                cJSON_AddNumberToObject(root, "usb_free_mb", (double)avail / (1024 * 1024));
            } else {
                usb_mounted = 0;
            }
        } else {
            usb_mounted = 0;
        }
    }

    cJSON_AddBoolToObject(root, "usb_mounted", usb_mounted);
    cJSON_AddStringToObject(root, "current", srv->config->timelapse_storage);

    send_json_response(fd, 200, root);
    cJSON_Delete(root);
}

/* GET /api/timelapse/browse - Browse USB folders */
static void serve_timelapse_browse(ControlServer *srv, int fd, const char *path) {
    cJSON *root = cJSON_CreateObject();

    /* Security: only allow browsing under /mnt/udisk */
    if (!path || strstr(path, "..") || strncmp(path, "/mnt/udisk", 10) != 0) {
        cJSON_AddStringToObject(root, "error", "Invalid path");
        send_json_response(fd, 400, root);
        cJSON_Delete(root);
        return;
    }

    cJSON *folders = cJSON_CreateArray();
    DIR *dir = opendir(path);
    if (dir) {
        struct dirent *entry;
        while ((entry = readdir(dir)) != NULL) {
            if (entry->d_name[0] == '.') continue;

            char fullpath[768];
            snprintf(fullpath, sizeof(fullpath), "%s/%s", path, entry->d_name);

            struct stat st;
            if (stat(fullpath, &st) == 0 && S_ISDIR(st.st_mode)) {
                cJSON_AddItemToArray(folders, cJSON_CreateString(entry->d_name));
            }
        }
        closedir(dir);
    } else {
        cJSON_AddStringToObject(root, "error", strerror(errno));
    }

    cJSON_AddItemToObject(root, "folders", folders);
    cJSON_AddStringToObject(root, "path", path);

    send_json_response(fd, 200, root);
    cJSON_Delete(root);
}

/* POST /api/timelapse/settings - Update timelapse settings */
static void handle_timelapse_settings(ControlServer *srv, int fd, const char *body) {
    FormParam params[CTRL_MAX_FORM_PARAMS];
    int nparams = parse_form_body(body, params, CTRL_MAX_FORM_PARAMS);

    AppConfig *cfg = srv->config;

    cfg->timelapse_enabled = form_has(params, nparams, "timelapse_enabled") &&
                             strcmp(form_get(params, nparams, "timelapse_enabled"), "1") == 0;

    const char *val;
    val = form_get(params, nparams, "timelapse_mode");
    if (val) safe_strcpy(cfg->timelapse_mode, sizeof(cfg->timelapse_mode), val);

    val = form_get(params, nparams, "timelapse_hyperlapse_interval");
    if (val) cfg->timelapse_hyperlapse_interval = atoi(val);

    val = form_get(params, nparams, "timelapse_storage");
    if (val) safe_strcpy(cfg->timelapse_storage, sizeof(cfg->timelapse_storage), val);

    val = form_get(params, nparams, "timelapse_usb_path");
    if (val) safe_strcpy(cfg->timelapse_usb_path, sizeof(cfg->timelapse_usb_path), val);

    val = form_get(params, nparams, "timelapse_output_fps");
    if (val) cfg->timelapse_output_fps = atoi(val);

    cfg->timelapse_variable_fps = form_has(params, nparams, "timelapse_variable_fps") &&
                                  strcmp(form_get(params, nparams, "timelapse_variable_fps"), "1") == 0;

    val = form_get(params, nparams, "timelapse_target_length");
    if (val) cfg->timelapse_target_length = atoi(val);

    val = form_get(params, nparams, "timelapse_variable_fps_min");
    if (val) cfg->timelapse_variable_fps_min = atoi(val);

    val = form_get(params, nparams, "timelapse_variable_fps_max");
    if (val) cfg->timelapse_variable_fps_max = atoi(val);

    val = form_get(params, nparams, "timelapse_crf");
    if (val) cfg->timelapse_crf = atoi(val);

    val = form_get(params, nparams, "timelapse_duplicate_last_frame");
    if (val) cfg->timelapse_duplicate_last_frame = atoi(val);

    val = form_get(params, nparams, "timelapse_stream_delay");
    if (val) cfg->timelapse_stream_delay = atof(val);

    cfg->timelapse_flip_x = form_has(params, nparams, "timelapse_flip_x") &&
                            strcmp(form_get(params, nparams, "timelapse_flip_x"), "1") == 0;
    cfg->timelapse_flip_y = form_has(params, nparams, "timelapse_flip_y") &&
                            strcmp(form_get(params, nparams, "timelapse_flip_y"), "1") == 0;

    /* Save config */
    config_save(cfg, CONFIG_DEFAULT_PATH);

    /* Notify encoder */
    if (srv->on_config_changed) {
        srv->on_config_changed(cfg);
    }

    cJSON *result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "status", "ok");
    send_json_response(fd, 200, result);
    cJSON_Delete(result);
}

/* POST /api/acproxycam/flv - FLV proxy announcement */
static void handle_acproxycam_flv_announce(ControlServer *srv, int fd,
                                            const char *body,
                                            struct sockaddr_in *client_addr) {
    cJSON *req = cJSON_Parse(body);
    int port = 8080;
    char ip[32] = {0};

    if (req) {
        const cJSON *jp = cJSON_GetObjectItemCaseSensitive(req, "port");
        if (jp && cJSON_IsNumber(jp)) port = jp->valueint;
        const cJSON *ji = cJSON_GetObjectItemCaseSensitive(req, "ip");
        if (ji && cJSON_IsString(ji)) {
            strncpy(ip, ji->valuestring, sizeof(ip) - 1);
        }
        cJSON_Delete(req);
    }

    if (!ip[0] && client_addr) {
        inet_ntop(AF_INET, &client_addr->sin_addr, ip, sizeof(ip));
    }

    if (ip[0]) {
        snprintf(srv->acproxycam_flv_url, sizeof(srv->acproxycam_flv_url),
                "http://%s:%d/flv", ip, port);
        srv->acproxycam_last_seen = time(NULL);
        fprintf(stderr, "ACProxyCam FLV announced: %s\n", srv->acproxycam_flv_url);

        /* Update FLV proxy URL for the FLV server to relay */
        if (srv->config->acproxycam_flv_proxy) {
            flv_proxy_set_url(srv->acproxycam_flv_url);
        }
    }

    cJSON *result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "status", "ok");
    cJSON_AddBoolToObject(result, "proxy_active", srv->config->acproxycam_flv_proxy);
    cJSON_AddStringToObject(result, "url", srv->acproxycam_flv_url);
    send_json_response(fd, 200, result);
    cJSON_Delete(result);
}

/* GET /api/acproxycam/flv - FLV proxy status */
static void serve_acproxycam_flv_status(ControlServer *srv, int fd) {
    int connected = (srv->acproxycam_flv_url[0] &&
                     (time(NULL) - srv->acproxycam_last_seen) < 60);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "enabled", srv->config->acproxycam_flv_proxy);
    cJSON_AddStringToObject(root, "url", srv->acproxycam_flv_url);
    cJSON_AddBoolToObject(root, "connected", connected);
    cJSON_AddNumberToObject(root, "flv_clients", srv->flv_proxy_clients);
    send_json_response(fd, 200, root);
    cJSON_Delete(root);
}

/* POST /api/restart - Restart application */
static void handle_restart(ControlServer *srv, int fd) {
    cJSON *result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "status", "restarting");
    send_json_response(fd, 200, result);
    cJSON_Delete(result);

    if (srv->on_restart) {
        srv->on_restart();
    }
}

/* POST /api/timelapse/mkdir - Create directory */
static void handle_timelapse_mkdir(ControlServer *srv, int fd, const char *body) {
    cJSON *req = cJSON_Parse(body);
    cJSON *result = cJSON_CreateObject();

    if (!req) {
        cJSON_AddBoolToObject(result, "success", 0);
        cJSON_AddStringToObject(result, "error", "Invalid JSON");
        send_json_response(fd, 400, result);
        cJSON_Delete(result);
        return;
    }

    const cJSON *jpath = cJSON_GetObjectItemCaseSensitive(req, "path");
    if (!jpath || !cJSON_IsString(jpath)) {
        cJSON_AddBoolToObject(result, "success", 0);
        cJSON_AddStringToObject(result, "error", "Missing path");
        send_json_response(fd, 400, result);
        cJSON_Delete(result);
        cJSON_Delete(req);
        return;
    }

    const char *path = jpath->valuestring;

    /* Security: only under /mnt/udisk */
    if (strstr(path, "..") || strncmp(path, "/mnt/udisk", 10) != 0) {
        cJSON_AddBoolToObject(result, "success", 0);
        cJSON_AddStringToObject(result, "error", "Invalid path");
        send_json_response(fd, 400, result);
        cJSON_Delete(result);
        cJSON_Delete(req);
        return;
    }

    if (mkdir(path, 0755) == 0) {
        cJSON_AddBoolToObject(result, "success", 1);
    } else {
        cJSON_AddBoolToObject(result, "success", 0);
        cJSON_AddStringToObject(result, "error", strerror(errno));
    }

    send_json_response(fd, 200, result);
    cJSON_Delete(result);
    cJSON_Delete(req);
}

/* GET /api/timelapse/moonraker - Moonraker connection status */
static void serve_timelapse_moonraker_status(ControlServer *srv, int fd) {
    cJSON *root = cJSON_CreateObject();

    if (g_moonraker_client) {
        /* Use real state from the WebSocket client */
        cJSON_AddBoolToObject(root, "connected",
                               moonraker_client_is_connected(g_moonraker_client));
        cJSON_AddStringToObject(root, "print_state",
                                 g_moonraker_client->print_state);
        cJSON_AddNumberToObject(root, "current_layer",
                                 g_moonraker_client->current_layer);
        cJSON_AddNumberToObject(root, "total_layers",
                                 g_moonraker_client->total_layers);
        cJSON_AddStringToObject(root, "filename",
                                 g_moonraker_client->filename);
        cJSON_AddBoolToObject(root, "timelapse_active",
                               g_moonraker_client->timelapse_active);
        cJSON_AddNumberToObject(root, "timelapse_frames",
                                 g_moonraker_client->timelapse_frames);
    } else {
        /* No moonraker client — try a quick TCP probe */
        int sock = socket(AF_INET, SOCK_STREAM, 0);
        int connected = 0;

        if (sock >= 0) {
            struct sockaddr_in addr = {
                .sin_family = AF_INET,
                .sin_port = htons(srv->config->moonraker_port),
            };
            inet_pton(AF_INET, srv->config->moonraker_host, &addr.sin_addr);

            int flags = fcntl(sock, F_GETFL, 0);
            fcntl(sock, F_SETFL, flags | O_NONBLOCK);

            int ret = connect(sock, (struct sockaddr *)&addr, sizeof(addr));
            if (ret == 0) {
                connected = 1;
            } else if (errno == EINPROGRESS) {
                fd_set wfds;
                FD_ZERO(&wfds);
                FD_SET(sock, &wfds);
                struct timeval tv = { .tv_sec = 2 };
                if (select(sock + 1, NULL, &wfds, NULL, &tv) > 0) {
                    int err = 0;
                    socklen_t len = sizeof(err);
                    getsockopt(sock, SOL_SOCKET, SO_ERROR, &err, &len);
                    connected = (err == 0);
                }
            }
            close(sock);
        }

        cJSON_AddBoolToObject(root, "connected", connected);
        cJSON_AddStringToObject(root, "print_state", "unknown");
        cJSON_AddNumberToObject(root, "current_layer", 0);
        cJSON_AddNumberToObject(root, "total_layers", 0);
        cJSON_AddStringToObject(root, "filename", "");
        cJSON_AddBoolToObject(root, "timelapse_active", 0);
        cJSON_AddNumberToObject(root, "timelapse_frames", 0);
    }

    cJSON_AddStringToObject(root, "host", srv->config->moonraker_host);
    cJSON_AddNumberToObject(root, "port", srv->config->moonraker_port);

    send_json_response(fd, 200, root);
    cJSON_Delete(root);
}

/* LED ON/OFF handler via MQTT light topic */
static void handle_led(ControlServer *srv, int fd, int on) {
    /* LED control only available in go-klipper mode (requires MQTT) */
    if (strcmp(srv->config->mode, "vanilla-klipper") == 0) {
        cJSON *result = cJSON_CreateObject();
        cJSON_AddStringToObject(result, "status", "error");
        cJSON_AddStringToObject(result, "message", "LED control not available in vanilla-klipper mode");
        send_json_response(fd, 200, result);
        cJSON_Delete(result);
        return;
    }

    int ret = mqtt_send_led(on, 100);

    cJSON *result = cJSON_CreateObject();
    if (ret == 0) {
        cJSON_AddStringToObject(result, "status", "ok");
    } else {
        cJSON_AddStringToObject(result, "status", "error");
        cJSON_AddStringToObject(result, "message", "MQTT not connected");
    }
    send_json_response(fd, 200, result);
    cJSON_Delete(result);
}

/* ============================================================================
 * Moonraker Camera Provisioning
 * ============================================================================ */

/* Resolve camera IP based on moonraker_camera_ip config setting.
 * The stream URL is loaded directly by the user's browser, so "auto"
 * must always use the printer's routable IP (active interface by route metric). */
static void resolve_camera_ip(const AppConfig *cfg, char *buf, size_t bufsize) {
    const char *mode = cfg->moonraker_camera_ip;
    if (strcmp(mode, "localhost") == 0) {
        safe_strcpy(buf, bufsize, "127.0.0.1");
    } else if (strcmp(mode, "eth0") == 0 || strcmp(mode, "eth1") == 0) {
        if (get_iface_ip(mode, buf, bufsize) != 0)
            safe_strcpy(buf, bufsize, "127.0.0.1");  /* fallback */
    } else {
        /* "auto" — always use printer's routable IP */
        if (get_ip_address(buf, bufsize) != 0)
            safe_strcpy(buf, bufsize, "127.0.0.1");  /* fallback */
    }
}

/* GET /api/network/interfaces - Return network interface IPs */
static void serve_network_interfaces(ControlServer *srv, int fd) {
    cJSON *root = cJSON_CreateObject();

    char ip[64];
    if (get_iface_ip("eth0", ip, sizeof(ip)) == 0)
        cJSON_AddStringToObject(root, "eth0", ip);
    if (get_iface_ip("eth1", ip, sizeof(ip)) == 0)
        cJSON_AddStringToObject(root, "eth1", ip);

    cJSON_AddStringToObject(root, "moonraker_camera_ip", srv->config->moonraker_camera_ip);

    send_json_response(fd, 200, root);
    cJSON_Delete(root);
}

/*
 * Make a one-shot HTTP POST to Moonraker to register/update a webcam entry.
 * Uses simple blocking socket with short timeout.
 */
static int moonraker_provision_camera(const char *host, int port,
                                       const char *name,
                                       const char *stream_url,
                                       const char *snapshot_url,
                                       int target_fps) {
    /* Build JSON body */
    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "name", name);
    cJSON_AddStringToObject(body, "location", "printer");
    cJSON_AddStringToObject(body, "icon", "mdiWebcam");
    cJSON_AddStringToObject(body, "service", "mjpegstreamer-adaptive");
    cJSON_AddStringToObject(body, "stream_url", stream_url);
    cJSON_AddStringToObject(body, "snapshot_url", snapshot_url);
    cJSON_AddNumberToObject(body, "target_fps", target_fps);
    cJSON_AddNumberToObject(body, "target_fps_idle", 1);
    cJSON_AddBoolToObject(body, "enabled", 1);
    char *json_str = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    if (!json_str) return -1;

    /* Connect to Moonraker */
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) { free(json_str); return -1; }

    struct timeval tv = { .tv_sec = 3, .tv_usec = 0 };
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, host, &addr.sin_addr);

    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(sock);
        free(json_str);
        return -1;
    }

    /* Send HTTP POST */
    int json_len = strlen(json_str);
    char header[512];
    int hlen = snprintf(header, sizeof(header),
        "POST /server/webcams/item HTTP/1.1\r\n"
        "Host: %s:%d\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n\r\n",
        host, port, json_len);

    send(sock, header, hlen, MSG_NOSIGNAL);
    send(sock, json_str, json_len, MSG_NOSIGNAL);
    free(json_str);

    /* Read response (just check status) */
    char resp[256];
    ssize_t n = recv(sock, resp, sizeof(resp) - 1, 0);
    close(sock);

    if (n > 0) {
        resp[n] = '\0';
        /* Check for 200 OK or 2xx */
        if (strstr(resp, "200") || strstr(resp, "201")) return 0;
    }
    return -1;
}

void control_server_provision_moonraker(ControlServer *srv) {
    if (!srv || !srv->config) return;

    const char *host = srv->config->moonraker_host;
    int port = srv->config->moonraker_port;

    if (!host[0] || port <= 0) return;

    /* Resolve camera IP based on config setting */
    char camera_ip[64];
    resolve_camera_ip(srv->config, camera_ip, sizeof(camera_ip));

    /* Parse cameras_json for per-camera moonraker settings */
    cJSON *cam_settings = NULL;
    if (srv->config->cameras_json[0]) {
        cam_settings = cJSON_Parse(srv->config->cameras_json);
    }

    /* Provision each camera */
    for (int i = 0; i < srv->num_cameras; i++) {
        CameraInfo *cam = &srv->cameras[i];
        if (!cam->enabled) continue;

        /* Check per-camera moonraker settings */
        int mr_enabled = 1;
        const char *mr_name = NULL;
        if (cam_settings) {
            cJSON *cs = cJSON_GetObjectItem(cam_settings, cam->unique_id);
            if (cs) {
                cJSON *en = cJSON_GetObjectItem(cs, "moonraker_enabled");
                if (en && !cJSON_IsTrue(en)) mr_enabled = 0;
                cJSON *nm = cJSON_GetObjectItem(cs, "moonraker_name");
                if (nm && nm->valuestring && nm->valuestring[0])
                    mr_name = nm->valuestring;
            }
        }

        if (!mr_enabled) continue;

        /* Determine camera name */
        char name[64];
        if (mr_name) {
            snprintf(name, sizeof(name), "%s", mr_name);
        } else if (cam->camera_id == 1) {
            snprintf(name, sizeof(name), "USB Camera");
        } else {
            snprintf(name, sizeof(name), "USB Camera %d", cam->camera_id);
        }

        /* Build stream/snapshot URLs with resolved camera IP */
        char stream_url[128], snap_url[128];
        snprintf(stream_url, sizeof(stream_url),
                 "http://%s:%d/stream", camera_ip, cam->streaming_port);
        snprintf(snap_url, sizeof(snap_url),
                 "http://%s:%d/snapshot", camera_ip, cam->streaming_port);

        int fps = cam->max_fps > 0 ? cam->max_fps : 20;

        int ret = moonraker_provision_camera(host, port, name,
                                              stream_url, snap_url, fps);
        if (ret == 0) {
            fprintf(stderr, "Moonraker: Provisioned '%s' (port %d)\n",
                    name, cam->streaming_port);
        } else {
            fprintf(stderr, "Moonraker: Failed to provision '%s'\n", name);
        }
    }

    if (cam_settings) cJSON_Delete(cam_settings);
}

/* ============================================================================
 * Multi-Camera API
 * ============================================================================ */

static void serve_cameras(ControlServer *srv, int fd) {
    cJSON *root = cJSON_CreateObject();
    cJSON *arr = cJSON_CreateArray();

    for (int i = 0; i < srv->num_cameras; i++) {
        CameraInfo *cam = &srv->cameras[i];
        cJSON *obj = cJSON_CreateObject();

        cJSON_AddNumberToObject(obj, "id", cam->camera_id);
        cJSON_AddStringToObject(obj, "device", cam->device);
        cJSON_AddStringToObject(obj, "name", cam->name);
        cJSON_AddStringToObject(obj, "unique_id", cam->unique_id);
        cJSON_AddStringToObject(obj, "usb_port", cam->usb_port);
        cJSON_AddNumberToObject(obj, "width", cam->width);
        cJSON_AddNumberToObject(obj, "height", cam->height);
        cJSON_AddNumberToObject(obj, "max_fps", cam->max_fps);
        cJSON_AddBoolToObject(obj, "has_mjpeg", cam->has_mjpeg);
        cJSON_AddBoolToObject(obj, "has_yuyv", cam->has_yuyv);
        cJSON_AddBoolToObject(obj, "is_primary", cam->is_primary);
        cJSON_AddBoolToObject(obj, "enabled", cam->enabled);
        cJSON_AddNumberToObject(obj, "streaming_port", cam->streaming_port);

        /* Supported resolutions */
        if (cam->num_resolutions > 0) {
            cJSON *res_arr = cJSON_CreateArray();
            for (int j = 0; j < cam->num_resolutions; j++) {
                char res_str[16];
                snprintf(res_str, sizeof(res_str), "%dx%d",
                         cam->resolutions[j].width, cam->resolutions[j].height);
                cJSON_AddItemToArray(res_arr, cJSON_CreateString(res_str));
            }
            cJSON_AddItemToObject(obj, "supported_resolutions", res_arr);
        }

        /* Check running status from managed processes */
        int running = 0;
        if (cam->camera_id == 1) {
            running = 1;  /* Primary is always running (we are the primary) */
        } else {
            for (int j = 0; j < srv->num_managed; j++) {
                if (srv->managed_procs[j].camera_id == cam->camera_id &&
                    srv->managed_procs[j].pid > 0) {
                    running = 1;
                    break;
                }
            }
        }
        cJSON_AddBoolToObject(obj, "running", running);

        /* Per-camera settings (secondary cameras only) */
        if (cam->camera_id > 1) {
            for (int j = 0; j < srv->num_managed; j++) {
                if (srv->managed_procs[j].camera_id == cam->camera_id) {
                    ManagedProcess *mp = &srv->managed_procs[j];

                    /* Error: user enabled but procmgr disabled it (restart limit) */
                    if (cam->enabled && !mp->enabled && mp->pid <= 0) {
                        cJSON_AddStringToObject(obj, "error",
                            "Camera crashed repeatedly (check resolution/USB bandwidth)");
                    }
                    int ow = mp->override_width > 0 ? mp->override_width : 640;
                    int oh = mp->override_height > 0 ? mp->override_height : 480;
                    char res[24];
                    snprintf(res, sizeof(res), "%dx%d", ow, oh);
                    cJSON_AddStringToObject(obj, "configured_resolution", res);
                    cJSON_AddStringToObject(obj, "capture_mode",
                                             mp->force_mjpeg ? "mjpeg" : "yuyv");
                    int cam_fps = mp->override_fps > 0 ? mp->override_fps :
                                  (srv->config->mjpeg_fps > 0 ? srv->config->mjpeg_fps : 10);
                    cJSON_AddNumberToObject(obj, "mjpeg_fps", cam_fps);
                    break;
                }
            }
        }

        cJSON_AddItemToArray(arr, obj);
    }

    cJSON_AddItemToObject(root, "cameras", arr);
    cJSON_AddNumberToObject(root, "active_camera_id", 1);

    send_json_response(fd, 200, root);
    cJSON_Delete(root);
}

static void serve_moonraker_cameras(ControlServer *srv, int fd) {
    cJSON *root = cJSON_CreateObject();
    cJSON *arr = cJSON_CreateArray();

    for (int i = 0; i < srv->num_cameras; i++) {
        CameraInfo *cam = &srv->cameras[i];

        cJSON *obj = cJSON_CreateObject();

        cJSON_AddNumberToObject(obj, "id", cam->camera_id);
        cJSON_AddStringToObject(obj, "name", cam->name);
        cJSON_AddStringToObject(obj, "unique_id", cam->unique_id);
        cJSON_AddNumberToObject(obj, "streaming_port", cam->streaming_port);
        cJSON_AddBoolToObject(obj, "is_primary", cam->is_primary);
        cJSON_AddBoolToObject(obj, "enabled", cam->enabled);

        cJSON_AddItemToArray(arr, obj);
    }

    cJSON_AddItemToObject(root, "cameras", arr);

    /* Parse cameras_json from config for per-camera moonraker settings */
    cJSON *settings = NULL;
    if (srv->config->cameras_json[0]) {
        settings = cJSON_Parse(srv->config->cameras_json);
    }
    if (!settings) settings = cJSON_CreateObject();
    cJSON_AddItemToObject(root, "settings", settings);

    send_json_response(fd, 200, root);
    cJSON_Delete(root);
}

static void handle_moonraker_cameras_post(ControlServer *srv, int fd,
                                            const char *body) {
    /* Body is JSON: {"settings": {...}, "moonraker_host": "...", ...} */
    cJSON *json = cJSON_Parse(body);
    if (!json) {
        send_json_error(fd, 400, "Invalid JSON");
        return;
    }

    /* Save moonraker connection settings if provided */
    cJSON *mr_host = cJSON_GetObjectItem(json, "moonraker_host");
    if (mr_host && mr_host->valuestring && mr_host->valuestring[0])
        safe_strcpy(srv->config->moonraker_host,
                    sizeof(srv->config->moonraker_host), mr_host->valuestring);

    cJSON *mr_port = cJSON_GetObjectItem(json, "moonraker_port");
    if (mr_port) {
        int port = cJSON_IsNumber(mr_port) ? mr_port->valueint : 0;
        if (mr_port->valuestring) port = atoi(mr_port->valuestring);
        if (port > 0 && port <= 65535) srv->config->moonraker_port = port;
    }

    cJSON *mr_cam_ip = cJSON_GetObjectItem(json, "moonraker_camera_ip");
    if (mr_cam_ip && mr_cam_ip->valuestring)
        safe_strcpy(srv->config->moonraker_camera_ip,
                    sizeof(srv->config->moonraker_camera_ip), mr_cam_ip->valuestring);

    cJSON *settings = cJSON_GetObjectItem(json, "settings");
    if (!settings) {
        /* No camera settings — just save connection settings */
        config_save(srv->config, srv->config->config_file);
        control_server_provision_moonraker(srv);
        cJSON *resp = cJSON_CreateObject();
        cJSON_AddStringToObject(resp, "status", "ok");
        send_json_response(fd, 200, resp);
        cJSON_Delete(resp);
        cJSON_Delete(json);
        return;
    }

    /* Merge incoming settings into existing cameras_json in config */
    cJSON *existing = NULL;
    if (srv->config->cameras_json[0]) {
        existing = cJSON_Parse(srv->config->cameras_json);
    }
    if (!existing) existing = cJSON_CreateObject();

    /* Iterate incoming settings and merge */
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, settings) {
        const char *unique_id = item->string;
        if (!unique_id) continue;

        /* Get or create entry for this camera */
        cJSON *cam_entry = cJSON_GetObjectItem(existing, unique_id);
        if (!cam_entry) {
            cam_entry = cJSON_CreateObject();
            cJSON_AddItemToObject(existing, unique_id, cam_entry);
        }

        /* Merge moonraker fields */
        cJSON *field;
        field = cJSON_GetObjectItem(item, "moonraker_name");
        if (field && field->valuestring) {
            cJSON_DeleteItemFromObject(cam_entry, "moonraker_name");
            cJSON_AddStringToObject(cam_entry, "moonraker_name", field->valuestring);
        }
        field = cJSON_GetObjectItem(item, "moonraker_enabled");
        if (field) {
            cJSON_DeleteItemFromObject(cam_entry, "moonraker_enabled");
            cJSON_AddBoolToObject(cam_entry, "moonraker_enabled",
                                   cJSON_IsTrue(field));
        }
        field = cJSON_GetObjectItem(item, "moonraker_default");
        if (field) {
            cJSON_DeleteItemFromObject(cam_entry, "moonraker_default");
            cJSON_AddBoolToObject(cam_entry, "moonraker_default",
                                   cJSON_IsTrue(field));
        }
    }

    /* Serialize back to config */
    char *json_str = cJSON_PrintUnformatted(existing);
    if (json_str) {
        strncpy(srv->config->cameras_json, json_str,
                sizeof(srv->config->cameras_json) - 1);
        free(json_str);
    }
    cJSON_Delete(existing);
    cJSON_Delete(json);

    /* Save config */
    config_save(srv->config, srv->config->config_file);

    /* Provision cameras to Moonraker */
    control_server_provision_moonraker(srv);

    /* Return success */
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "status", "ok");
    send_json_response(fd, 200, resp);
    cJSON_Delete(resp);
}

/* Load per-camera encoder overrides from cameras_json config.
 * Looks up by camera unique_id, loads resolution/mode/fps overrides. */
void control_server_load_camera_overrides(ManagedProcess *proc,
                                           const CameraInfo *cam,
                                           const AppConfig *cfg) {
    if (!proc || !cam || !cfg || !cfg->cameras_json[0]) return;
    if (!cam->unique_id[0]) return;

    cJSON *root = cJSON_Parse(cfg->cameras_json);
    if (!root) return;

    cJSON *entry = cJSON_GetObjectItem(root, cam->unique_id);
    if (entry) {
        /* Resolution */
        cJSON *res = cJSON_GetObjectItem(entry, "resolution");
        if (res && res->valuestring) {
            int w = 0, h = 0;
            if (sscanf(res->valuestring, "%dx%d", &w, &h) == 2 && w > 0 && h > 0) {
                proc->override_width = w;
                proc->override_height = h;
            }
        }
        /* Capture mode */
        cJSON *mode = cJSON_GetObjectItem(entry, "capture_mode");
        if (mode && mode->valuestring) {
            proc->force_mjpeg = (strcmp(mode->valuestring, "mjpeg") == 0) ? 1 : 0;
        }
        /* FPS */
        cJSON *fps = cJSON_GetObjectItem(entry, "mjpeg_fps");
        if (fps && cJSON_IsNumber(fps) && fps->valueint >= 2 && fps->valueint <= 30) {
            proc->override_fps = fps->valueint;
        }
    }

    cJSON_Delete(root);
}

static void handle_camera_enable(ControlServer *srv, int fd,
                                  const char *body) {
    FormParam params[8];
    int np = parse_form_body(body, params, 8);
    const char *id_str = form_get(params, np, "id");

    if (!id_str) {
        send_json_error(fd, 400, "Missing camera id");
        return;
    }
    int cam_id = atoi(id_str);
    if (cam_id < 2 || cam_id > CAMERA_MAX) {
        send_json_error(fd, 400, "Invalid camera id (must be 2-4)");
        return;
    }

    /* Find camera */
    CameraInfo *cam = NULL;
    for (int i = 0; i < srv->num_cameras; i++) {
        if (srv->cameras[i].camera_id == cam_id) {
            cam = &srv->cameras[i];
            break;
        }
    }
    if (!cam) {
        send_json_error(fd, 404, "Camera not found");
        return;
    }

    cam->enabled = 1;

    /* Persist enabled state to config */
    if (cam->unique_id[0]) {
        cJSON *existing = srv->config->cameras_json[0] ?
            cJSON_Parse(srv->config->cameras_json) : NULL;
        if (!existing) existing = cJSON_CreateObject();
        cJSON *entry = cJSON_GetObjectItem(existing, cam->unique_id);
        if (!entry) {
            entry = cJSON_CreateObject();
            cJSON_AddItemToObject(existing, cam->unique_id, entry);
        }
        cJSON_DeleteItemFromObject(entry, "enabled");
        cJSON_AddBoolToObject(entry, "enabled", 1);
        char *json_str = cJSON_PrintUnformatted(existing);
        if (json_str) {
            strncpy(srv->config->cameras_json, json_str,
                    sizeof(srv->config->cameras_json) - 1);
            free(json_str);
        }
        cJSON_Delete(existing);
        config_save(srv->config, srv->config->config_file);
    }

    /* Start the process if not already running */
    char binary_path[256];
    ssize_t len = readlink("/proc/self/exe", binary_path,
                            sizeof(binary_path) - 1);
    if (len > 0) {
        binary_path[len] = '\0';
        /* Find or create managed process slot */
        ManagedProcess *proc = NULL;
        for (int i = 0; i < srv->num_managed; i++) {
            if (srv->managed_procs[i].camera_id == cam_id) {
                proc = &srv->managed_procs[i];
                break;
            }
        }
        if (!proc && srv->num_managed < CAMERA_MAX) {
            proc = &srv->managed_procs[srv->num_managed];
            srv->num_managed++;
            /* Load saved per-camera overrides from config */
            control_server_load_camera_overrides(proc, cam, srv->config);
        }
        if (proc && proc->pid <= 0) {
            proc->enabled = 1;
            procmgr_start_camera(proc, cam, srv->config, binary_path);
        }
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "status", "ok");
    cJSON_AddNumberToObject(root, "camera_id", cam_id);
    cJSON_AddStringToObject(root, "action", "enabled");
    send_json_response(fd, 200, root);
    cJSON_Delete(root);
}

static void handle_camera_disable(ControlServer *srv, int fd,
                                   const char *body) {
    FormParam params[8];
    int np = parse_form_body(body, params, 8);
    const char *id_str = form_get(params, np, "id");

    if (!id_str) {
        send_json_error(fd, 400, "Missing camera id");
        return;
    }
    int cam_id = atoi(id_str);
    if (cam_id < 2 || cam_id > CAMERA_MAX) {
        send_json_error(fd, 400, "Cannot disable primary camera");
        return;
    }

    /* Find camera and disable */
    CameraInfo *cam = NULL;
    for (int i = 0; i < srv->num_cameras; i++) {
        if (srv->cameras[i].camera_id == cam_id) {
            srv->cameras[i].enabled = 0;
            cam = &srv->cameras[i];
            break;
        }
    }

    /* Persist disabled state to config */
    if (cam && cam->unique_id[0]) {
        cJSON *existing = srv->config->cameras_json[0] ?
            cJSON_Parse(srv->config->cameras_json) : NULL;
        if (!existing) existing = cJSON_CreateObject();
        cJSON *entry = cJSON_GetObjectItem(existing, cam->unique_id);
        if (!entry) {
            entry = cJSON_CreateObject();
            cJSON_AddItemToObject(existing, cam->unique_id, entry);
        }
        cJSON_DeleteItemFromObject(entry, "enabled");
        cJSON_AddBoolToObject(entry, "enabled", 0);
        char *json_str = cJSON_PrintUnformatted(existing);
        if (json_str) {
            strncpy(srv->config->cameras_json, json_str,
                    sizeof(srv->config->cameras_json) - 1);
            free(json_str);
        }
        cJSON_Delete(existing);
        config_save(srv->config, srv->config->config_file);
    }

    /* Stop the process */
    for (int i = 0; i < srv->num_managed; i++) {
        if (srv->managed_procs[i].camera_id == cam_id) {
            srv->managed_procs[i].enabled = 0;
            procmgr_stop_camera(&srv->managed_procs[i]);
            break;
        }
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "status", "ok");
    cJSON_AddNumberToObject(root, "camera_id", cam_id);
    cJSON_AddStringToObject(root, "action", "disabled");
    send_json_response(fd, 200, root);
    cJSON_Delete(root);
}

static void handle_camera_settings(ControlServer *srv, int fd,
                                    const char *body) {
    FormParam params[16];
    int np = parse_form_body(body, params, 16);
    const char *id_str = form_get(params, np, "id");

    if (!id_str) {
        send_json_error(fd, 400, "Missing camera id");
        return;
    }
    int cam_id = atoi(id_str);
    if (cam_id < 2 || cam_id > CAMERA_MAX) {
        send_json_error(fd, 400, "Invalid camera id (must be 2-4)");
        return;
    }

    /* Find managed process */
    ManagedProcess *proc = NULL;
    for (int i = 0; i < srv->num_managed; i++) {
        if (srv->managed_procs[i].camera_id == cam_id) {
            proc = &srv->managed_procs[i];
            break;
        }
    }

    /* Parse resolution (e.g. "800x600") */
    const char *res = form_get(params, np, "resolution");
    int new_width = 0, new_height = 0;
    if (res && sscanf(res, "%dx%d", &new_width, &new_height) == 2) {
        if (proc) {
            proc->override_width = new_width;
            proc->override_height = new_height;
        }
    }

    /* Parse capture mode (yuyv or mjpeg) */
    const char *mode = form_get(params, np, "mode");
    if (mode && proc) {
        proc->force_mjpeg = (strcmp(mode, "mjpeg") == 0) ? 1 : 0;
    }

    /* Parse per-camera FPS */
    const char *fps_str = form_get(params, np, "mjpeg_fps");
    int new_fps = 0;
    if (fps_str) {
        new_fps = atoi(fps_str);
        if (new_fps >= 2 && new_fps <= 30 && proc) {
            proc->override_fps = new_fps;
        }
    }

    /* Persist per-camera settings to cameras_json */
    if (proc) {
        /* Find camera unique_id */
        const char *unique_id = NULL;
        for (int i = 0; i < srv->num_cameras; i++) {
            if (srv->cameras[i].camera_id == cam_id) {
                unique_id = srv->cameras[i].unique_id;
                break;
            }
        }
        if (unique_id && unique_id[0]) {
            cJSON *existing = NULL;
            if (srv->config->cameras_json[0])
                existing = cJSON_Parse(srv->config->cameras_json);
            if (!existing) existing = cJSON_CreateObject();

            cJSON *cam_entry = cJSON_GetObjectItem(existing, unique_id);
            if (!cam_entry) {
                cam_entry = cJSON_CreateObject();
                cJSON_AddItemToObject(existing, unique_id, cam_entry);
            }

            /* Save encoder settings */
            if (proc->override_width > 0 && proc->override_height > 0) {
                char res_buf[24];
                snprintf(res_buf, sizeof(res_buf), "%dx%d",
                         proc->override_width, proc->override_height);
                cJSON_DeleteItemFromObject(cam_entry, "resolution");
                cJSON_AddStringToObject(cam_entry, "resolution", res_buf);
            }
            cJSON_DeleteItemFromObject(cam_entry, "capture_mode");
            cJSON_AddStringToObject(cam_entry, "capture_mode",
                                     proc->force_mjpeg ? "mjpeg" : "yuyv");
            if (proc->override_fps > 0) {
                cJSON_DeleteItemFromObject(cam_entry, "mjpeg_fps");
                cJSON_AddNumberToObject(cam_entry, "mjpeg_fps", proc->override_fps);
            }

            char *json_str = cJSON_PrintUnformatted(existing);
            if (json_str) {
                strncpy(srv->config->cameras_json, json_str,
                        sizeof(srv->config->cameras_json) - 1);
                free(json_str);
            }
            cJSON_Delete(existing);
            config_save(srv->config, srv->config->config_file);
        }
    }

    /* If camera is running, restart it with new settings */
    int restarted = 0;
    if (proc && proc->pid > 0) {
        CameraInfo *cam = NULL;
        for (int i = 0; i < srv->num_cameras; i++) {
            if (srv->cameras[i].camera_id == cam_id) {
                cam = &srv->cameras[i];
                break;
            }
        }
        if (cam) {
            procmgr_stop_camera(proc);
            char binary_path[256];
            ssize_t len = readlink("/proc/self/exe", binary_path,
                                    sizeof(binary_path) - 1);
            if (len > 0) {
                binary_path[len] = '\0';
                procmgr_start_camera(proc, cam, srv->config, binary_path);
                restarted = 1;
            }
        }
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "status", "ok");
    cJSON_AddNumberToObject(root, "camera_id", cam_id);
    if (new_width > 0) {
        char res_str[16];
        snprintf(res_str, sizeof(res_str), "%dx%d", new_width, new_height);
        cJSON_AddStringToObject(root, "resolution", res_str);
    }
    if (mode) cJSON_AddStringToObject(root, "capture_mode", mode);
    if (new_fps > 0) cJSON_AddNumberToObject(root, "mjpeg_fps", new_fps);
    cJSON_AddBoolToObject(root, "restarted", restarted);
    send_json_response(fd, 200, root);
    cJSON_Delete(root);
}

/* ============================================================================
 * HTTP Request Handler
 * ============================================================================ */

static void handle_client(ControlServer *srv, int client_fd,
                           struct sockaddr_in *client_addr) {
    /* Read request */
    char buf[CTRL_MAX_POST_BODY + 4096];
    ssize_t nread = recv(client_fd, buf, sizeof(buf) - 1, 0);
    if (nread <= 0) {
        close(client_fd);
        return;
    }
    buf[nread] = '\0';

    /* Parse request line */
    char *line_end = strstr(buf, "\r\n");
    if (!line_end) {
        close(client_fd);
        return;
    }

    char method[8] = {0};
    char full_path[512] = {0};
    sscanf(buf, "%7s %511s", method, full_path);

    /* Split path and query string */
    char path[512];
    char query_string[512] = {0};
    char *qmark = strchr(full_path, '?');
    if (qmark) {
        size_t path_len = qmark - full_path;
        if (path_len >= sizeof(path)) path_len = sizeof(path) - 1;
        memcpy(path, full_path, path_len);
        path[path_len] = '\0';
        snprintf(query_string, sizeof(query_string), "%s", qmark + 1);
    } else {
        snprintf(path, sizeof(path), "%s", full_path);
    }

    /* Parse query params */
    FormParam query_params[16];
    int nquery = 0;
    if (query_string[0]) {
        nquery = parse_form_body(query_string, query_params, 16);
    }

    /* Read POST body if present */
    char *post_body = NULL;
    if (strcmp(method, "POST") == 0 || strcmp(method, "DELETE") == 0) {
        /* Find Content-Length */
        const char *cl_hdr = strstr(buf, "Content-Length:");
        if (!cl_hdr) cl_hdr = strstr(buf, "content-length:");
        int content_length = 0;
        if (cl_hdr) {
            content_length = atoi(cl_hdr + 15);
        }

        /* Find body start (after \r\n\r\n) */
        char *body_start = strstr(buf, "\r\n\r\n");
        if (body_start) {
            body_start += 4;
            int body_in_buf = nread - (body_start - buf);

            if (content_length > 0 && content_length < CTRL_MAX_POST_BODY) {
                post_body = malloc(content_length + 1);
                if (post_body) {
                    /* Copy what we already have */
                    int to_copy = body_in_buf < content_length ? body_in_buf : content_length;
                    memcpy(post_body, body_start, to_copy);

                    /* Read remaining if needed */
                    int remaining = content_length - to_copy;
                    while (remaining > 0) {
                        ssize_t n = recv(client_fd, post_body + to_copy, remaining, 0);
                        if (n <= 0) break;
                        to_copy += n;
                        remaining -= n;
                    }
                    post_body[to_copy] = '\0';
                }
            } else if (body_in_buf > 0) {
                post_body = strndup(body_start, body_in_buf);
            }
        }
    }

    /* Route the request */
    int is_get = strcmp(method, "GET") == 0;
    int is_post = strcmp(method, "POST") == 0;
    int is_delete = strcmp(method, "DELETE") == 0;

    if (is_get && strcmp(path, "/") == 0) {
        serve_homepage(srv, client_fd);
    }
    else if (strcmp(path, "/control") == 0) {
        if (is_post) {
            handle_control_post(srv, client_fd, post_body ? post_body : "");
        } else {
            serve_control_page(srv, client_fd);
        }
    }
    else if (is_get && strcmp(path, "/status") == 0) {
        serve_status(srv, client_fd);
    }
    else if (is_get && strcmp(path, "/api/stats") == 0) {
        serve_api_stats(srv, client_fd);
    }
    else if (is_get && strcmp(path, "/api/config") == 0) {
        serve_api_config(srv, client_fd);
    }
    else if (is_get && strcmp(path, "/api/camera/controls") == 0) {
        const char *cam_id_str = form_get(query_params, nquery, "camera_id");
        int cam_id = cam_id_str ? atoi(cam_id_str) : 1;
        serve_camera_controls(srv, client_fd, cam_id);
    }
    else if (is_post && strcmp(path, "/api/camera/set") == 0) {
        handle_camera_set(srv, client_fd, post_body ? post_body : "");
    }
    else if (is_post && strcmp(path, "/api/touch") == 0) {
        handle_touch(srv, client_fd, post_body ? post_body : "");
    }
    else if (is_get && strcmp(path, "/api/led/on") == 0) {
        handle_led(srv, client_fd, 1);
    }
    else if (is_get && strcmp(path, "/api/led/off") == 0) {
        handle_led(srv, client_fd, 0);
    }
    else if (is_get && strcmp(path, "/api/restart") == 0) {
        handle_restart(srv, client_fd);
    }
    /* Timelapse routes */
    else if (is_get && strcmp(path, "/timelapse") == 0) {
        serve_timelapse_page(srv, client_fd);
    }
    else if (is_get && strcmp(path, "/api/timelapse/list") == 0) {
        const char *storage = form_get(query_params, nquery, "storage");
        serve_timelapse_list(srv, client_fd, storage ? storage : "internal");
    }
    else if (is_get && strncmp(path, "/api/timelapse/thumb/", 21) == 0) {
        const char *name = path + 21;
        const char *storage = form_get(query_params, nquery, "storage");
        char decoded_name[256];
        strncpy(decoded_name, name, sizeof(decoded_name) - 1);
        url_decode(decoded_name);
        serve_timelapse_thumb(srv, client_fd, decoded_name, storage ? storage : "internal");
    }
    else if (is_get && strncmp(path, "/api/timelapse/video/", 21) == 0) {
        const char *name = path + 21;
        const char *storage = form_get(query_params, nquery, "storage");
        char decoded_name[256];
        strncpy(decoded_name, name, sizeof(decoded_name) - 1);
        url_decode(decoded_name);
        serve_timelapse_video(srv, client_fd, decoded_name, storage ? storage : "internal", buf);
    }
    else if (is_delete && strncmp(path, "/api/timelapse/delete/", 22) == 0) {
        const char *name = path + 22;
        const char *storage = form_get(query_params, nquery, "storage");
        char decoded_name[256];
        strncpy(decoded_name, name, sizeof(decoded_name) - 1);
        url_decode(decoded_name);
        handle_timelapse_delete(srv, client_fd, decoded_name, storage ? storage : "internal");
    }
    else if (is_get && strcmp(path, "/api/timelapse/storage") == 0) {
        serve_timelapse_storage(srv, client_fd);
    }
    else if (is_get && strcmp(path, "/api/timelapse/browse") == 0) {
        const char *browse_path = form_get(query_params, nquery, "path");
        serve_timelapse_browse(srv, client_fd, browse_path ? browse_path : "/mnt/udisk");
    }
    else if (is_post && strcmp(path, "/api/timelapse/mkdir") == 0) {
        handle_timelapse_mkdir(srv, client_fd, post_body ? post_body : "");
    }
    else if (is_get && strcmp(path, "/api/timelapse/moonraker") == 0) {
        serve_timelapse_moonraker_status(srv, client_fd);
    }
    else if (is_post && strcmp(path, "/api/timelapse/settings") == 0) {
        handle_timelapse_settings(srv, client_fd, post_body ? post_body : "");
    }
    /* Multi-camera routes */
    else if (is_get && strcmp(path, "/api/cameras") == 0) {
        serve_cameras(srv, client_fd);
    }
    else if (is_get && strcmp(path, "/api/network/interfaces") == 0) {
        serve_network_interfaces(srv, client_fd);
    }
    else if (is_get && strcmp(path, "/api/moonraker/cameras") == 0) {
        serve_moonraker_cameras(srv, client_fd);
    }
    else if (is_post && strcmp(path, "/api/moonraker/cameras") == 0) {
        handle_moonraker_cameras_post(srv, client_fd, post_body ? post_body : "");
    }
    else if (is_post && strcmp(path, "/api/camera/enable") == 0) {
        handle_camera_enable(srv, client_fd, post_body ? post_body : "");
    }
    else if (is_post && strcmp(path, "/api/camera/disable") == 0) {
        handle_camera_disable(srv, client_fd, post_body ? post_body : "");
    }
    else if (is_post && strcmp(path, "/api/camera/settings") == 0) {
        handle_camera_settings(srv, client_fd, post_body ? post_body : "");
    }
    /* ACProxyCam routes */
    else if (strcmp(path, "/api/acproxycam/flv") == 0) {
        if (is_post) {
            handle_acproxycam_flv_announce(srv, client_fd,
                                           post_body ? post_body : "", client_addr);
        } else {
            serve_acproxycam_flv_status(srv, client_fd);
        }
    }
    /* Fault detection routes */
    else if (is_get && strcmp(path, "/api/fault_detect/models") == 0) {
        serve_fault_detect_models(srv, client_fd);
    }
    else if (is_post && strcmp(path, "/api/fault_detect/settings") == 0) {
        handle_fault_detect_settings(srv, client_fd, post_body ? post_body : "");
    }
    /* Streaming redirects */
    else if (is_get && strcmp(path, "/stream") == 0) {
        char url[128];
        snprintf(url, sizeof(url), "http://%s:%d/stream",
                "\" + location.hostname + \"", srv->config->streaming_port);
        /* Simple text redirect hint */
        char body_buf[256];
        int blen = snprintf(body_buf, sizeof(body_buf),
            "Stream available at streaming port %d", srv->config->streaming_port);
        send_http_response(client_fd, 200, "text/plain", body_buf, blen, NULL);
    }
    else if (is_get && (strcmp(path, "/snapshot") == 0 || strcmp(path, "/snap") == 0)) {
        char url[128];
        snprintf(url, sizeof(url), "/snapshot");
        /* Redirect to streaming port */
        char redirect_url[128];
        snprintf(redirect_url, sizeof(redirect_url),
                "http://\" + location.hostname + \":%d/snapshot",
                srv->config->streaming_port);
        send_http_response(client_fd, 200, "text/plain",
                          "Use streaming port for snapshots", 31, NULL);
    }
    else {
        send_404(client_fd);
    }

    free(post_body);
    close(client_fd);
}

/* ============================================================================
 * Server Thread
 * ============================================================================ */

static void *control_server_thread(void *arg) {
    ControlServer *srv = (ControlServer *)arg;

    while (srv->running) {
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(srv->listen_fd, &read_fds);

        struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
        int ret = select(srv->listen_fd + 1, &read_fds, NULL, NULL, &tv);

        if (ret > 0 && FD_ISSET(srv->listen_fd, &read_fds)) {
            struct sockaddr_in client_addr;
            socklen_t addr_len = sizeof(client_addr);
            int client_fd = accept(srv->listen_fd, (struct sockaddr *)&client_addr, &addr_len);

            if (client_fd >= 0) {
                /* Set recv timeout */
                struct timeval to = { .tv_sec = 30 };
                setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &to, sizeof(to));

                /* Disable Nagle */
                int opt = 1;
                setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));

                /* Handle request synchronously (control server is low-traffic) */
                handle_client(srv, client_fd, &client_addr);
            }
        }

        /* Periodic: update CPU stats (every 2s) */
        static time_t last_cpu_update = 0;
        time_t now = time(NULL);
        if (now - last_cpu_update >= 2) {
            cpu_monitor_update(&srv->cpu_monitor);
            read_encoder_stats(srv);
            last_cpu_update = now;
        }

        /* Periodic: IP change detection + WiFi optimization (every 30s) */
        static time_t last_net_check = 0;
        static char last_ip[64] = {0};
        static int route_fixed = 0;
        static int wifi_optimized = 0;
        if (now - last_net_check >= 30) {
            last_net_check = now;

            /* Check for IP changes */
            char current_ip[64] = {0};
            if (get_ip_address(current_ip, sizeof(current_ip)) == 0) {
                if (last_ip[0] && strcmp(current_ip, last_ip) != 0) {
                    fprintf(stderr, "Network: IP changed %s -> %s\n", last_ip, current_ip);
                    control_server_provision_moonraker(srv);
                    route_fixed = 0;  /* Re-check routes on IP change */
                }
                snprintf(last_ip, sizeof(last_ip), "%s", current_ip);
            }

            /* Fix WiFi route priority (retry until successful) */
            if (!route_fixed) {
                route_fixed = wifi_fix_route_priority();
            }

            /* Optimize WiFi driver (one-shot) */
            if (!wifi_optimized) {
                wifi_optimized = wifi_optimize_driver();
            }
        }
    }

    return NULL;
}

/* ============================================================================
 * Public API
 * ============================================================================ */

int control_server_start(AppConfig *cfg, int port, const char *template_dir) {
    ControlServer *srv = &g_control_server;
    memset(srv, 0, sizeof(*srv));

    srv->config = cfg;
    srv->port = (port > 0) ? port : cfg->control_port;
    if (srv->port <= 0) srv->port = 8081;

    /* Template directory */
    if (template_dir) {
        strncpy(srv->template_dir, template_dir, sizeof(srv->template_dir) - 1);
    } else {
        strncpy(srv->template_dir, CTRL_TEMPLATE_DIR_DEFAULT, sizeof(srv->template_dir) - 1);
    }

    /* Generate session ID */
    snprintf(srv->session_id, sizeof(srv->session_id), "%08x%08x",
             (unsigned int)time(NULL), (unsigned int)getpid());

    /* Read streamer version from VERSION file in template directory */
    safe_strcpy(srv->streamer_version, sizeof(srv->streamer_version), "");
    {
        char vpath[300];
        snprintf(vpath, sizeof(vpath), "%s/VERSION", srv->template_dir);
        FILE *vf = fopen(vpath, "r");
        if (vf) {
            char vbuf[16] = {0};
            if (fgets(vbuf, sizeof(vbuf), vf)) {
                /* Trim trailing newline */
                char *nl = strchr(vbuf, '\n');
                if (nl) *nl = '\0';
                safe_strcpy(srv->streamer_version, sizeof(srv->streamer_version), vbuf);
            }
            fclose(vf);
        }
    }

    /* Initialize CPU monitor */
    cpu_monitor_init(&srv->cpu_monitor);

    /* Create listen socket */
    srv->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (srv->listen_fd < 0) {
        fprintf(stderr, "Control: socket() failed: %s\n", strerror(errno));
        return -1;
    }

    int opt = 1;
    setsockopt(srv->listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = INADDR_ANY,
        .sin_port = htons(srv->port),
    };

    if (bind(srv->listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "Control: bind(%d) failed: %s\n", srv->port, strerror(errno));
        close(srv->listen_fd);
        return -1;
    }

    if (listen(srv->listen_fd, 8) < 0) {
        fprintf(stderr, "Control: listen() failed: %s\n", strerror(errno));
        close(srv->listen_fd);
        return -1;
    }

    srv->running = 1;

    if (pthread_create(&srv->thread, NULL, control_server_thread, srv) != 0) {
        fprintf(stderr, "Control: Failed to create thread\n");
        close(srv->listen_fd);
        return -1;
    }
    pthread_setname_np(srv->thread, "control_srv");

    fprintf(stderr, "Control: Server listening on port %d\n", srv->port);
    return 0;
}

void control_server_stop(void) {
    ControlServer *srv = &g_control_server;
    if (!srv->running) return;

    srv->running = 0;
    pthread_join(srv->thread, NULL);

    if (srv->listen_fd > 0) {
        close(srv->listen_fd);
        srv->listen_fd = 0;
    }

    fprintf(stderr, "Control: Server stopped\n");
}

void control_server_update_stats(float mjpeg_fps, float h264_fps,
                                  int mjpeg_clients, int flv_clients,
                                  int display_clients, int max_camera_fps,
                                  int skip_ratio) {
    ControlServer *srv = &g_control_server;
    srv->encoder_mjpeg_fps = mjpeg_fps;
    srv->encoder_h264_fps = h264_fps;
    srv->encoder_mjpeg_clients = mjpeg_clients;
    srv->encoder_flv_clients = flv_clients;
    srv->encoder_display_clients = display_clients;
    srv->max_camera_fps = max_camera_fps;
    srv->runtime_skip_ratio = skip_ratio;
}

void control_server_set_config_callback(void (*cb)(AppConfig *cfg)) {
    g_control_server.on_config_changed = cb;
}

void control_server_set_restart_callback(void (*cb)(void)) {
    g_control_server.on_restart = cb;
}

void control_server_set_cameras(CameraInfo *cameras, int num_cameras,
                                 ManagedProcess *procs, int num_managed) {
    g_control_server.cameras = cameras;
    g_control_server.num_cameras = num_cameras;
    g_control_server.managed_procs = procs;
    g_control_server.num_managed = num_managed;
}

void control_server_set_moonraker(struct MoonrakerClient *mc) {
    g_moonraker_client = mc;
}
