/*
 * Moonraker WebSocket Client Implementation
 *
 * Minimal RFC 6455 WebSocket client that subscribes to Moonraker print
 * events and drives timelapse recording via direct function calls.
 */

#include "moonraker_client.h"
#include "timelapse.h"
#include "cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

/* External verbose flag from rkmpi_enc.c */
extern int g_verbose;

/* Buffer sizes */
#define WS_RECV_BUF     8192
#define WS_SEND_BUF     2048
#define RECONNECT_DELAY 5       /* seconds between reconnect attempts */
#define CONNECT_TIMEOUT 10      /* seconds for TCP connect */
#define RECV_TIMEOUT_MS 30000   /* select() timeout for recv loop */

/* WebSocket opcodes */
#define WS_OP_TEXT   0x01
#define WS_OP_CLOSE  0x08
#define WS_OP_PING   0x09
#define WS_OP_PONG   0x0A

/* ============================================================================
 * Logging
 * ============================================================================ */

static void mr_log(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    fprintf(stderr, "Moonraker: ");
    vfprintf(stderr, fmt, args);
    va_end(args);
    fflush(stderr);
}

static void mr_debug(const char *fmt, ...) {
    if (!g_verbose) return;
    va_list args;
    va_start(args, fmt);
    fprintf(stderr, "Moonraker: ");
    vfprintf(stderr, fmt, args);
    va_end(args);
    fflush(stderr);
}

/* ============================================================================
 * TCP Connection
 * ============================================================================ */

static int tcp_connect(const char *host, int port, int timeout_sec) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
        close(fd);
        return -1;
    }

    /* Non-blocking connect with timeout */
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    int ret = connect(fd, (struct sockaddr *)&addr, sizeof(addr));
    if (ret < 0 && errno != EINPROGRESS) {
        close(fd);
        return -1;
    }

    if (ret < 0) {
        fd_set wfds;
        FD_ZERO(&wfds);
        FD_SET(fd, &wfds);
        struct timeval tv = { .tv_sec = timeout_sec };
        if (select(fd + 1, NULL, &wfds, NULL, &tv) <= 0) {
            close(fd);
            return -1;
        }
        int err = 0;
        socklen_t len = sizeof(err);
        getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len);
        if (err != 0) {
            close(fd);
            return -1;
        }
    }

    /* Restore blocking mode */
    fcntl(fd, F_SETFL, flags);

    /* Enable TCP keepalive */
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));

    /* Disable Nagle for low latency */
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    return fd;
}

/* ============================================================================
 * WebSocket Layer (RFC 6455)
 * ============================================================================ */

/* Generate random masking key */
static void ws_random_mask(uint8_t mask[4]) {
    /* Use /dev/urandom for randomness, fallback to time-based */
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd >= 0) {
        if (read(fd, mask, 4) == 4) {
            close(fd);
            return;
        }
        close(fd);
    }
    /* Fallback */
    uint32_t r = (uint32_t)time(NULL) ^ (uint32_t)getpid();
    memcpy(mask, &r, 4);
}

/*
 * WebSocket handshake: HTTP upgrade to /websocket
 * Returns 0 on success, -1 on error.
 */
static int ws_handshake(MoonrakerClient *mc) {
    /* Fixed WebSocket key (base64-encoded 16 bytes) — we don't validate
     * the server's Sec-WebSocket-Accept since this is local-only */
    static const char *ws_key = "dGhlIHNhbXBsZSBub25jZQ==";

    char request[512];
    int len = snprintf(request, sizeof(request),
        "GET /websocket HTTP/1.1\r\n"
        "Host: %s:%d\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: %s\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "\r\n",
        mc->host, mc->port, ws_key);

    if (send(mc->fd, request, len, MSG_NOSIGNAL) != len)
        return -1;

    /* Read HTTP response (must contain "101") */
    char response[1024];
    int total = 0;
    struct timeval tv = { .tv_sec = 5 };
    setsockopt(mc->fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    while (total < (int)sizeof(response) - 1) {
        ssize_t n = recv(mc->fd, response + total, sizeof(response) - 1 - total, 0);
        if (n <= 0) return -1;
        total += n;
        response[total] = '\0';
        if (strstr(response, "\r\n\r\n")) break;
    }

    if (!strstr(response, "101"))
        return -1;

    /* Clear recv timeout (we use select() in the main loop) */
    tv.tv_sec = 0;
    tv.tv_usec = 0;
    setsockopt(mc->fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    return 0;
}

/*
 * Send a masked WebSocket text frame.
 * Returns 0 on success, -1 on error.
 */
static int ws_send_text(MoonrakerClient *mc, const char *data, size_t len) {
    /* Frame: FIN + opcode (2 bytes) + mask flag + length + mask key + masked payload */
    uint8_t header[14];
    int hlen = 0;

    header[0] = 0x80 | WS_OP_TEXT;  /* FIN=1, opcode=text */

    if (len < 126) {
        header[1] = 0x80 | (uint8_t)len;  /* MASK=1 */
        hlen = 2;
    } else if (len < 65536) {
        header[1] = 0x80 | 126;
        header[2] = (len >> 8) & 0xFF;
        header[3] = len & 0xFF;
        hlen = 4;
    } else {
        /* Messages > 64K not expected for our JSON-RPC */
        return -1;
    }

    /* Masking key */
    uint8_t mask[4];
    ws_random_mask(mask);
    memcpy(header + hlen, mask, 4);
    hlen += 4;

    /* Build masked payload */
    uint8_t *frame = malloc(hlen + len);
    if (!frame) return -1;

    memcpy(frame, header, hlen);
    for (size_t i = 0; i < len; i++) {
        frame[hlen + i] = data[i] ^ mask[i & 3];
    }

    ssize_t sent = 0;
    ssize_t total = hlen + len;
    while (sent < total) {
        ssize_t n = send(mc->fd, frame + sent, total - sent, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                usleep(1000);
                continue;
            }
            free(frame);
            return -1;
        }
        sent += n;
    }

    free(frame);
    return 0;
}

/*
 * Send a WebSocket pong frame (masked, as required for client).
 */
static int ws_send_pong(MoonrakerClient *mc, const uint8_t *payload, size_t len) {
    uint8_t header[6 + 125];  /* Max pong payload is 125 bytes */
    if (len > 125) len = 125;

    header[0] = 0x80 | WS_OP_PONG;
    header[1] = 0x80 | (uint8_t)len;

    uint8_t mask[4];
    ws_random_mask(mask);
    memcpy(header + 2, mask, 4);

    for (size_t i = 0; i < len; i++) {
        header[6 + i] = payload[i] ^ mask[i & 3];
    }

    ssize_t total = 6 + len;
    return send(mc->fd, header, total, MSG_NOSIGNAL) == total ? 0 : -1;
}

/*
 * Receive a single WebSocket frame.
 * Returns payload length on success, -1 on error, 0 on timeout.
 * Caller must free *payload_out if return > 0.
 */
static int ws_recv_frame(MoonrakerClient *mc, int *opcode_out,
                          uint8_t **payload_out, size_t *len_out,
                          int timeout_ms) {
    /* Wait for data with select() */
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(mc->fd, &rfds);
    struct timeval tv = {
        .tv_sec = timeout_ms / 1000,
        .tv_usec = (timeout_ms % 1000) * 1000
    };

    int ret = select(mc->fd + 1, &rfds, NULL, NULL, &tv);
    if (ret < 0) return -1;
    if (ret == 0) return 0;  /* Timeout */

    /* Read first 2 bytes (FIN, opcode, MASK, length) */
    uint8_t hdr[2];
    ssize_t n = recv(mc->fd, hdr, 2, MSG_WAITALL);
    if (n != 2) return -1;

    int opcode = hdr[0] & 0x0F;
    int masked = (hdr[1] >> 7) & 1;
    uint64_t payload_len = hdr[1] & 0x7F;

    /* Extended payload length */
    if (payload_len == 126) {
        uint8_t ext[2];
        if (recv(mc->fd, ext, 2, MSG_WAITALL) != 2) return -1;
        payload_len = ((uint64_t)ext[0] << 8) | ext[1];
    } else if (payload_len == 127) {
        uint8_t ext[8];
        if (recv(mc->fd, ext, 8, MSG_WAITALL) != 8) return -1;
        payload_len = 0;
        for (int i = 0; i < 8; i++)
            payload_len = (payload_len << 8) | ext[i];
    }

    /* Sanity check: reject frames > 1MB */
    if (payload_len > 1024 * 1024) return -1;

    /* Read masking key (server frames should not be masked, but handle it) */
    uint8_t mask[4] = {0};
    if (masked) {
        if (recv(mc->fd, mask, 4, MSG_WAITALL) != 4) return -1;
    }

    /* Read payload */
    uint8_t *payload = NULL;
    if (payload_len > 0) {
        payload = malloc(payload_len + 1);
        if (!payload) return -1;

        size_t received = 0;
        while (received < payload_len) {
            n = recv(mc->fd, payload + received, payload_len - received, 0);
            if (n <= 0) {
                free(payload);
                return -1;
            }
            received += n;
        }

        /* Unmask if needed */
        if (masked) {
            for (size_t i = 0; i < payload_len; i++)
                payload[i] ^= mask[i & 3];
        }
        payload[payload_len] = '\0';  /* NUL-terminate for text frames */
    }

    *opcode_out = opcode;
    *payload_out = payload;
    *len_out = (size_t)payload_len;
    return (int)payload_len;
}

/* ============================================================================
 * JSON-RPC Layer
 * ============================================================================ */

/*
 * Send a JSON-RPC 2.0 request.
 * params_json is the raw JSON string for "params" (or NULL for no params).
 */
static int send_jsonrpc(MoonrakerClient *mc, const char *method,
                         const char *params_json) {
    char buf[WS_SEND_BUF];
    int id = ++mc->request_id;

    int len;
    if (params_json) {
        len = snprintf(buf, sizeof(buf),
            "{\"jsonrpc\":\"2.0\",\"method\":\"%s\",\"id\":%d,\"params\":%s}",
            method, id, params_json);
    } else {
        len = snprintf(buf, sizeof(buf),
            "{\"jsonrpc\":\"2.0\",\"method\":\"%s\",\"id\":%d}",
            method, id);
    }

    if (len >= (int)sizeof(buf)) return -1;

    mr_debug("Send: %s\n", buf);
    return ws_send_text(mc, buf, len);
}

/*
 * Subscribe to print_stats and virtual_sdcard objects.
 * Returns 0 on success, -1 on error.
 */
static int subscribe_print_stats(MoonrakerClient *mc) {
    const char *params =
        "{\"objects\":{"
            "\"print_stats\":null,"
            "\"virtual_sdcard\":[\"current_layer\",\"total_layer\"]"
        "}}";
    return send_jsonrpc(mc, "printer.objects.subscribe", params);
}

/* ============================================================================
 * Timelapse Helpers
 * ============================================================================ */

/* Configure timelapse from AppConfig settings */
static void configure_timelapse(MoonrakerClient *mc) {
    AppConfig *cfg = mc->config;

    timelapse_set_fps(cfg->timelapse_output_fps);
    timelapse_set_crf(cfg->timelapse_crf);
    timelapse_set_duplicate_last(cfg->timelapse_duplicate_last_frame);
    timelapse_set_flip(cfg->timelapse_flip_x, cfg->timelapse_flip_y);

    if (cfg->timelapse_variable_fps) {
        timelapse_set_variable_fps(cfg->timelapse_variable_fps_min,
                                    cfg->timelapse_variable_fps_max,
                                    cfg->timelapse_target_length);
    }

    /* Set output directory based on storage setting */
    if (strcmp(cfg->timelapse_storage, "usb") == 0) {
        const char *usb_path = cfg->timelapse_usb_path[0]
            ? cfg->timelapse_usb_path
            : "/mnt/udisk/Time-lapse-Video/";
        timelapse_set_output_dir(usb_path);
    } else {
        timelapse_set_output_dir(TIMELAPSE_OUTPUT_DIR);
    }

    /* Use hardware VENC encoding */
    timelapse_set_use_venc(1);
}

/* Capture a frame with stream delay */
static void capture_with_delay(MoonrakerClient *mc) {
    float delay = mc->config->timelapse_stream_delay;
    if (delay > 0) {
        usleep((useconds_t)(delay * 1000000));
    }
    if (timelapse_capture_frame() == 0) {
        mc->timelapse_frames++;
    }
}

/* ============================================================================
 * Timelapse Callbacks
 * ============================================================================ */

static void on_print_start(MoonrakerClient *mc, const char *filename) {
    mr_log("Print started: %s\n", filename ? filename : "(unknown)");

    /* Store filename */
    if (filename) {
        strncpy(mc->filename, filename, sizeof(mc->filename) - 1);
        mc->filename[sizeof(mc->filename) - 1] = '\0';
    }

    /* Reset timelapse state */
    mc->timelapse_first_layer_captured = 0;
    mc->timelapse_frames = 0;
    mc->current_layer = 0;

    /* Configure and initialize timelapse */
    configure_timelapse(mc);

    /* Extract base name from gcode filename (remove path and .gcode extension) */
    const char *base = filename;
    if (base) {
        const char *slash = strrchr(base, '/');
        if (slash) base = slash + 1;
    }

    char name[256];
    if (base && base[0]) {
        strncpy(name, base, sizeof(name) - 1);
        name[sizeof(name) - 1] = '\0';
        /* Remove .gcode extension */
        char *dot = strrchr(name, '.');
        if (dot && (strcmp(dot, ".gcode") == 0 || strcmp(dot, ".GCODE") == 0)) {
            *dot = '\0';
        }
    } else {
        snprintf(name, sizeof(name), "timelapse_%ld", (long)time(NULL));
    }

    if (timelapse_init(name, NULL) == 0) {
        mc->timelapse_active = 1;
        mr_log("Timelapse initialized: %s\n", name);
    } else {
        mr_log("Timelapse init failed for: %s\n", name);
    }
}

static void on_first_layer(MoonrakerClient *mc) {
    if (!mc->timelapse_active) return;
    if (mc->timelapse_first_layer_captured) return;

    mc->timelapse_first_layer_captured = 1;
    mr_debug("First layer — capturing frame\n");
    capture_with_delay(mc);

    /* Start hyperlapse timer if in hyperlapse mode */
    if (strcmp(mc->config->timelapse_mode, "hyperlapse") == 0) {
        mc->hyperlapse_running = 1;
        /* Thread creation handled below in hyperlapse_thread_func */
        extern void *hyperlapse_thread_func(void *arg);
        if (pthread_create(&mc->hyperlapse_thread, NULL,
                            hyperlapse_thread_func, mc) != 0) {
            mr_log("Failed to start hyperlapse thread\n");
            mc->hyperlapse_running = 0;
        } else {
            mr_debug("Hyperlapse thread started (interval=%ds)\n",
                     mc->config->timelapse_hyperlapse_interval);
        }
    }
}

static void on_layer_change(MoonrakerClient *mc, int layer, int total) {
    if (!mc->timelapse_active) return;

    mc->current_layer = layer;
    mc->total_layers = total;

    /* Skip layer 1 (already captured by on_first_layer) */
    if (layer <= 1) return;

    /* Only capture in layer mode */
    if (strcmp(mc->config->timelapse_mode, "layer") == 0) {
        mr_debug("Layer %d/%d — capturing frame\n", layer, total);
        capture_with_delay(mc);
    }
}

static void stop_hyperlapse(MoonrakerClient *mc) {
    if (mc->hyperlapse_running) {
        mc->hyperlapse_running = 0;
        pthread_join(mc->hyperlapse_thread, NULL);
        mr_debug("Hyperlapse thread stopped\n");
    }
}

static void on_print_complete(MoonrakerClient *mc, const char *filename) {
    if (!mc->timelapse_active) return;

    mr_log("Print complete: %s (%d frames)\n",
           filename ? filename : mc->filename, mc->timelapse_frames);

    stop_hyperlapse(mc);

    /* End delay before final frame */
    float end_delay = mc->config->timelapse_end_delay;
    if (end_delay > 0) {
        mr_debug("End delay: %.1fs\n", end_delay);
        usleep((useconds_t)(end_delay * 1000000));
    }

    /* Capture final frame */
    capture_with_delay(mc);

    /* Finalize timelapse (encode frames to MP4) */
    mr_log("Finalizing timelapse (%d frames)...\n", mc->timelapse_frames);
    timelapse_finalize();

    mc->timelapse_active = 0;
    mc->timelapse_frames = 0;
}

static void on_print_cancel(MoonrakerClient *mc, const char *filename,
                              const char *reason) {
    if (!mc->timelapse_active) return;

    mr_log("Print %s: %s (%d frames)\n",
           reason, filename ? filename : mc->filename, mc->timelapse_frames);

    stop_hyperlapse(mc);

    /* Finalize partial recording (save what we have) */
    if (mc->timelapse_frames > 0) {
        mr_log("Saving partial timelapse (%d frames)...\n", mc->timelapse_frames);
        timelapse_finalize();
    } else {
        timelapse_cancel();
    }

    mc->timelapse_active = 0;
    mc->timelapse_frames = 0;
}

/* ============================================================================
 * Hyperlapse Thread
 * ============================================================================ */

void *hyperlapse_thread_func(void *arg) {
    MoonrakerClient *mc = (MoonrakerClient *)arg;
    int interval = mc->config->timelapse_hyperlapse_interval;
    if (interval < 1) interval = 30;

    mr_debug("Hyperlapse: capturing every %ds\n", interval);

    while (mc->hyperlapse_running && mc->timelapse_active) {
        /* Sleep in 1-second intervals so we can check running flag */
        for (int i = 0; i < interval; i++) {
            if (!mc->hyperlapse_running || !mc->timelapse_active) goto done;
            sleep(1);
        }

        if (mc->hyperlapse_running && mc->timelapse_active) {
            mr_debug("Hyperlapse: capturing frame %d\n",
                     mc->timelapse_frames + 1);
            capture_with_delay(mc);
        }
    }

done:
    return NULL;
}

/* ============================================================================
 * Event Processing
 * ============================================================================ */

/*
 * Extract layer info from notification.
 * Tries virtual_sdcard first (Anycubic printers), then print_stats.info.
 */
static void extract_layers(cJSON *params_obj, int *layer_out, int *total_out) {
    int layer = -1, total = -1;

    /* Priority 1: virtual_sdcard */
    cJSON *vsd = cJSON_GetObjectItemCaseSensitive(params_obj, "virtual_sdcard");
    if (vsd) {
        cJSON *cl = cJSON_GetObjectItemCaseSensitive(vsd, "current_layer");
        cJSON *tl = cJSON_GetObjectItemCaseSensitive(vsd, "total_layer");
        if (cJSON_IsNumber(cl)) layer = (int)cl->valuedouble;
        if (cJSON_IsNumber(tl)) total = (int)tl->valuedouble;
    }

    /* Priority 2: print_stats.info */
    if (layer < 0) {
        cJSON *ps = cJSON_GetObjectItemCaseSensitive(params_obj, "print_stats");
        if (ps) {
            cJSON *info = cJSON_GetObjectItemCaseSensitive(ps, "info");
            if (info) {
                cJSON *cl = cJSON_GetObjectItemCaseSensitive(info, "current_layer");
                cJSON *tl = cJSON_GetObjectItemCaseSensitive(info, "total_layer");
                if (cJSON_IsNumber(cl)) layer = (int)cl->valuedouble;
                if (cJSON_IsNumber(tl)) total = (int)tl->valuedouble;
            }
        }
    }

    *layer_out = layer;
    *total_out = total;
}

/*
 * Handle a notify_status_update event.
 * params is the first element of the "params" array.
 */
static void handle_status_update(MoonrakerClient *mc, cJSON *params_obj) {
    /* Extract print_stats fields */
    cJSON *ps = cJSON_GetObjectItemCaseSensitive(params_obj, "print_stats");
    if (ps) {
        cJSON *state = cJSON_GetObjectItemCaseSensitive(ps, "state");
        if (cJSON_IsString(state) && state->valuestring) {
            const char *new_state = state->valuestring;
            const char *old_state = mc->print_state;

            /* Detect state transitions */
            int was_printing = (strcmp(old_state, "printing") == 0);
            int is_printing = (strcmp(new_state, "printing") == 0);

            /* * → printing */
            if (!was_printing && is_printing) {
                /* Get filename */
                cJSON *fn = cJSON_GetObjectItemCaseSensitive(ps, "filename");
                const char *fname = (cJSON_IsString(fn) && fn->valuestring)
                    ? fn->valuestring : NULL;
                on_print_start(mc, fname);
            }
            /* printing → complete */
            else if (was_printing && strcmp(new_state, "complete") == 0) {
                on_print_complete(mc, mc->filename);
            }
            /* printing → cancelled/error */
            else if (was_printing &&
                     (strcmp(new_state, "cancelled") == 0 ||
                      strcmp(new_state, "error") == 0)) {
                on_print_cancel(mc, mc->filename, new_state);
            }

            /* Update stored state */
            strncpy(mc->print_state, new_state,
                    sizeof(mc->print_state) - 1);
            mc->print_state[sizeof(mc->print_state) - 1] = '\0';
        }

        /* Extract filename if present (for late joins) */
        cJSON *fn = cJSON_GetObjectItemCaseSensitive(ps, "filename");
        if (cJSON_IsString(fn) && fn->valuestring && fn->valuestring[0]) {
            strncpy(mc->filename, fn->valuestring,
                    sizeof(mc->filename) - 1);
        }

        /* Extract print duration */
        cJSON *dur = cJSON_GetObjectItemCaseSensitive(ps, "print_duration");
        if (cJSON_IsNumber(dur)) {
            mc->print_duration = (float)dur->valuedouble;
        }
    }

    /* Extract and process layer info */
    int layer = -1, total = -1;
    extract_layers(params_obj, &layer, &total);

    if (layer >= 0 && strcmp(mc->print_state, "printing") == 0) {
        if (total >= 0) mc->total_layers = total;

        int prev_layer = mc->current_layer;

        /* First layer detection */
        if (layer >= 1 && !mc->timelapse_first_layer_captured) {
            on_first_layer(mc);
        }

        /* Layer change detection (layer must increase, skip layer 1) */
        if (layer != prev_layer && layer >= 2) {
            on_layer_change(mc, layer, total >= 0 ? total : mc->total_layers);
        }

        mc->current_layer = layer;
    }
}

/*
 * Process a WebSocket text message (JSON).
 */
static void process_message(MoonrakerClient *mc, const char *payload) {
    cJSON *json = cJSON_Parse(payload);
    if (!json) return;

    /* Check for JSON-RPC notification: notify_status_update */
    cJSON *method = cJSON_GetObjectItemCaseSensitive(json, "method");
    if (cJSON_IsString(method) &&
        strcmp(method->valuestring, "notify_status_update") == 0) {
        cJSON *params = cJSON_GetObjectItemCaseSensitive(json, "params");
        if (cJSON_IsArray(params)) {
            cJSON *first = cJSON_GetArrayItem(params, 0);
            if (first) {
                handle_status_update(mc, first);
            }
        }
    }
    /* Check for subscription response (contains initial state) */
    else if (cJSON_GetObjectItemCaseSensitive(json, "result")) {
        cJSON *result = cJSON_GetObjectItemCaseSensitive(json, "result");
        cJSON *status = cJSON_GetObjectItemCaseSensitive(result, "status");
        if (status) {
            mr_debug("Processing initial state from subscription\n");
            handle_status_update(mc, status);
        }
    }

    cJSON_Delete(json);
}

/* ============================================================================
 * Main Thread
 * ============================================================================ */

static void *moonraker_thread_func(void *arg) {
    MoonrakerClient *mc = (MoonrakerClient *)arg;

    while (mc->running) {
        /* Step 1: TCP connect */
        mr_debug("Connecting to %s:%d...\n", mc->host, mc->port);
        mc->fd = tcp_connect(mc->host, mc->port, CONNECT_TIMEOUT);
        if (mc->fd < 0) {
            mr_debug("Connection failed, retrying in %ds\n", RECONNECT_DELAY);
            for (int i = 0; i < RECONNECT_DELAY && mc->running; i++)
                sleep(1);
            continue;
        }

        /* Step 2: WebSocket handshake */
        if (ws_handshake(mc) != 0) {
            mr_log("WebSocket handshake failed\n");
            close(mc->fd);
            mc->fd = -1;
            for (int i = 0; i < RECONNECT_DELAY && mc->running; i++)
                sleep(1);
            continue;
        }

        mc->connected = 1;
        mr_log("Connected to %s:%d\n", mc->host, mc->port);

        /* Step 3: Set custom timelapse mode */
        timelapse_set_custom_mode(1);

        /* Step 4: Subscribe to print status */
        if (subscribe_print_stats(mc) != 0) {
            mr_log("Failed to subscribe\n");
            close(mc->fd);
            mc->fd = -1;
            mc->connected = 0;
            timelapse_set_custom_mode(0);
            for (int i = 0; i < RECONNECT_DELAY && mc->running; i++)
                sleep(1);
            continue;
        }

        /* Step 5: Receive loop */
        while (mc->running && mc->connected) {
            int opcode = 0;
            uint8_t *payload = NULL;
            size_t payload_len = 0;

            int ret = ws_recv_frame(mc, &opcode, &payload, &payload_len,
                                     RECV_TIMEOUT_MS);

            if (ret < 0) {
                /* Connection error */
                mr_log("Connection lost\n");
                break;
            }

            if (ret == 0) {
                /* Timeout — no data, just continue */
                continue;
            }

            switch (opcode) {
            case WS_OP_TEXT:
                if (payload && payload_len > 0) {
                    process_message(mc, (const char *)payload);
                }
                break;

            case WS_OP_PING:
                ws_send_pong(mc, payload, payload_len);
                break;

            case WS_OP_CLOSE:
                mr_debug("Server sent close frame\n");
                free(payload);
                goto disconnected;

            default:
                /* Ignore other opcodes (binary, continuation) */
                break;
            }

            free(payload);
        }

disconnected:
        /* Cleanup connection */
        if (mc->fd >= 0) {
            close(mc->fd);
            mc->fd = -1;
        }
        mc->connected = 0;

        /* Only release custom mode if no active timelapse */
        if (!mc->timelapse_active) {
            timelapse_set_custom_mode(0);
        }

        if (mc->running) {
            mr_log("Disconnected, reconnecting in %ds\n", RECONNECT_DELAY);
            for (int i = 0; i < RECONNECT_DELAY && mc->running; i++)
                sleep(1);
        }
    }

    /* Final cleanup */
    if (mc->fd >= 0) {
        close(mc->fd);
        mc->fd = -1;
    }
    mc->connected = 0;

    return NULL;
}

/* ============================================================================
 * Public API
 * ============================================================================ */

int moonraker_client_start(MoonrakerClient *mc, const char *host, int port,
                            AppConfig *cfg) {
    memset(mc, 0, sizeof(*mc));
    mc->fd = -1;

    strncpy(mc->host, host, sizeof(mc->host) - 1);
    mc->port = port;
    mc->config = cfg;
    strncpy(mc->print_state, "standby", sizeof(mc->print_state) - 1);

    mc->running = 1;
    if (pthread_create(&mc->thread, NULL, moonraker_thread_func, mc) != 0) {
        mr_log("Failed to create thread\n");
        return -1;
    }

    mr_log("Started (target: %s:%d)\n", host, port);
    return 0;
}

void moonraker_client_stop(MoonrakerClient *mc) {
    if (!mc->running) return;

    mc->running = 0;

    /* Stop hyperlapse thread first */
    stop_hyperlapse(mc);

    /* Close socket to unblock any recv() */
    if (mc->fd >= 0) {
        shutdown(mc->fd, SHUT_RDWR);
    }

    pthread_join(mc->thread, NULL);

    /* Release custom timelapse mode */
    timelapse_set_custom_mode(0);

    mr_log("Stopped\n");
}

int moonraker_client_is_connected(const MoonrakerClient *mc) {
    return mc->connected;
}
