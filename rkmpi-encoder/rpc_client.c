/*
 * RPC Client Implementation
 *
 * Connects to port 18086 and handles video_stream_request messages.
 */

#include "rpc_client.h"
#include "timelapse.h"
#include "cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>

/* Global RPC client */
RPCClient g_rpc_client;

/* External verbose flag */
extern int g_verbose;

/*
 * Timing instrumentation - enable with -DENCODER_TIMING
 */
#ifdef ENCODER_TIMING
#include <time.h>
#define RPC_TIMING_INTERVAL 100

typedef struct {
    uint64_t select_time;
    uint64_t recv_time;
    uint64_t json_parse_time;
    uint64_t total_iter;
    int count;
} RpcTiming;

static RpcTiming g_rpc_timing = {0};

static uint64_t rpc_timing_get_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

#define RPC_TIMING_START(var) uint64_t _rt_##var = rpc_timing_get_us()
#define RPC_TIMING_END(field) g_rpc_timing.field += rpc_timing_get_us() - _rt_##field
#define RPC_TIMING_LOG() do { \
    if (g_rpc_timing.count >= RPC_TIMING_INTERVAL) { \
        double n = (double)g_rpc_timing.count; \
        fprintf(stderr, "[RPC] iters=%d avg(us): select=%.1f recv=%.1f json=%.1f total=%.1f\n", \
                g_rpc_timing.count, \
                g_rpc_timing.select_time / n, \
                g_rpc_timing.recv_time / n, \
                g_rpc_timing.json_parse_time / n, \
                g_rpc_timing.total_iter / n); \
        memset(&g_rpc_timing, 0, sizeof(g_rpc_timing)); \
    } \
} while(0)
#else
#define RPC_TIMING_START(var) (void)0
#define RPC_TIMING_END(field) (void)0
#define RPC_TIMING_LOG() (void)0
#endif

static void rpc_log(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    fprintf(stderr, "RPC: ");
    vfprintf(stderr, fmt, args);
    va_end(args);
    fflush(stderr);
}

/* Send VideoStreamReply response */
static void rpc_send_video_reply(RPCClient *client, int req_id, const char *method) {
    /* Format response as pretty JSON like gkcam does */
    char response[512];
    int len = snprintf(response, sizeof(response) - 1,
        "{\n"
        "\t\"id\":\t0,\n"
        "\t\"method\":\t\"Video/VideoStreamReply\",\n"
        "\t\"params\":\t{\n"
        "\t\t\"eventtime\":\t0,\n"
        "\t\t\"status\":\t{\n"
        "\t\t\t\"video_stream_reply\":\t{\n"
        "\t\t\t\t\"id\":\t%d,\n"
        "\t\t\t\t\"method\":\t\"%s\",\n"
        "\t\t\t\t\"result\":\t{\n"
        "\t\t\t\t}\n"
        "\t\t\t}\n"
        "\t\t}\n"
        "\t}\n"
        "}\x03",
        req_id, method);

    if (send(client->sock_fd, response, len, MSG_NOSIGNAL) > 0) {
        rpc_log("Sent VideoStreamReply (id=%d, method=%s)\n", req_id, method);
    }
}

/* Handle video_stream_request commands */
static void handle_video_request(RPCClient *client, cJSON *video_request) {
    cJSON *req_id = cJSON_GetObjectItem(video_request, "id");
    cJSON *req_method = cJSON_GetObjectItem(video_request, "method");
    cJSON *req_params = cJSON_GetObjectItem(video_request, "params");

    if (!cJSON_IsNumber(req_id) || !cJSON_IsString(req_method)) {
        return;
    }

    int id = (int)req_id->valuedouble;
    const char *m = req_method->valuestring;

    /* Handle openDelayCamera - Initialize timelapse */
    if (strcmp(m, "openDelayCamera") == 0) {
        rpc_log("Received openDelayCamera\n");

        /* If custom timelapse mode is enabled, ignore RPC timelapse commands */
        if (timelapse_is_custom_mode()) {
            rpc_log("Ignoring openDelayCamera - custom timelapse mode active\n");
            rpc_send_video_reply(client, id, m);  /* Still respond success */
            return;
        }

        const char *filepath = NULL;
        if (req_params) {
            cJSON *fp = cJSON_GetObjectItem(req_params, "filepath");
            if (cJSON_IsString(fp)) {
                filepath = fp->valuestring;
            }
        }

        if (timelapse_init_legacy(filepath) == 0) {
            rpc_send_video_reply(client, id, m);
        } else {
            rpc_log("openDelayCamera: timelapse_init failed\n");
            rpc_send_video_reply(client, id, m);  /* Still respond success */
        }
        return;
    }

    /* Handle SetLed - Return success (LED controlled by firmware) */
    if (strcmp(m, "SetLed") == 0) {
        /* Don't log SetLed to reduce noise - it's called frequently */
        rpc_send_video_reply(client, id, m);
        return;
    }

    /* Handle startLanCapture */
    if (strcmp(m, "startLanCapture") == 0) {
        rpc_log("Received startLanCapture\n");
        rpc_send_video_reply(client, id, m);

        /* If custom timelapse mode is enabled, ignore RPC capture commands */
        if (timelapse_is_custom_mode()) {
            /* Don't log - this gets called frequently during print */
            return;
        }

        /* Capture timelapse frame if active (RPC mode) */
        if (timelapse_is_active()) {
            timelapse_capture_frame();
        }
        return;
    }

    /* Handle stopLanCapture */
    if (strcmp(m, "stopLanCapture") == 0) {
        rpc_log("Received stopLanCapture\n");
        rpc_send_video_reply(client, id, m);
        return;
    }
}

/* Check for print completion to finalize timelapse */
static void check_print_completion(cJSON *status) {
    /* If custom timelapse mode is enabled, h264_server handles finalization */
    if (timelapse_is_custom_mode()) {
        return;
    }

    if (!timelapse_is_active()) {
        return;
    }

    cJSON *print_stats = cJSON_GetObjectItem(status, "print_stats");
    if (!print_stats) {
        return;
    }

    cJSON *state = cJSON_GetObjectItem(print_stats, "state");
    if (!cJSON_IsString(state)) {
        return;
    }

    const char *state_str = state->valuestring;

    /* Finalize on print completion */
    if (strcmp(state_str, "complete") == 0) {
        rpc_log("Print completed, finalizing timelapse\n");
        timelapse_finalize();
    }
    /* Cancel on error or cancellation */
    else if (strcmp(state_str, "cancelled") == 0 ||
             strcmp(state_str, "error") == 0) {
        rpc_log("Print %s, canceling timelapse\n", state_str);
        timelapse_cancel();
    }
}

/* Handle incoming RPC message */
static void rpc_handle_message(RPCClient *client, const char *msg) {
    cJSON *json = cJSON_Parse(msg);
    if (!json) return;

    cJSON *method = cJSON_GetObjectItem(json, "method");
    cJSON *params = cJSON_GetObjectItem(json, "params");

    if (!cJSON_IsString(method) || !params) {
        cJSON_Delete(json);
        return;
    }

    /* Handle process_status_update messages */
    if (strcmp(method->valuestring, "process_status_update") == 0) {
        cJSON *status = cJSON_GetObjectItem(params, "status");
        if (status) {
            /* Handle video stream requests */
            cJSON *video_request = cJSON_GetObjectItem(status, "video_stream_request");
            if (video_request) {
                handle_video_request(client, video_request);
            }

            /* Check for print completion (to finalize timelapse) */
            check_print_completion(status);
        }
    }

    cJSON_Delete(json);
}

/* Connect to RPC port */
static int rpc_connect(RPCClient *client) {
    client->sock_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (client->sock_fd < 0) {
        rpc_log("socket() failed: %s\n", strerror(errno));
        return -1;
    }

    /* Set tiny receive buffer to prevent buildup */
    int rcvbuf = 4096;
    setsockopt(client->sock_fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));

    /* Set timeout */
    struct timeval tv;
    tv.tv_sec = RPC_TIMEOUT_SEC;
    tv.tv_usec = 0;
    setsockopt(client->sock_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(client->sock_fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(RPC_PORT);
    addr.sin_addr.s_addr = inet_addr(RPC_HOST);

    if (connect(client->sock_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        rpc_log("connect() failed: %s\n", strerror(errno));
        close(client->sock_fd);
        client->sock_fd = -1;
        return -1;
    }

    rpc_log("Connected to port %d\n", RPC_PORT);
    client->connected = 1;
    return 0;
}

/* Disconnect from RPC port */
static void rpc_disconnect(RPCClient *client) {
    if (client->sock_fd >= 0) {
        close(client->sock_fd);
        client->sock_fd = -1;
    }
    client->connected = 0;
}

/* RPC client thread */
static void *rpc_thread(void *arg) {
    RPCClient *client = (RPCClient *)arg;
    uint8_t recv_buf[RPC_RECV_BUF];

    /* Patterns we're looking for */
    const char *video_needle = "\"video_stream_request\"";
    const char *print_needle = "\"print_stats\"";

    while (client->running) {
        RPC_TIMING_START(total_iter);

        /* Connect if not connected */
        if (!client->connected) {
            if (rpc_connect(client) != 0) {
                sleep(3);
                continue;
            }
        }

        /* Use select for efficient waiting */
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(client->sock_fd, &read_fds);

        RPC_TIMING_START(select_time);
        struct timeval tv = { .tv_sec = 0, .tv_usec = 500000 };  /* 500ms */
        int ret = select(client->sock_fd + 1, &read_fds, NULL, NULL, &tv);
        RPC_TIMING_END(select_time);

        if (ret <= 0) {
#ifdef ENCODER_TIMING
            RPC_TIMING_END(total_iter);
            g_rpc_timing.count++;
            RPC_TIMING_LOG();
#endif
            continue;
        }

        /* Receive data */
        RPC_TIMING_START(recv_time);
        ssize_t n = recv(client->sock_fd, recv_buf, sizeof(recv_buf) - 1, 0);
        RPC_TIMING_END(recv_time);

        if (n <= 0) {
            if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) {
                rpc_log("Connection closed\n");
                rpc_disconnect(client);
            }
            continue;
        }

        recv_buf[n] = '\0';

        /* Quick check for messages we care about */
        char *found = strstr((char *)recv_buf, video_needle);
        if (!found) {
            /* Also check for print_stats (for timelapse completion detection) */
            if (timelapse_is_active()) {
                found = strstr((char *)recv_buf, print_needle);
            }
        }
        if (!found) {
#ifdef ENCODER_TIMING
            RPC_TIMING_END(total_iter);
            g_rpc_timing.count++;
            RPC_TIMING_LOG();
#endif
            continue;
        }

        /* Find message boundaries (ETX delimited) */
        char *msg_start = (char *)recv_buf;
        char *etx = found;

        /* Find start of message (look backwards for ETX or start of buffer) */
        while (etx > (char *)recv_buf && *etx != RPC_ETX) {
            etx--;
        }
        if (*etx == RPC_ETX) {
            msg_start = etx + 1;
        }

        /* Find end of message */
        char *msg_end = strchr(found, RPC_ETX);
        if (!msg_end) continue;

        /* Extract and handle message */
        RPC_TIMING_START(json_parse_time);
        size_t msg_len = msg_end - msg_start;
        char *msg = malloc(msg_len + 1);
        if (msg) {
            memcpy(msg, msg_start, msg_len);
            msg[msg_len] = '\0';
            rpc_handle_message(client, msg);
            free(msg);
        }
        RPC_TIMING_END(json_parse_time);

#ifdef ENCODER_TIMING
        RPC_TIMING_END(total_iter);
        g_rpc_timing.count++;
        RPC_TIMING_LOG();
#endif
    }

    rpc_disconnect(client);
    return NULL;
}

/* Start RPC client */
int rpc_client_start(void) {
    memset(&g_rpc_client, 0, sizeof(g_rpc_client));
    g_rpc_client.sock_fd = -1;
    g_rpc_client.running = 1;

    if (pthread_create(&g_rpc_client.thread, NULL, rpc_thread, &g_rpc_client) != 0) {
        rpc_log("Failed to create thread\n");
        return -1;
    }

    rpc_log("Started\n");
    return 0;
}

/* Stop RPC client */
void rpc_client_stop(void) {
    g_rpc_client.running = 0;

    /* Close socket to unblock any recv() */
    if (g_rpc_client.sock_fd >= 0) {
        shutdown(g_rpc_client.sock_fd, SHUT_RDWR);
    }

    pthread_join(g_rpc_client.thread, NULL);
    rpc_log("Stopped\n");
}
