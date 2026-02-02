/*
 * MQTT Client Implementation
 *
 * Implements MQTT 3.1.1 protocol with TLS for video responder functionality.
 *
 * NOTE: TLS support requires OpenSSL. If not available at compile time,
 * this module provides stub functions that return errors.
 */

#include "mqtt_client.h"
#include "cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>

/* Check for OpenSSL availability */
#ifdef HAVE_OPENSSL
#include <openssl/ssl.h>
#include <openssl/err.h>
#define TLS_AVAILABLE 1
#else
#define TLS_AVAILABLE 0
#endif

/* Global MQTT client */
MQTTClient g_mqtt_client;

/* External verbose flag */
extern int g_verbose;

/*
 * Timing instrumentation - enable with -DENCODER_TIMING
 */
#ifdef ENCODER_TIMING
#define MQTT_TIMING_INTERVAL 100

typedef struct {
    uint64_t select_time;
    uint64_t ssl_read_time;
    uint64_t json_parse_time;
    uint64_t total_iter;
    int count;
} MqttTiming;

static MqttTiming g_mqtt_timing = {0};

static uint64_t mqtt_timing_get_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

#define MQTT_TIMING_START(var) uint64_t _mt_##var = mqtt_timing_get_us()
#define MQTT_TIMING_END(field) g_mqtt_timing.field += mqtt_timing_get_us() - _mt_##field
#define MQTT_TIMING_LOG() do { \
    if (g_mqtt_timing.count >= MQTT_TIMING_INTERVAL) { \
        double n = (double)g_mqtt_timing.count; \
        fprintf(stderr, "[MQTT] iters=%d avg(us): select=%.1f ssl=%.1f json=%.1f total=%.1f\n", \
                g_mqtt_timing.count, \
                g_mqtt_timing.select_time / n, \
                g_mqtt_timing.ssl_read_time / n, \
                g_mqtt_timing.json_parse_time / n, \
                g_mqtt_timing.total_iter / n); \
        memset(&g_mqtt_timing, 0, sizeof(g_mqtt_timing)); \
    } \
} while(0)
#else
#define MQTT_TIMING_START(var) (void)0
#define MQTT_TIMING_END(field) (void)0
#define MQTT_TIMING_LOG() (void)0
#endif

static void mqtt_log(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    fprintf(stderr, "MQTT: ");
    vfprintf(stderr, fmt, args);
    va_end(args);
    fflush(stderr);
}

#if TLS_AVAILABLE

static uint64_t get_time_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000 + (uint64_t)ts.tv_nsec / 1000;
}

static uint64_t get_time_ms(void) {
    return get_time_us() / 1000;
}

/* Encode MQTT remaining length (variable-length encoding) */
static size_t mqtt_encode_remaining_length(uint8_t *buf, size_t length) {
    size_t i = 0;
    do {
        uint8_t byte = length % 128;
        length /= 128;
        if (length > 0) {
            byte |= 0x80;
        }
        buf[i++] = byte;
    } while (length > 0);
    return i;
}

/* Encode MQTT string (length-prefixed UTF-8) */
static size_t mqtt_encode_string(uint8_t *buf, const char *str) {
    size_t len = strlen(str);
    buf[0] = (len >> 8) & 0xFF;
    buf[1] = len & 0xFF;
    memcpy(buf + 2, str, len);
    return 2 + len;
}

/* Build MQTT CONNECT packet */
static size_t mqtt_build_connect(uint8_t *buf, size_t buf_size,
                                 const char *client_id,
                                 const char *username,
                                 const char *password) {
    uint8_t var_header[256];
    uint8_t *p = var_header;

    /* Protocol name "MQTT" */
    p += mqtt_encode_string(p, "MQTT");

    /* Protocol level (4 = MQTT 3.1.1) */
    *p++ = 0x04;

    /* Connect flags: Username, Password, Clean Session */
    *p++ = 0xC2;

    /* Keep alive (60 seconds) */
    *p++ = 0x00;
    *p++ = 0x3C;

    size_t var_header_len = p - var_header;

    /* Payload: client_id, username, password */
    uint8_t payload[512];
    uint8_t *pp = payload;
    pp += mqtt_encode_string(pp, client_id);
    pp += mqtt_encode_string(pp, username);
    pp += mqtt_encode_string(pp, password);
    size_t payload_len = pp - payload;

    /* Build packet */
    size_t remaining = var_header_len + payload_len;
    uint8_t *out = buf;

    *out++ = MQTT_CONNECT;
    out += mqtt_encode_remaining_length(out, remaining);
    memcpy(out, var_header, var_header_len);
    out += var_header_len;
    memcpy(out, payload, payload_len);
    out += payload_len;

    return out - buf;
}

/* Build MQTT SUBSCRIBE packet */
static size_t mqtt_build_subscribe(uint8_t *buf, size_t buf_size,
                                   const char *topic, uint16_t packet_id) {
    uint8_t payload[512];
    uint8_t *p = payload;

    /* Packet ID */
    *p++ = (packet_id >> 8) & 0xFF;
    *p++ = packet_id & 0xFF;

    /* Topic + QoS */
    p += mqtt_encode_string(p, topic);
    *p++ = 0x00;  /* QoS 0 */

    size_t payload_len = p - payload;

    /* Build packet */
    uint8_t *out = buf;
    *out++ = MQTT_SUBSCRIBE;
    out += mqtt_encode_remaining_length(out, payload_len);
    memcpy(out, payload, payload_len);
    out += payload_len;

    return out - buf;
}

/* Build MQTT PUBLISH packet */
static size_t mqtt_build_publish(uint8_t *buf, size_t buf_size,
                                 const char *topic, const char *payload,
                                 int qos, uint16_t packet_id) {
    uint8_t var_header[256];
    uint8_t *p = var_header;

    p += mqtt_encode_string(p, topic);
    if (qos > 0) {
        *p++ = (packet_id >> 8) & 0xFF;
        *p++ = packet_id & 0xFF;
    }
    size_t var_header_len = p - var_header;

    size_t payload_len = strlen(payload);
    size_t remaining = var_header_len + payload_len;

    uint8_t *out = buf;
    uint8_t flags = (qos << 1) & 0x06;
    *out++ = MQTT_PUBLISH | flags;
    out += mqtt_encode_remaining_length(out, remaining);
    memcpy(out, var_header, var_header_len);
    out += var_header_len;
    memcpy(out, payload, payload_len);
    out += payload_len;

    return out - buf;
}

/* SSL send wrapper */
static int mqtt_ssl_send(MQTTClient *client, const void *data, size_t len) {
    if (!client->ssl) return -1;
    int ret = SSL_write((SSL *)client->ssl, data, (int)len);
    return (ret > 0) ? 0 : -1;
}

/* SSL recv wrapper */
static int mqtt_ssl_recv(MQTTClient *client, void *buf, size_t len, int timeout_sec) {
    if (!client->ssl) return -1;

    /* Set timeout */
    struct timeval tv;
    tv.tv_sec = timeout_sec;
    tv.tv_usec = 0;
    setsockopt(client->ssl_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    int ret = SSL_read((SSL *)client->ssl, buf, (int)len);
    return ret;
}

/* Check if msgid was already handled */
static int mqtt_is_msgid_handled(MQTTClient *client, const char *msgid) {
    if (!msgid || !msgid[0]) return 0;

    pthread_mutex_lock(&client->mutex);

    /* Cleanup old msgids periodically */
    uint64_t now = get_time_ms();
    if (now - client->msgid_cleanup_time > 60000) {
        client->msgid_count = 0;
        client->msgid_cleanup_time = now;
    }

    /* Check if already handled */
    for (int i = 0; i < client->msgid_count; i++) {
        if (strcmp(client->handled_msgids[i], msgid) == 0) {
            pthread_mutex_unlock(&client->mutex);
            return 1;
        }
    }

    /* Add to handled list */
    if (client->msgid_count < MQTT_MAX_MSGIDS) {
        strncpy(client->handled_msgids[client->msgid_count], msgid, 39);
        client->handled_msgids[client->msgid_count][39] = '\0';
        client->msgid_count++;
    }

    pthread_mutex_unlock(&client->mutex);
    return 0;
}

/* Send video response report */
static void mqtt_send_video_response(MQTTClient *client, const char *action, const char *msgid) {
    char topic[256];
    snprintf(topic, sizeof(topic),
             "anycubic/anycubicCloud/v1/printer/public/%s/%s/video/report",
             client->config.model_id, client->creds.device_id);

    /* Build response JSON */
    const char *state = (strcmp(action, "stopCapture") == 0) ? "pushStopped" : "initSuccess";

    char payload[512];
    snprintf(payload, sizeof(payload),
             "{\"type\":\"video\",\"action\":\"%s\",\"timestamp\":%llu,"
             "\"msgid\":\"%08x%08x\",\"state\":\"%s\",\"code\":200,\"msg\":\"\",\"data\":null}",
             action, (unsigned long long)get_time_ms(),
             (unsigned)rand(), (unsigned)rand(), state);

    uint8_t buf[1024];
    size_t len = mqtt_build_publish(buf, sizeof(buf), topic, payload, 0, 0);
    mqtt_ssl_send(client, buf, len);

    mqtt_log("Sent %s report (%s)\n", action, state);
}

/* Send counter report for spurious stopCapture */
static void mqtt_send_counter_report(MQTTClient *client) {
    char topic[256];
    snprintf(topic, sizeof(topic),
             "anycubic/anycubicCloud/v1/printer/public/%s/%s/video/report",
             client->config.model_id, client->creds.device_id);

    char payload[512];
    snprintf(payload, sizeof(payload),
             "{\"type\":\"video\",\"action\":\"startCapture\",\"timestamp\":%llu,"
             "\"msgid\":\"%08x%08x\",\"state\":\"initSuccess\",\"code\":200,\"msg\":\"\",\"data\":null}",
             (unsigned long long)get_time_ms(),
             (unsigned)rand(), (unsigned)rand());

    uint8_t buf[1024];
    size_t len = mqtt_build_publish(buf, sizeof(buf), topic, payload, 0, 0);
    mqtt_ssl_send(client, buf, len);

    mqtt_log("Sent counter report (startCapture/initSuccess)\n");
}

/* Handle incoming MQTT packet */
static int mqtt_handle_packet(MQTTClient *client, uint8_t *data, size_t len) {
    if (len < 2) return 0;

    uint8_t pkt_type = data[0] >> 4;

    /* Decode remaining length */
    size_t i = 1;
    size_t mult = 1, remaining_len = 0;
    while (i < len && (data[i] & 0x80)) {
        remaining_len += (data[i] & 0x7F) * mult;
        mult *= 128;
        i++;
    }
    if (i >= len) return 0;
    remaining_len += (data[i] & 0x7F) * mult;
    size_t header_end = i + 1;

    size_t pkt_len = header_end + remaining_len;
    if (len < pkt_len) return 0;

    /* Skip non-PUBLISH packets */
    if (pkt_type != 3) {
        return (int)pkt_len;
    }

    /* Parse PUBLISH */
    size_t topic_len = (data[header_end] << 8) | data[header_end + 1];
    size_t topic_start = header_end + 2;

    if (topic_start + topic_len > pkt_len) return (int)pkt_len;

    char topic[256];
    size_t copy_len = (topic_len < sizeof(topic) - 1) ? topic_len : sizeof(topic) - 1;
    memcpy(topic, data + topic_start, copy_len);
    topic[copy_len] = '\0';

    size_t payload_start = topic_start + topic_len;
    size_t payload_len = pkt_len - payload_start;

    char *payload = malloc(payload_len + 1);
    if (!payload) return (int)pkt_len;
    memcpy(payload, data + payload_start, payload_len);
    payload[payload_len] = '\0';

    /* Handle video commands (not reports) */
    if (strstr(topic, "/video") && !strstr(topic, "/report")) {
        cJSON *msg = cJSON_Parse(payload);
        if (msg) {
            cJSON *action_json = cJSON_GetObjectItem(msg, "action");
            cJSON *msgid_json = cJSON_GetObjectItem(msg, "msgid");

            const char *action = cJSON_IsString(action_json) ? action_json->valuestring : NULL;
            const char *msgid = cJSON_IsString(msgid_json) ? msgid_json->valuestring : NULL;

            if (action && (strcmp(action, "startCapture") == 0 || strcmp(action, "stopCapture") == 0)) {
                /* Deduplicate */
                if (!mqtt_is_msgid_handled(client, msgid)) {
                    mqtt_log("Received %s (msgid=%.8s...)\n", action, msgid ? msgid : "none");

                    /* Update streaming state */
                    if (strcmp(action, "stopCapture") == 0) {
                        client->streaming_paused = 1;
                        mqtt_log("Streaming PAUSED\n");
                    } else {
                        client->streaming_paused = 0;
                        mqtt_log("Streaming RESUMED\n");
                    }

                    mqtt_send_video_response(client, action, msgid);
                }
            }

            cJSON_Delete(msg);
        }
    }
    /* Handle spurious stopCapture reports */
    else if (strstr(topic, "/video/report")) {
        cJSON *msg = cJSON_Parse(payload);
        if (msg) {
            cJSON *action_json = cJSON_GetObjectItem(msg, "action");
            cJSON *msgid_json = cJSON_GetObjectItem(msg, "msgid");

            const char *action = cJSON_IsString(action_json) ? action_json->valuestring : NULL;
            const char *msgid = cJSON_IsString(msgid_json) ? msgid_json->valuestring : NULL;

            /* If it's a stopCapture report we didn't send, counter it */
            if (action && strcmp(action, "stopCapture") == 0 &&
                msgid && !mqtt_is_msgid_handled(client, msgid)) {
                mqtt_log("Detected spurious stopCapture report, countering!\n");
                mqtt_send_counter_report(client);
            }

            cJSON_Delete(msg);
        }
    }

    free(payload);
    return (int)pkt_len;
}

/* Connect to MQTT broker */
static int mqtt_connect(MQTTClient *client) {
    /* Create socket */
    client->ssl_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (client->ssl_fd < 0) {
        mqtt_log("socket() failed: %s\n", strerror(errno));
        return -1;
    }

    /* Connect */
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(MQTT_PORT);
    addr.sin_addr.s_addr = inet_addr(MQTT_HOST);

    struct timeval tv;
    tv.tv_sec = MQTT_TIMEOUT_SEC;
    tv.tv_usec = 0;
    setsockopt(client->ssl_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(client->ssl_fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    if (connect(client->ssl_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        mqtt_log("connect() failed: %s\n", strerror(errno));
        close(client->ssl_fd);
        client->ssl_fd = -1;
        return -1;
    }

    /* SSL setup */
    client->ssl_ctx = SSL_CTX_new(TLS_client_method());
    if (!client->ssl_ctx) {
        mqtt_log("SSL_CTX_new() failed\n");
        close(client->ssl_fd);
        client->ssl_fd = -1;
        return -1;
    }

    SSL_CTX_set_verify((SSL_CTX *)client->ssl_ctx, SSL_VERIFY_NONE, NULL);

    client->ssl = SSL_new((SSL_CTX *)client->ssl_ctx);
    if (!client->ssl) {
        mqtt_log("SSL_new() failed\n");
        SSL_CTX_free((SSL_CTX *)client->ssl_ctx);
        close(client->ssl_fd);
        client->ssl_fd = -1;
        return -1;
    }

    SSL_set_fd((SSL *)client->ssl, client->ssl_fd);

    if (SSL_connect((SSL *)client->ssl) <= 0) {
        mqtt_log("SSL_connect() failed\n");
        SSL_free((SSL *)client->ssl);
        SSL_CTX_free((SSL_CTX *)client->ssl_ctx);
        close(client->ssl_fd);
        client->ssl_fd = -1;
        return -1;
    }

    /* Send MQTT CONNECT */
    uint8_t buf[1024];
    size_t len = mqtt_build_connect(buf, sizeof(buf), client->client_id,
                                    client->creds.username, client->creds.password);
    if (mqtt_ssl_send(client, buf, len) < 0) {
        mqtt_log("Failed to send CONNECT\n");
        goto error;
    }

    /* Read CONNACK */
    uint8_t connack[4];
    int ret = mqtt_ssl_recv(client, connack, sizeof(connack), MQTT_TIMEOUT_SEC);
    if (ret < 4 || connack[0] != MQTT_CONNACK || connack[3] != 0) {
        mqtt_log("CONNECT failed: ret=%d, response=%02x%02x%02x%02x\n",
                 ret, connack[0], connack[1], connack[2], connack[3]);
        goto error;
    }

    mqtt_log("Connected to broker\n");
    client->connected = 1;
    client->last_activity = get_time_ms();
    return 0;

error:
    SSL_free((SSL *)client->ssl);
    SSL_CTX_free((SSL_CTX *)client->ssl_ctx);
    close(client->ssl_fd);
    client->ssl = NULL;
    client->ssl_ctx = NULL;
    client->ssl_fd = -1;
    return -1;
}

/* Subscribe to topics */
static void mqtt_subscribe_topics(MQTTClient *client) {
    uint8_t buf[512];
    char topic[256];
    size_t len;

    /* Web video topic */
    snprintf(topic, sizeof(topic),
             "anycubic/anycubicCloud/v1/web/printer/%s/%s/video",
             client->config.model_id, client->creds.device_id);
    len = mqtt_build_subscribe(buf, sizeof(buf), topic, 1);
    mqtt_ssl_send(client, buf, len);

    /* Slicer video topic */
    snprintf(topic, sizeof(topic),
             "anycubic/anycubicCloud/v1/slicer/printer/%s/%s/video",
             client->config.model_id, client->creds.device_id);
    len = mqtt_build_subscribe(buf, sizeof(buf), topic, 2);
    mqtt_ssl_send(client, buf, len);

    /* Report topic (to detect spurious stopCapture) */
    snprintf(topic, sizeof(topic),
             "anycubic/anycubicCloud/v1/printer/public/%s/%s/video/report",
             client->config.model_id, client->creds.device_id);
    len = mqtt_build_subscribe(buf, sizeof(buf), topic, 3);
    mqtt_ssl_send(client, buf, len);

    /* Read SUBACK responses */
    uint8_t suback[8];
    mqtt_ssl_recv(client, suback, sizeof(suback), 2);
    mqtt_ssl_recv(client, suback, sizeof(suback), 2);
    mqtt_ssl_recv(client, suback, sizeof(suback), 2);

    mqtt_log("Subscribed to video topics (model=%s)\n", client->config.model_id);
}

/* Disconnect from broker */
static void mqtt_disconnect(MQTTClient *client) {
    if (client->ssl) {
        uint8_t disconnect[] = { MQTT_DISCONNECT, 0x00 };
        mqtt_ssl_send(client, disconnect, sizeof(disconnect));

        SSL_shutdown((SSL *)client->ssl);
        SSL_free((SSL *)client->ssl);
        client->ssl = NULL;
    }

    if (client->ssl_ctx) {
        SSL_CTX_free((SSL_CTX *)client->ssl_ctx);
        client->ssl_ctx = NULL;
    }

    if (client->ssl_fd >= 0) {
        close(client->ssl_fd);
        client->ssl_fd = -1;
    }

    client->connected = 0;
}

/* MQTT client thread */
static void *mqtt_thread(void *arg) {
    MQTTClient *client = (MQTTClient *)arg;
    uint8_t recv_buf[4096];

    while (client->running) {
        MQTT_TIMING_START(total_iter);

        /* Connect if not connected */
        if (!client->connected) {
            if (mqtt_connect(client) != 0) {
                mqtt_log("Connection failed, retrying in 5s\n");
                sleep(5);
                continue;
            }
            mqtt_subscribe_topics(client);
        }

        /* Receive data with select() to avoid busy-wait */
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(client->ssl_fd, &read_fds);

        MQTT_TIMING_START(select_time);
        struct timeval tv = { .tv_sec = MQTT_RECV_TIMEOUT, .tv_usec = 0 };
        int sel_ret = select(client->ssl_fd + 1, &read_fds, NULL, NULL, &tv);
        MQTT_TIMING_END(select_time);

        if (sel_ret <= 0) {
            /* Timeout or error - check if we need to send keepalive */
            uint64_t now = get_time_ms();
            if (client->connected && (now - client->last_activity) >= (MQTT_KEEPALIVE_INTERVAL * 1000)) {
                /* Send PINGREQ to keep connection alive */
                uint8_t pingreq[] = { MQTT_PINGREQ, 0x00 };
                if (mqtt_ssl_send(client, pingreq, sizeof(pingreq)) >= 0) {
                    client->last_activity = now;
                }
            }
#ifdef ENCODER_TIMING
            MQTT_TIMING_END(total_iter);
            g_mqtt_timing.count++;
            MQTT_TIMING_LOG();
#endif
            continue;
        }

        /* Data available - try to read */
        MQTT_TIMING_START(ssl_read_time);
        int n = SSL_read((SSL *)client->ssl, recv_buf, sizeof(recv_buf));
        MQTT_TIMING_END(ssl_read_time);

        if (n > 0) {
            /* Update activity timestamp on receive */
            client->last_activity = get_time_ms();
            /* Process all packets in buffer */
            MQTT_TIMING_START(json_parse_time);
            size_t offset = 0;
            while (offset < (size_t)n) {
                int consumed = mqtt_handle_packet(client, recv_buf + offset, n - offset);
                if (consumed <= 0) break;
                offset += consumed;
            }
            MQTT_TIMING_END(json_parse_time);
        } else if (n <= 0) {
            int err = SSL_get_error((SSL *)client->ssl, n);
            if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
                /* Non-blocking would-block - sleep briefly and retry */
                usleep(10000);  /* 10ms */
                continue;
            } else if (err == SSL_ERROR_ZERO_RETURN) {
                /* Connection closed cleanly */
                mqtt_log("Connection closed by broker\n");
                mqtt_disconnect(client);
            } else {
                /* Real error */
                mqtt_log("Connection lost (err=%d), reconnecting...\n", err);
                mqtt_disconnect(client);
            }
        }

#ifdef ENCODER_TIMING
        MQTT_TIMING_END(total_iter);
        g_mqtt_timing.count++;
        MQTT_TIMING_LOG();
#endif
    }

    mqtt_disconnect(client);
    return NULL;
}

/* Start MQTT client */
int mqtt_client_start(void) {
    memset(&g_mqtt_client, 0, sizeof(g_mqtt_client));
    g_mqtt_client.ssl_fd = -1;

    /* Load credentials */
    if (json_load_mqtt_credentials(&g_mqtt_client.creds) != 0) {
        mqtt_log("Failed to load MQTT credentials\n");
        return -1;
    }

    if (json_load_device_config(&g_mqtt_client.config) != 0) {
        mqtt_log("Failed to load device config\n");
        return -1;
    }

    /* Generate client ID */
    snprintf(g_mqtt_client.client_id, sizeof(g_mqtt_client.client_id),
             "rkmpi_%08x", (unsigned)rand());

    pthread_mutex_init(&g_mqtt_client.mutex, NULL);
    g_mqtt_client.running = 1;
    g_mqtt_client.msgid_cleanup_time = get_time_ms();

    /* Initialize OpenSSL */
    SSL_library_init();
    SSL_load_error_strings();

    if (pthread_create(&g_mqtt_client.thread, NULL, mqtt_thread, &g_mqtt_client) != 0) {
        mqtt_log("Failed to create thread\n");
        return -1;
    }

    mqtt_log("Started (device=%.8s...)\n", g_mqtt_client.creds.device_id);
    return 0;
}

/* Stop MQTT client */
void mqtt_client_stop(void) {
    g_mqtt_client.running = 0;
    pthread_join(g_mqtt_client.thread, NULL);
    pthread_mutex_destroy(&g_mqtt_client.mutex);
    mqtt_log("Stopped\n");
}

/* Check if streaming is paused */
int mqtt_is_streaming_paused(void) {
    return g_mqtt_client.streaming_paused;
}

#else /* !TLS_AVAILABLE */

/* Stub implementations when TLS is not available */

int mqtt_client_start(void) {
    mqtt_log("TLS not available - MQTT client disabled\n");
    mqtt_log("Build with -DHAVE_OPENSSL and link with -lssl -lcrypto to enable\n");
    return -1;
}

void mqtt_client_stop(void) {
    /* Nothing to do */
}

int mqtt_is_streaming_paused(void) {
    return 0;
}

#endif /* TLS_AVAILABLE */
