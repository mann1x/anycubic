/*
 * MQTT Client for Video Responder
 *
 * Subscribes to video topics and responds to startCapture/stopCapture commands
 * to keep the slicer connection alive when gkcam is not running.
 */

#ifndef MQTT_CLIENT_H
#define MQTT_CLIENT_H

#include <stdint.h>
#include <pthread.h>
#include "json_util.h"

/* MQTT broker settings */
#define MQTT_HOST           "127.0.0.1"
#define MQTT_PORT           9883
#define MQTT_TIMEOUT_SEC    10
#define MQTT_RECV_TIMEOUT   1  /* 1 second recv timeout for loop */

/* MQTT packet types */
#define MQTT_CONNECT    0x10
#define MQTT_CONNACK    0x20
#define MQTT_PUBLISH    0x30
#define MQTT_PUBACK     0x40
#define MQTT_SUBSCRIBE  0x82
#define MQTT_SUBACK     0x90
#define MQTT_DISCONNECT 0xE0

/* Message ID tracking */
#define MQTT_MAX_MSGIDS  64

/* MQTT client state */
typedef struct {
    int ssl_fd;                 /* SSL socket file descriptor (or -1) */
    void *ssl_ctx;              /* SSL context (OpenSSL SSL_CTX*) */
    void *ssl;                  /* SSL connection (OpenSSL SSL*) */
    MQTTCredentials creds;
    DeviceConfig config;
    char client_id[32];
    volatile int running;
    volatile int connected;
    volatile int streaming_paused;  /* Set by stopCapture, cleared by startCapture */
    pthread_t thread;
    pthread_mutex_t mutex;
    /* Message deduplication */
    char handled_msgids[MQTT_MAX_MSGIDS][40];
    int msgid_count;
    uint64_t msgid_cleanup_time;
} MQTTClient;

/* Global MQTT client instance */
extern MQTTClient g_mqtt_client;

/* Initialize and start MQTT client thread */
int mqtt_client_start(void);

/* Stop MQTT client */
void mqtt_client_stop(void);

/* Check if streaming is paused (stopCapture received) */
int mqtt_is_streaming_paused(void);

#endif /* MQTT_CLIENT_H */
