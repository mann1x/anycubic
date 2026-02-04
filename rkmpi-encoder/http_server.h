/*
 * HTTP Server for MJPEG and FLV Streaming
 *
 * Provides two HTTP servers:
 * - MJPEG server on port 8080: /stream (multipart), /snapshot (single JPEG)
 * - FLV server on port 18088: /flv (H.264 in FLV container)
 *
 * Uses select() for non-blocking I/O with multiple clients.
 */

#ifndef HTTP_SERVER_H
#define HTTP_SERVER_H

#include <stdint.h>
#include <stddef.h>
#include <pthread.h>

/* Server ports */
#define HTTP_MJPEG_PORT  8080
#define HTTP_FLV_PORT    18088

/* Limits */
#define HTTP_MAX_CLIENTS      24
#define HTTP_RECV_BUF_SIZE    4096
#define HTTP_SEND_BUF_SIZE    (512 * 1024)

/* Idle connection timeout (seconds) - close connections that don't send a request */
#define HTTP_IDLE_TIMEOUT_SEC 10

/* MJPEG boundary string */
#define MJPEG_BOUNDARY  "mjpegstream"

/* Client state */
typedef enum {
    CLIENT_STATE_IDLE,          /* Waiting for request */
    CLIENT_STATE_STREAMING,     /* Actively streaming */
    CLIENT_STATE_CLOSING        /* Marked for close */
} ClientState;

/* Default control port */
#define HTTP_CONTROL_PORT 8081

/* Client request type */
typedef enum {
    REQUEST_NONE,
    REQUEST_MJPEG_STREAM,       /* /stream */
    REQUEST_MJPEG_SNAPSHOT,     /* /snapshot */
    REQUEST_FLV_STREAM,         /* /flv */
    REQUEST_DISPLAY_STREAM,     /* /display */
    REQUEST_DISPLAY_SNAPSHOT,   /* /display/snapshot */
    REQUEST_HOMEPAGE            /* / */
} RequestType;

/* Connection warmup to prevent CPU spikes */
#define CLIENT_WARMUP_FRAMES  15  /* Frames with throttling during warmup */
#define CLIENT_WARMUP_DELAY_MS 30 /* Delay per frame during warmup (ms) */

/* Client connection */
typedef struct {
    int fd;
    ClientState state;
    RequestType request;
    uint64_t last_frame_seq;    /* Last frame sequence sent */
    uint64_t connect_time;      /* Connection timestamp */
    uint8_t *send_buf;          /* Send buffer for FLV */
    size_t send_buf_size;
    size_t send_buf_pos;        /* Current position in send buffer */
    int header_sent;            /* Have we sent HTTP headers? */
    int frames_sent;            /* Frames sent to this client (for warmup) */
} HttpClient;

/* HTTP server instance */
typedef struct {
    int listen_fd;
    int port;
    HttpClient clients[HTTP_MAX_CLIENTS];
    int client_count;
    pthread_mutex_t mutex;
    volatile int running;
} HttpServer;

/* MJPEG server thread data */
typedef struct {
    HttpServer server;
    pthread_t thread;
    volatile int running;
} MjpegServerThread;

/* FLV server thread data */
typedef struct {
    HttpServer server;
    pthread_t thread;
    volatile int running;
    int width;
    int height;
    int fps;
} FlvServerThread;

/* Global server instances */
extern MjpegServerThread g_mjpeg_server;
extern FlvServerThread g_flv_server;

/* Initialize and start MJPEG server (port 0 = use default HTTP_MJPEG_PORT) */
int mjpeg_server_start(int port);
void mjpeg_server_stop(void);

/* Initialize and start FLV server */
int flv_server_start(int width, int height, int fps);
void flv_server_stop(void);

/* Get client counts (for stats) */
int mjpeg_server_client_count(void);
int flv_server_client_count(void);

/* Set control port for homepage links (0 = use default HTTP_CONTROL_PORT) */
void http_set_control_port(int port);

/* Check if H.264 encoding is enabled (implemented in rkmpi_enc.c) */
int is_h264_enabled(void);

#endif /* HTTP_SERVER_H */
