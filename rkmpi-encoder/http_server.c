/*
 * HTTP Server Implementation
 *
 * Minimal socket-based HTTP server for MJPEG and FLV streaming.
 */

#define _GNU_SOURCE
#include "http_server.h"
#include "frame_buffer.h"
#include "flv_mux.h"
#include "display_capture.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <sys/uio.h>
#include <time.h>

/* Global server instances */
MjpegServerThread g_mjpeg_server;
FlvServerThread g_flv_server;

/* Control port for homepage links (default: 8081) */
static int g_control_port = HTTP_CONTROL_PORT;

/* FLV proxy state */
static char g_flv_proxy_url[256] = "";
static pthread_mutex_t g_flv_proxy_lock = PTHREAD_MUTEX_INITIALIZER;

/* FLV proxy FPS tracking */
static volatile float g_flv_proxy_fps = 0.0f;

/* Forward declarations for FLV proxy */
typedef struct {
    int client_fd;
    char url[256];
} FlvProxyArg;
static void *flv_proxy_thread(void *arg);

/* FLV tag parser state machine for counting video frames */
enum {
    FLV_PARSE_HEADER,       /* Skipping 9-byte FLV header + 4-byte prev tag size */
    FLV_PARSE_TAG_HEADER,   /* Reading 11-byte tag header */
    FLV_PARSE_TAG_DATA,     /* Skipping tag data */
    FLV_PARSE_PREV_SIZE     /* Skipping 4-byte previous tag size */
};

typedef struct {
    int state;
    int bytes_left;
    uint8_t tag_hdr[11];
    int tag_hdr_pos;
    /* Stats */
    int video_frames;
    struct timespec last_time;
} FlvTagCounter;

static void flv_counter_init(FlvTagCounter *c) {
    memset(c, 0, sizeof(*c));
    c->state = FLV_PARSE_HEADER;
    c->bytes_left = 9 + 4;  /* FLV header (9) + first PreviousTagSize0 (4) */
    clock_gettime(CLOCK_MONOTONIC, &c->last_time);
}

/* Process buffer through FLV tag counter, update g_flv_proxy_fps */
static void flv_count_tags(FlvTagCounter *c, const uint8_t *buf, int len) {
    int i = 0;
    while (i < len) {
        switch (c->state) {
        case FLV_PARSE_HEADER:
        case FLV_PARSE_TAG_DATA:
        case FLV_PARSE_PREV_SIZE: {
            /* Skip bytes_left bytes */
            int skip = len - i;
            if (skip > c->bytes_left) skip = c->bytes_left;
            i += skip;
            c->bytes_left -= skip;
            if (c->bytes_left == 0) {
                if (c->state == FLV_PARSE_TAG_DATA) {
                    c->state = FLV_PARSE_PREV_SIZE;
                    c->bytes_left = 4;
                } else {
                    /* After header or prev_size, expect tag header */
                    c->state = FLV_PARSE_TAG_HEADER;
                    c->tag_hdr_pos = 0;
                }
            }
            break;
        }
        case FLV_PARSE_TAG_HEADER: {
            /* Collect 11-byte tag header */
            int need = 11 - c->tag_hdr_pos;
            int avail = len - i;
            if (avail > need) avail = need;
            memcpy(c->tag_hdr + c->tag_hdr_pos, buf + i, avail);
            c->tag_hdr_pos += avail;
            i += avail;
            if (c->tag_hdr_pos == 11) {
                /* Tag type is first byte: 0x09 = video */
                if (c->tag_hdr[0] == 0x09)
                    c->video_frames++;
                /* Data size is bytes 1-3 (big-endian) */
                uint32_t data_size = ((uint32_t)c->tag_hdr[1] << 16) |
                                     ((uint32_t)c->tag_hdr[2] << 8) |
                                     (uint32_t)c->tag_hdr[3];
                c->state = FLV_PARSE_TAG_DATA;
                c->bytes_left = (int)data_size;
                if (c->bytes_left == 0) {
                    c->state = FLV_PARSE_PREV_SIZE;
                    c->bytes_left = 4;
                }
            }
            break;
        }
        }
    }

    /* Compute FPS every 2 seconds */
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    double elapsed = (now.tv_sec - c->last_time.tv_sec) +
                     (now.tv_nsec - c->last_time.tv_nsec) / 1e9;
    if (elapsed >= 2.0) {
        g_flv_proxy_fps = (float)(c->video_frames / elapsed);
        c->video_frames = 0;
        c->last_time = now;
    }
}

void http_set_control_port(int port) {
    g_control_port = (port > 0) ? port : HTTP_CONTROL_PORT;
}

/*
 * Timing instrumentation - enable with -DENCODER_TIMING
 */
#ifdef ENCODER_TIMING
#define HTTP_TIMING_INTERVAL 500  /* Log every N iterations */

typedef struct {
    uint64_t select_time;     /* Time in select() */
    uint64_t fb_copy_time;    /* Frame buffer copy */
    uint64_t net_send_time;   /* Network send */
    uint64_t total_iter;      /* Total iteration time */
    int count;
} HttpTiming;

static HttpTiming g_mjpeg_timing = {0};
static HttpTiming g_flv_timing = {0};

#define HTTP_TIMING_START(var) uint64_t _ht_##var = get_time_us()
#define HTTP_TIMING_END(timing, field) (timing)->field += get_time_us() - _ht_##field
#define HTTP_TIMING_LOG(name, t) do { \
    if ((t)->count >= HTTP_TIMING_INTERVAL) { \
        double n = (double)(t)->count; \
        fprintf(stderr, "[HTTP %s] iters=%d avg(us): select=%.1f fb_copy=%.1f send=%.1f total=%.1f\n", \
                name, (t)->count, \
                (t)->select_time / n, \
                (t)->fb_copy_time / n, \
                (t)->net_send_time / n, \
                (t)->total_iter / n); \
        memset((t), 0, sizeof(*(t))); \
    } \
} while(0)
#else
#define HTTP_TIMING_START(var) (void)0
#define HTTP_TIMING_END(timing, field) (void)0
#define HTTP_TIMING_LOG(name, t) (void)0
#endif

/* Logging (use external if available) */
extern int g_verbose;
static void log_info(const char *fmt, ...) {
    if (g_verbose) {
        va_list args;
        va_start(args, fmt);
        vfprintf(stderr, fmt, args);
        va_end(args);
        fflush(stderr);
    }
}

static uint64_t get_time_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000 + (uint64_t)ts.tv_nsec / 1000;
}

/* Set socket non-blocking */
static int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

/* Initialize HTTP server */
static int http_server_init(HttpServer *srv, int port) {
    memset(srv, 0, sizeof(*srv));
    srv->port = port;
    pthread_mutex_init(&srv->mutex, NULL);

    /* Create listen socket */
    srv->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (srv->listen_fd < 0) {
        fprintf(stderr, "HTTP: socket() failed: %s\n", strerror(errno));
        return -1;
    }

    /* Allow address reuse */
    int opt = 1;
    setsockopt(srv->listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    /* Bind */
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (bind(srv->listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "HTTP: bind(%d) failed: %s\n", port, strerror(errno));
        close(srv->listen_fd);
        return -1;
    }

    /* Listen */
    if (listen(srv->listen_fd, 8) < 0) {
        fprintf(stderr, "HTTP: listen() failed: %s\n", strerror(errno));
        close(srv->listen_fd);
        return -1;
    }

    set_nonblocking(srv->listen_fd);
    srv->running = 1;

    log_info("HTTP: Server listening on port %d\n", port);
    return 0;
}

/* Cleanup HTTP server */
static void http_server_cleanup(HttpServer *srv) {
    srv->running = 0;

    /* Close all clients */
    for (int i = 0; i < HTTP_MAX_CLIENTS; i++) {
        if (srv->clients[i].fd > 0) {
            close(srv->clients[i].fd);
            srv->clients[i].fd = 0;
            if (srv->clients[i].send_buf) {
                free(srv->clients[i].send_buf);
                srv->clients[i].send_buf = NULL;
            }
        }
    }

    if (srv->listen_fd > 0) {
        close(srv->listen_fd);
        srv->listen_fd = 0;
    }

    pthread_mutex_destroy(&srv->mutex);
}

/* Accept new client */
static void http_server_accept(HttpServer *srv) {
    struct sockaddr_in client_addr;
    socklen_t addr_len = sizeof(client_addr);

    int client_fd = accept(srv->listen_fd, (struct sockaddr *)&client_addr, &addr_len);
    if (client_fd < 0) {
        return;
    }

    /* Find free slot */
    int slot = -1;
    for (int i = 0; i < HTTP_MAX_CLIENTS; i++) {
        if (srv->clients[i].fd == 0) {
            slot = i;
            break;
        }
    }

    if (slot < 0) {
        /* No free slots */
        close(client_fd);
        log_info("HTTP: Rejected client (max connections)\n");
        return;
    }

    /* Setup client */
    set_nonblocking(client_fd);

    /* Disable Nagle's algorithm for lower latency */
    int opt = 1;
    setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));

    /* Send buffer sized for streaming frames (~155KB).
     * Kernel doubles this to 512KB effective. */
    int sndbuf = 256 * 1024;
    setsockopt(client_fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));

    HttpClient *client = &srv->clients[slot];
    memset(client, 0, sizeof(*client));
    client->fd = client_fd;
    client->state = CLIENT_STATE_IDLE;
    client->request = REQUEST_NONE;
    client->connect_time = get_time_us();

    srv->client_count++;
    log_info("HTTP[%d]: Client connected from %s (slot %d)\n",
             srv->port, inet_ntoa(client_addr.sin_addr), slot);
}

/* Parse HTTP request */
static RequestType parse_http_request(const char *buf, size_t len, int port) {
    if (len < 10) return REQUEST_NONE;

    /* Check for GET request */
    if (strncmp(buf, "GET ", 4) != 0) {
        return REQUEST_NONE;
    }

    /* Find path */
    const char *path = buf + 4;
    const char *path_end = strchr(path, ' ');
    if (!path_end) return REQUEST_NONE;

    size_t path_len = path_end - path;

    /* Use actual MJPEG server port for routing (supports custom ports) */
    int mjpeg_port = g_mjpeg_server.server.port > 0 ? g_mjpeg_server.server.port : HTTP_MJPEG_PORT;

    if (port == mjpeg_port) {
        /* Homepage */
        if (path_len == 1 && path[0] == '/') {
            return REQUEST_HOMEPAGE;
        }
        if (path_len >= 7 && strncmp(path, "/stream", 7) == 0) {
            return REQUEST_MJPEG_STREAM;
        }
        if (path_len >= 9 && strncmp(path, "/snapshot", 9) == 0) {
            return REQUEST_MJPEG_SNAPSHOT;
        }
        /* Display capture endpoints */
        if (path_len >= 17 && strncmp(path, "/display/snapshot", 17) == 0) {
            return REQUEST_DISPLAY_SNAPSHOT;
        }
        if (path_len >= 8 && strncmp(path, "/display", 8) == 0) {
            return REQUEST_DISPLAY_STREAM;
        }
    } else if (port == HTTP_FLV_PORT) {
        if (path_len >= 4 && strncmp(path, "/flv", 4) == 0) {
            return REQUEST_FLV_STREAM;
        }
    }

    return REQUEST_NONE;
}

/* Send HTTP response */
static int http_send(int fd, const void *data, size_t len) {
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

/* Blocking streaming send via writev.
 * Returns: 0=sent, -1=error (close client) */
static int streaming_sendv(int fd, struct iovec *iov, int iovcnt) {
    while (iovcnt > 0) {
        ssize_t n = writev(fd, iov, iovcnt);
        if (n < 0)
            return -1;
        while (iovcnt > 0 && (size_t)n >= iov->iov_len) {
            n -= iov->iov_len;
            iov++;
            iovcnt--;
        }
        if (iovcnt > 0 && n > 0) {
            iov->iov_base = (char *)iov->iov_base + n;
            iov->iov_len -= n;
        }
    }
    return 0;
}

/* Send 404 response */
static void http_send_404(int fd) {
    const char *response =
        "HTTP/1.1 404 Not Found\r\n"
        "Content-Type: text/plain\r\n"
        "Content-Length: 9\r\n"
        "\r\n"
        "Not Found";
    http_send(fd, response, strlen(response));
}

/* Send 503 Service Unavailable response with custom message */
static void http_send_503(int fd, const char *message) {
    char response[512];
    size_t msg_len = strlen(message);
    int len = snprintf(response, sizeof(response),
        "HTTP/1.1 503 Service Unavailable\r\n"
        "Content-Type: text/plain\r\n"
        "Content-Length: %zu\r\n"
        "\r\n"
        "%s", msg_len, message);
    http_send(fd, response, len);
}

/* Send homepage */
static void http_send_homepage(int fd, int streaming_port) {
    int control_port = g_control_port;

    /* Build HTML with embedded JavaScript for dynamic URLs */
    char html[8192];
    int len = snprintf(html, sizeof(html),
        "<!DOCTYPE html><html><head><title>H264 Streamer</title>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<style>"
        "body{font-family:sans-serif;margin:20px;background:#1a1a1a;color:#fff}"
        ".container{max-width:800px;margin:0 auto}"
        "h1{color:#4CAF50;margin-bottom:5px}"
        ".subtitle{color:#888;margin-bottom:20px}"
        ".section{background:#2d2d2d;padding:15px;margin:15px 0;border-radius:8px}"
        ".section h2{margin:0 0 10px 0;color:#888;font-size:14px;text-transform:uppercase}"
        "button{background:#4CAF50;color:#fff;border:none;padding:10px 20px;border-radius:4px;cursor:pointer;font-size:14px;margin:2px}"
        "button:hover{background:#45a049}"
        "button.secondary{background:#555}"
        "button.secondary:hover{background:#666}"
        ".stream-row{display:flex;align-items:center;margin:8px 0;padding:8px;background:#222;border-radius:4px}"
        ".stream-url{flex:1;font-family:monospace;font-size:13px;color:#4CAF50;word-break:break-all}"
        ".stream-btns{display:flex;gap:5px}"
        ".stream-btns button{padding:5px 10px;font-size:12px}"
        ".copy-btn{background:#444;padding:5px 8px !important}"
        ".copy-btn:hover{background:#555}"
        ".endpoint-row{display:flex;margin:6px 0;padding:6px 0;border-bottom:1px solid #333}"
        ".endpoint-path{font-family:monospace;color:#4CAF50;min-width:180px}"
        ".endpoint-desc{color:#aaa;font-size:13px}"
        ".copied{background:#2e7d32 !important}"
        "</style></head><body>"
        "<div class='container'>"
        "<h1>H264 Streamer</h1>"
        "<p class='subtitle'>HTTP streaming server for Anycubic printers</p>"

        /* Control Panel Section */
        "<div class='section'>"
        "<h2>Control Panel</h2>"
        "<p style='color:#aaa;margin:0 0 10px 0;font-size:14px'>Configure streaming settings, camera controls, and preview video.</p>"
        "<button onclick='openControl()'>Open Control Panel</button>"
        "</div>"

        /* Streams Section */
        "<div class='section'>"
        "<h2>Video Streams</h2>"
        "<div id='streams'></div>"
        "</div>"

        /* Endpoints Section */
        "<div class='section'>"
        "<h2>API Endpoints</h2>"
        "<p style='color:#888;font-size:12px;margin:0 0 10px 0'>Available on control port (<span id='cp'>%d</span>)</p>"
        "<div class='endpoint-row'><span class='endpoint-path'>/control</span><span class='endpoint-desc'>Web control panel with settings and preview</span></div>"
        "<div class='endpoint-row'><span class='endpoint-path'>/api/stats</span><span class='endpoint-desc'>JSON stats (FPS, CPU, clients)</span></div>"
        "<div class='endpoint-row'><span class='endpoint-path'>/api/config</span><span class='endpoint-desc'>JSON full running configuration</span></div>"
        "<div class='endpoint-row'><span class='endpoint-path'>/status</span><span class='endpoint-desc'>Plain text status summary</span></div>"
        "<div class='endpoint-row'><span class='endpoint-path'>/timelapse</span><span class='endpoint-desc'>Timelapse management page</span></div>"
        "<div class='endpoint-row'><span class='endpoint-path'>/api/timelapse/list</span><span class='endpoint-desc'>JSON list of timelapse recordings</span></div>"
        "<div class='endpoint-row'><span class='endpoint-path'>/api/camera/controls</span><span class='endpoint-desc'>JSON camera controls with ranges</span></div>"
        "<div class='endpoint-row'><span class='endpoint-path'>/api/touch</span><span class='endpoint-desc'>POST touch events to printer LCD</span></div>"
        "</div>"
        "</div>"

        /* JavaScript */
        "<script>"
        "var sp=%d,cp=%d;"
        "var host=location.hostname;"
        "var streamBase='http://'+host+':'+sp;"
        "var ctrlBase='http://'+host+':'+cp;"
        "function openControl(){window.open(ctrlBase+'/control','_blank')}"
        "function openStream(url){window.open(url,'_blank')}"
        "function copyText(text,btn){"
        "var ta=document.createElement('textarea');"
        "ta.value=text;ta.style.position='fixed';ta.style.left='-9999px';"
        "document.body.appendChild(ta);ta.select();"
        "try{document.execCommand('copy');"
        "btn.classList.add('copied');btn.textContent='Copied!';"
        "setTimeout(function(){btn.classList.remove('copied');btn.textContent='Copy'},1500)"
        "}catch(e){alert('Copy failed: '+text)}"
        "document.body.removeChild(ta)}"
        "function addStream(name,url){"
        "var d=document.getElementById('streams');"
        "var r=document.createElement('div');r.className='stream-row';"
        "var u=encodeURIComponent(url);"
        "r.innerHTML='<span class=\"stream-url\">'+url+'</span>"
        "<div class=\"stream-btns\"><button onclick=\"openStream(decodeURIComponent(\\''+u+'\\'))\" class=\"secondary\">Open</button>"
        "<button onclick=\"copyText(decodeURIComponent(\\''+u+'\\'),this)\" class=\"copy-btn\">Copy</button></div>';"
        "d.appendChild(r)}"
        "addStream('MJPEG Stream',streamBase+'/stream');"
        "addStream('Snapshot',streamBase+'/snapshot');"
        "addStream('H.264 FLV','http://'+host+':18088/flv');"
        "addStream('Display Stream',streamBase+'/display');"
        "addStream('Display Snapshot',streamBase+'/display/snapshot');"
        "</script></body></html>",
        control_port, streaming_port, control_port
    );

    char headers[256];
    int hlen = snprintf(headers, sizeof(headers),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/html; charset=utf-8\r\n"
        "Content-Length: %d\r\n"
        "Cache-Control: no-cache\r\n"
        "Connection: close\r\n"
        "\r\n", len);

    http_send(fd, headers, hlen);
    http_send(fd, html, len);
}

/* Send MJPEG stream headers */
static void http_send_mjpeg_headers(int fd) {
    char headers[256];
    int len = snprintf(headers, sizeof(headers),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: multipart/x-mixed-replace; boundary=" MJPEG_BOUNDARY "\r\n"
        "Cache-Control: no-cache\r\n"
        "Connection: close\r\n"
        "\r\n");
    http_send(fd, headers, len);
}

/* External functions from rkmpi_enc.c for snapshot support */
extern void request_camera_snapshot(void);
extern int is_snapshot_pending(void);

/* Send single JPEG snapshot (camera)
 * If no clients are connected, the capture loop may be idle.
 * Request a snapshot and wait for it.
 */
static void http_send_snapshot(int fd) {
    /* First try to get an existing frame */
    uint64_t cur_seq = frame_buffer_get_sequence(&g_jpeg_buffer);
    uint64_t cur_ts = 0;
    uint8_t *jpeg_buf = malloc(FRAME_BUFFER_MAX_JPEG);
    if (!jpeg_buf) {
        http_send_404(fd);
        return;
    }

    size_t jpeg_size = frame_buffer_copy(&g_jpeg_buffer, jpeg_buf,
                                          FRAME_BUFFER_MAX_JPEG, NULL, &cur_ts, NULL);

    /* If we have a recent frame (< 2 seconds old), use it */
    uint64_t now = get_time_us();
    if (jpeg_size > 0 && cur_ts > 0 && (now - cur_ts) < 2000000) {
        char headers[256];
        int hlen = snprintf(headers, sizeof(headers),
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: image/jpeg\r\n"
            "Content-Length: %zu\r\n"
            "Cache-Control: no-cache\r\n"
            "Connection: close\r\n"
            "\r\n", jpeg_size);
        http_send(fd, headers, hlen);
        http_send(fd, jpeg_buf, jpeg_size);
        free(jpeg_buf);
        return;
    }

    /* No recent frame - request capture and wait */
    request_camera_snapshot();

    int waited_ms = 0;
    const int max_wait_ms = 3000;  /* 3 second timeout */
    const int poll_ms = 50;

    while (waited_ms < max_wait_ms) {
        usleep(poll_ms * 1000);
        waited_ms += poll_ms;

        /* Check if new frame is available */
        uint64_t new_seq = frame_buffer_get_sequence(&g_jpeg_buffer);
        if (new_seq > cur_seq) {
            jpeg_size = frame_buffer_copy(&g_jpeg_buffer, jpeg_buf,
                                          FRAME_BUFFER_MAX_JPEG, NULL, NULL, NULL);
            if (jpeg_size > 0) break;
        }
    }

    if (jpeg_size > 0) {
        char headers[256];
        int hlen = snprintf(headers, sizeof(headers),
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: image/jpeg\r\n"
            "Content-Length: %zu\r\n"
            "Cache-Control: no-cache\r\n"
            "Connection: close\r\n"
            "\r\n", jpeg_size);
        http_send(fd, headers, hlen);
        http_send(fd, jpeg_buf, jpeg_size);
    } else {
        http_send_404(fd);
    }

    free(jpeg_buf);
}

/* Send single JPEG snapshot (display)
 * Since display capture only runs when clients are connected,
 * we need to trigger capture and wait for a frame.
 */
static void http_send_display_snapshot(int fd) {
    /* Trigger capture by incrementing client count */
    display_client_connect();

    /* Wait for a frame (up to 5 seconds) */
    uint8_t *jpeg_buf = malloc(FRAME_BUFFER_MAX_DISPLAY);
    if (!jpeg_buf) {
        display_client_disconnect();
        http_send_404(fd);
        return;
    }

    size_t jpeg_size = 0;
    int waited_ms = 0;
    const int max_wait_ms = 5000;
    const int poll_ms = 100;

    /* Get initial sequence to detect new frames */
    uint64_t start_seq = frame_buffer_get_sequence(&g_display_buffer);

    /* Wait for a new frame (sequence must increase) */
    while (waited_ms < max_wait_ms) {
        usleep(poll_ms * 1000);
        waited_ms += poll_ms;

        uint64_t cur_seq = frame_buffer_get_sequence(&g_display_buffer);
        if (cur_seq > start_seq) {
            /* New frame available - copy it */
            jpeg_size = frame_buffer_copy(&g_display_buffer, jpeg_buf,
                                         FRAME_BUFFER_MAX_DISPLAY, NULL, NULL, NULL);
            if (jpeg_size > 0) break;
        }
    }

    display_client_disconnect();

    if (jpeg_size > 0) {
        char headers[256];
        int hlen = snprintf(headers, sizeof(headers),
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: image/jpeg\r\n"
            "Content-Length: %zu\r\n"
            "Cache-Control: no-cache\r\n"
            "Connection: close\r\n"
            "\r\n", jpeg_size);
        http_send(fd, headers, hlen);
        http_send(fd, jpeg_buf, jpeg_size);
    } else {
        http_send_404(fd);
    }

    free(jpeg_buf);
}

/* Send FLV stream headers (gkcam-compatible) */
static void http_send_flv_headers(int fd) {
    /* Must match gkcam exactly for slicer compatibility */
    const char *headers =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/plain\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Content-Length: 99999999999\r\n"
        "\r\n";
    http_send(fd, headers, strlen(headers));
}

/* Handle client read (for request parsing) */
static void http_handle_client_read(HttpServer *srv, HttpClient *client) {
    char buf[HTTP_RECV_BUF_SIZE];
    ssize_t n = recv(client->fd, buf, sizeof(buf) - 1, 0);

    if (n <= 0) {
        client->state = CLIENT_STATE_CLOSING;
        return;
    }

    buf[n] = '\0';

    if (client->state == CLIENT_STATE_IDLE) {
        RequestType req = parse_http_request(buf, n, srv->port);

        if (req == REQUEST_NONE) {
            http_send_404(client->fd);
            client->state = CLIENT_STATE_CLOSING;
            return;
        }

        client->request = req;

        switch (req) {
            case REQUEST_MJPEG_STREAM:
                http_send_mjpeg_headers(client->fd);
                client->state = CLIENT_STATE_STREAMING;
                client->header_sent = 1;
                /* Skip stale frame in buffer - wait for next fresh frame */
                client->last_frame_seq = frame_buffer_get_sequence(&g_jpeg_buffer);
                log_info("HTTP[%d]: MJPEG stream started\n", srv->port);
                break;

            case REQUEST_MJPEG_SNAPSHOT:
                http_send_snapshot(client->fd);
                client->state = CLIENT_STATE_CLOSING;
                break;

            case REQUEST_DISPLAY_STREAM:
                http_send_mjpeg_headers(client->fd);
                client->state = CLIENT_STATE_STREAMING;
                client->header_sent = 1;
                /* Skip stale frame in buffer - wait for next fresh frame */
                client->last_frame_seq = frame_buffer_get_sequence(&g_display_buffer);
                display_client_connect();
                log_info("HTTP[%d]: Display stream started\n", srv->port);
                break;

            case REQUEST_DISPLAY_SNAPSHOT:
                http_send_display_snapshot(client->fd);
                client->state = CLIENT_STATE_CLOSING;
                break;

            case REQUEST_FLV_STREAM:
                /* Check for FLV proxy mode first */
                if (!is_h264_enabled() && flv_proxy_is_active()) {
                    /* Proxy mode: hand off fd to proxy thread */
                    FlvProxyArg *pa = malloc(sizeof(FlvProxyArg));
                    if (pa) {
                        pa->client_fd = client->fd;
                        pthread_mutex_lock(&g_flv_proxy_lock);
                        strncpy(pa->url, g_flv_proxy_url, sizeof(pa->url) - 1);
                        pa->url[sizeof(pa->url) - 1] = '\0';
                        pthread_mutex_unlock(&g_flv_proxy_lock);

                        pthread_t pt;
                        pthread_attr_t attr;
                        pthread_attr_init(&attr);
                        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
                        if (pthread_create(&pt, &attr, flv_proxy_thread, pa) == 0) {
                            pthread_setname_np(pt, "flv_proxy");
                            /* fd ownership transferred to proxy thread */
                            client->fd = -1;
                            client->state = CLIENT_STATE_CLOSING;
                            log_info("HTTP[%d]: FLV proxy started\n", srv->port);
                        } else {
                            free(pa);
                            http_send_503(client->fd, "FLV proxy thread failed");
                            client->state = CLIENT_STATE_CLOSING;
                        }
                        pthread_attr_destroy(&attr);
                    } else {
                        http_send_503(client->fd, "Out of memory");
                        client->state = CLIENT_STATE_CLOSING;
                    }
                    break;
                }
                /* Reject FLV connections if H.264 is disabled and no proxy */
                if (!is_h264_enabled()) {
                    http_send_503(client->fd, "H.264 encoding is disabled");
                    client->state = CLIENT_STATE_CLOSING;
                    break;
                }
                http_send_flv_headers(client->fd);
                client->state = CLIENT_STATE_STREAMING;
                client->header_sent = 1;
                /* Set to 0 so FLV thread knows to send FLV header/metadata
                 * The FLV thread will then set it to current seq after sending headers */
                client->last_frame_seq = 0;

                /* Allocate send buffer for FLV */
                client->send_buf = malloc(HTTP_SEND_BUF_SIZE);
                client->send_buf_size = HTTP_SEND_BUF_SIZE;
                client->send_buf_pos = 0;
                log_info("HTTP[%d]: FLV stream started\n", srv->port);
                break;

            case REQUEST_HOMEPAGE:
                http_send_homepage(client->fd, srv->port);
                client->state = CLIENT_STATE_CLOSING;
                break;

            default:
                http_send_404(client->fd);
                client->state = CLIENT_STATE_CLOSING;
                break;
        }
    }
}

/* Close client and cleanup */
static void http_close_client(HttpServer *srv, int slot) {
    HttpClient *client = &srv->clients[slot];

    /* Track display client disconnect */
    if (client->request == REQUEST_DISPLAY_STREAM) {
        display_client_disconnect();
    }

    if (client->fd > 0) {
        close(client->fd);
        log_info("HTTP[%d]: Client disconnected (slot %d)\n", srv->port, slot);
    }

    if (client->send_buf) {
        free(client->send_buf);
    }

    memset(client, 0, sizeof(*client));
    srv->client_count--;
}

/* ============================================================================
 * MJPEG Server Thread
 * ============================================================================ */

/* Check for new connections and requests (non-blocking, returns immediately) */
static void mjpeg_check_connections(HttpServer *srv) {
    fd_set read_fds;
    FD_ZERO(&read_fds);
    FD_SET(srv->listen_fd, &read_fds);
    int max_fd = srv->listen_fd;

    for (int i = 0; i < HTTP_MAX_CLIENTS; i++) {
        if (srv->clients[i].fd > 0 && srv->clients[i].state == CLIENT_STATE_IDLE) {
            FD_SET(srv->clients[i].fd, &read_fds);
            if (srv->clients[i].fd > max_fd) max_fd = srv->clients[i].fd;
        }
    }

    struct timeval tv = { .tv_sec = 0, .tv_usec = 0 };  /* immediate return */
    int ret = select(max_fd + 1, &read_fds, NULL, NULL, &tv);
    if (ret <= 0) return;

    if (FD_ISSET(srv->listen_fd, &read_fds))
        http_server_accept(srv);

    for (int i = 0; i < HTTP_MAX_CLIENTS; i++) {
        if (srv->clients[i].fd > 0 && FD_ISSET(srv->clients[i].fd, &read_fds))
            http_handle_client_read(srv, &srv->clients[i]);
    }
}

/* Switch streaming client to blocking mode with send timeout.
 * Disable TCP_NODELAY (enable Nagle) for streaming — with TCP_CORK
 * per frame, the kernel batches segments optimally. */
static void make_streaming_socket(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0)
        fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);

    struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    /* Disable TCP_NODELAY for streaming (was set in accept for request parsing).
     * With TCP_CORK per frame, this lets the kernel batch optimally. */
    int off = 0;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &off, sizeof(off));
}

static void *mjpeg_server_thread(void *arg) {
    MjpegServerThread *st = (MjpegServerThread *)arg;
    HttpServer *srv = &st->server;

    uint8_t *camera_buf = malloc(FRAME_BUFFER_MAX_JPEG);
    uint8_t *display_buf = malloc(FRAME_BUFFER_MAX_DISPLAY);
    char header_buf[256];

    uint64_t last_camera_seq = 0;
    uint64_t last_display_seq = 0;

    while (st->running && srv->running) {
        HTTP_TIMING_START(total_iter);

        /* 1. Check for new connections and client requests (non-blocking) */
        HTTP_TIMING_START(select_time);
        mjpeg_check_connections(srv);
        HTTP_TIMING_END(&g_mjpeg_timing, select_time);

        /* Count streaming clients and switch new ones to blocking mode */
        int has_camera_clients = 0;
        int has_display_clients = 0;
        uint64_t now = get_time_us();

        for (int i = 0; i < HTTP_MAX_CLIENTS; i++) {
            HttpClient *client = &srv->clients[i];

            if (client->fd > 0 && client->state == CLIENT_STATE_CLOSING) {
                http_close_client(srv, i);
                continue;
            }

            /* Close idle connections */
            if (client->fd > 0 && client->state == CLIENT_STATE_IDLE) {
                uint64_t idle_time = (now - client->connect_time) / 1000000;
                if (idle_time >= HTTP_IDLE_TIMEOUT_SEC) {
                    log_info("HTTP[%d]: Closing idle connection (slot %d, %llu sec)\n",
                             srv->port, i, (unsigned long long)idle_time);
                    http_close_client(srv, i);
                    continue;
                }
            }

            if (client->fd > 0 && client->state == CLIENT_STATE_STREAMING) {
                if (client->request == REQUEST_MJPEG_STREAM)
                    has_camera_clients = 1;
                else if (client->request == REQUEST_DISPLAY_STREAM)
                    has_display_clients = 1;

                /* Switch new streaming clients to blocking mode (once) */
                if (client->frames_sent == 0 && client->header_sent) {
                    make_streaming_socket(client->fd);
                }
            }
        }

        /* 2. Wait for new frame using condvar (efficient sleep, no polling) */
        if (has_camera_clients || has_display_clients) {
            /* Wait on camera buffer (most frequent source).
             * Short timeout to also check display frames and new connections. */
            HTTP_TIMING_START(fb_copy_time);
            frame_buffer_wait(&g_jpeg_buffer, last_camera_seq, 100);
            HTTP_TIMING_END(&g_mjpeg_timing, fb_copy_time);
        } else {
            /* No streaming clients — sleep longer */
            usleep(500000);
            continue;
        }

        /* 3. Send camera frames to all MJPEG clients (one frame, all clients) */
        uint64_t camera_seq = frame_buffer_get_sequence(&g_jpeg_buffer);
        if (has_camera_clients && camera_seq > last_camera_seq) {
            /* Copy frame to local buffer — safe from overwrites if writev blocks */
            uint64_t seq;
            size_t jpeg_size = frame_buffer_copy(&g_jpeg_buffer, camera_buf,
                                                  FRAME_BUFFER_MAX_JPEG, &seq, NULL, NULL);

            if (jpeg_size > 0) {
                /* Build MJPEG multipart header ONCE for all clients */
                int hlen = snprintf(header_buf, sizeof(header_buf),
                    "--%s\r\nContent-Type: image/jpeg\r\nContent-Length: %zu\r\n\r\n",
                    MJPEG_BOUNDARY, jpeg_size);

                /* Send to every camera streaming client */
                for (int i = 0; i < HTTP_MAX_CLIENTS; i++) {
                    HttpClient *client = &srv->clients[i];
                    if (client->fd <= 0 || client->state != CLIENT_STATE_STREAMING)
                        continue;
                    if (client->request != REQUEST_MJPEG_STREAM)
                        continue;
                    if (seq <= client->last_frame_seq)
                        continue;

                    /* Warmup pacing */
                    if (client->frames_sent < CLIENT_WARMUP_FRAMES)
                        usleep(CLIENT_WARMUP_DELAY_MS * 1000);

                    HTTP_TIMING_START(net_send_time);

                    /* Cork → writev → uncork: batch into full MSS segments */
                    int cork = 1;
                    setsockopt(client->fd, IPPROTO_TCP, TCP_CORK, &cork, sizeof(cork));

                    struct iovec iov[3] = {
                        { .iov_base = header_buf, .iov_len = hlen },
                        { .iov_base = (void *)camera_buf, .iov_len = jpeg_size },
                        { .iov_base = (void *)"\r\n", .iov_len = 2 }
                    };
                    if (streaming_sendv(client->fd, iov, 3) < 0) {
                        client->state = CLIENT_STATE_CLOSING;
                    } else {
                        client->last_frame_seq = seq;
                        client->frames_sent++;
                    }

                    cork = 0;
                    setsockopt(client->fd, IPPROTO_TCP, TCP_CORK, &cork, sizeof(cork));

                    HTTP_TIMING_END(&g_mjpeg_timing, net_send_time);
                }
                last_camera_seq = seq;
            }
        }

        /* 4. Send display frames to display streaming clients */
        uint64_t display_seq = frame_buffer_get_sequence(&g_display_buffer);
        if (has_display_clients && display_seq > last_display_seq) {
            uint64_t seq;
            size_t jpeg_size = frame_buffer_copy(&g_display_buffer, display_buf,
                                                  FRAME_BUFFER_MAX_DISPLAY, &seq, NULL, NULL);
            if (jpeg_size > 0) {
                int hlen = snprintf(header_buf, sizeof(header_buf),
                    "--%s\r\nContent-Type: image/jpeg\r\nContent-Length: %zu\r\n\r\n",
                    MJPEG_BOUNDARY, jpeg_size);

                for (int i = 0; i < HTTP_MAX_CLIENTS; i++) {
                    HttpClient *client = &srv->clients[i];
                    if (client->fd <= 0 || client->state != CLIENT_STATE_STREAMING)
                        continue;
                    if (client->request != REQUEST_DISPLAY_STREAM)
                        continue;
                    if (seq <= client->last_frame_seq)
                        continue;

                    if (client->frames_sent < CLIENT_WARMUP_FRAMES)
                        usleep(CLIENT_WARMUP_DELAY_MS * 1000);

                    int cork = 1;
                    setsockopt(client->fd, IPPROTO_TCP, TCP_CORK, &cork, sizeof(cork));

                    struct iovec iov[3] = {
                        { .iov_base = header_buf, .iov_len = hlen },
                        { .iov_base = (void *)display_buf, .iov_len = jpeg_size },
                        { .iov_base = (void *)"\r\n", .iov_len = 2 }
                    };
                    if (streaming_sendv(client->fd, iov, 3) < 0) {
                        client->state = CLIENT_STATE_CLOSING;
                    } else {
                        client->last_frame_seq = seq;
                        client->frames_sent++;
                    }

                    cork = 0;
                    setsockopt(client->fd, IPPROTO_TCP, TCP_CORK, &cork, sizeof(cork));
                }
                last_display_seq = seq;
            }
        }

        HTTP_TIMING_END(&g_mjpeg_timing, total_iter);
#ifdef ENCODER_TIMING
        g_mjpeg_timing.count++;
#endif
        HTTP_TIMING_LOG("MJPEG", &g_mjpeg_timing);
    }

    free(camera_buf);
    free(display_buf);
    return NULL;
}

int mjpeg_server_start(int port) {
    memset(&g_mjpeg_server, 0, sizeof(g_mjpeg_server));

    /* Use default port if not specified */
    if (port <= 0) {
        port = HTTP_MJPEG_PORT;
    }

    if (http_server_init(&g_mjpeg_server.server, port) != 0) {
        return -1;
    }

    g_mjpeg_server.running = 1;

    if (pthread_create(&g_mjpeg_server.thread, NULL, mjpeg_server_thread, &g_mjpeg_server) != 0) {
        fprintf(stderr, "HTTP: Failed to create MJPEG server thread\n");
        http_server_cleanup(&g_mjpeg_server.server);
        return -1;
    }
    pthread_setname_np(g_mjpeg_server.thread, "http_mjpeg");

    return 0;
}

void mjpeg_server_stop(void) {
    g_mjpeg_server.running = 0;
    g_mjpeg_server.server.running = 0;

    /* Wake up any waiting threads */
    frame_buffer_broadcast(&g_jpeg_buffer);

    pthread_join(g_mjpeg_server.thread, NULL);
    http_server_cleanup(&g_mjpeg_server.server);
}

int mjpeg_server_client_count(void) {
    return g_mjpeg_server.server.client_count;
}

/* ============================================================================
 * FLV Server Thread
 * ============================================================================ */

static void *flv_server_thread(void *arg) {
    FlvServerThread *st = (FlvServerThread *)arg;
    HttpServer *srv = &st->server;

    uint8_t *h264_buf = malloc(FRAME_BUFFER_MAX_H264);
    uint8_t *flv_buf = malloc(FLV_MAX_TAG_SIZE);

    /* Per-client muxers */
    FLVMuxer muxers[HTTP_MAX_CLIENTS];
    for (int i = 0; i < HTTP_MAX_CLIENTS; i++) {
        flv_muxer_init(&muxers[i], st->width, st->height, st->fps);
    }

    while (st->running && srv->running) {
#ifdef ENCODER_TIMING
        HTTP_TIMING_START(total_iter);
#endif
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(srv->listen_fd, &read_fds);

        int max_fd = srv->listen_fd;
        int has_streaming = 0;

        for (int i = 0; i < HTTP_MAX_CLIENTS; i++) {
            if (srv->clients[i].fd > 0 && srv->clients[i].state == CLIENT_STATE_IDLE) {
                FD_SET(srv->clients[i].fd, &read_fds);
                if (srv->clients[i].fd > max_fd) {
                    max_fd = srv->clients[i].fd;
                }
            }
            if (srv->clients[i].fd > 0 && srv->clients[i].state == CLIENT_STATE_STREAMING) {
                has_streaming = 1;
            }
        }

        /* Short timeout when streaming, longer when idle */
        HTTP_TIMING_START(select_time);
        struct timeval tv = has_streaming
            ? (struct timeval){ .tv_sec = 0, .tv_usec = 50000 }   /* 50ms */
            : (struct timeval){ .tv_sec = 0, .tv_usec = 500000 }; /* 500ms */
        int ret = select(max_fd + 1, &read_fds, NULL, NULL, &tv);
        HTTP_TIMING_END(&g_flv_timing, select_time);

        if (ret > 0) {
            if (FD_ISSET(srv->listen_fd, &read_fds)) {
                http_server_accept(srv);
            }

            for (int i = 0; i < HTTP_MAX_CLIENTS; i++) {
                if (srv->clients[i].fd > 0 && FD_ISSET(srv->clients[i].fd, &read_fds)) {
                    http_handle_client_read(srv, &srv->clients[i]);

                    /* New FLV client - send header and metadata */
                    if (srv->clients[i].state == CLIENT_STATE_STREAMING &&
                        srv->clients[i].request == REQUEST_FLV_STREAM &&
                        srv->clients[i].last_frame_seq == 0) {

                        /* Reset muxer for new connection */
                        flv_muxer_reset(&muxers[i]);

                        /* Send FLV header */
                        size_t hdr_size = flv_create_header(flv_buf, FLV_MAX_TAG_SIZE);
                        if (hdr_size > 0) {
                            http_send(srv->clients[i].fd, flv_buf, hdr_size);
                        }

                        /* Send metadata */
                        size_t meta_size = flv_create_metadata(&muxers[i], flv_buf, FLV_MAX_TAG_SIZE);
                        if (meta_size > 0) {
                            http_send(srv->clients[i].fd, flv_buf, meta_size);
                        }

                        /* Mark headers as sent by setting seq to current
                         * This also ensures we wait for fresh frames */
                        srv->clients[i].last_frame_seq = frame_buffer_get_sequence(&g_h264_buffer);
                    }
                }
            }
        }

        /* Stream H.264 to connected FLV clients */
        uint64_t current_seq = frame_buffer_get_sequence(&g_h264_buffer);
        uint64_t now = get_time_us();

        for (int i = 0; i < HTTP_MAX_CLIENTS; i++) {
            HttpClient *client = &srv->clients[i];

            if (client->fd > 0 && client->state == CLIENT_STATE_CLOSING) {
                http_close_client(srv, i);
                flv_muxer_reset(&muxers[i]);
                continue;
            }

            /* Close idle connections that haven't sent a request */
            if (client->fd > 0 && client->state == CLIENT_STATE_IDLE) {
                uint64_t idle_time = (now - client->connect_time) / 1000000;  /* seconds */
                if (idle_time >= HTTP_IDLE_TIMEOUT_SEC) {
                    log_info("HTTP[%d]: Closing idle connection (slot %d, %llu sec)\n",
                             srv->port, i, (unsigned long long)idle_time);
                    http_close_client(srv, i);
                    flv_muxer_reset(&muxers[i]);
                    continue;
                }
            }

            if (client->fd > 0 && client->state == CLIENT_STATE_STREAMING &&
                client->request == REQUEST_FLV_STREAM) {

                if (current_seq > client->last_frame_seq) {
                    /* Warmup pacing: add delays during initial connection
                     * to spread CPU load (can't skip H.264 frames due to dependencies) */
                    if (client->frames_sent < CLIENT_WARMUP_FRAMES) {
                        usleep(CLIENT_WARMUP_DELAY_MS * 1000);
                    }

                    uint64_t seq;
                    int is_keyframe;
                    HTTP_TIMING_START(fb_copy_time);
                    size_t h264_size = frame_buffer_copy(&g_h264_buffer, h264_buf,
                                                         FRAME_BUFFER_MAX_H264, &seq, NULL, &is_keyframe);
                    HTTP_TIMING_END(&g_flv_timing, fb_copy_time);

                    if (h264_size > 0) {
                        /* Mux to FLV */
                        size_t flv_size = flv_mux_h264(&muxers[i], h264_buf, h264_size,
                                                       flv_buf, FLV_MAX_TAG_SIZE);

                        if (flv_size > 0) {
                            HTTP_TIMING_START(net_send_time);
                            if (http_send(client->fd, flv_buf, flv_size) < 0) {
                                client->state = CLIENT_STATE_CLOSING;
                            } else {
                                client->last_frame_seq = seq;
                                client->frames_sent++;
                            }
                            HTTP_TIMING_END(&g_flv_timing, net_send_time);
                        }
                    }
                }
            }
        }

#ifdef ENCODER_TIMING
        HTTP_TIMING_END(&g_flv_timing, total_iter);
        g_flv_timing.count++;
        HTTP_TIMING_LOG("FLV", &g_flv_timing);
#endif
    }

    /* Cleanup */
    for (int i = 0; i < HTTP_MAX_CLIENTS; i++) {
        flv_muxer_cleanup(&muxers[i]);
    }
    free(h264_buf);
    free(flv_buf);

    return NULL;
}

int flv_server_start(int width, int height, int fps) {
    memset(&g_flv_server, 0, sizeof(g_flv_server));
    g_flv_server.width = width;
    g_flv_server.height = height;
    g_flv_server.fps = fps;

    if (http_server_init(&g_flv_server.server, HTTP_FLV_PORT) != 0) {
        return -1;
    }

    g_flv_server.running = 1;

    if (pthread_create(&g_flv_server.thread, NULL, flv_server_thread, &g_flv_server) != 0) {
        fprintf(stderr, "HTTP: Failed to create FLV server thread\n");
        http_server_cleanup(&g_flv_server.server);
        return -1;
    }
    pthread_setname_np(g_flv_server.thread, "http_flv");

    return 0;
}

void flv_server_stop(void) {
    g_flv_server.running = 0;
    g_flv_server.server.running = 0;

    frame_buffer_broadcast(&g_h264_buffer);

    pthread_join(g_flv_server.thread, NULL);
    http_server_cleanup(&g_flv_server.server);
}

int flv_server_client_count(void) {
    return g_flv_server.server.client_count;
}

/* ============================================================================
 * FLV Proxy - relay ACProxyCam's FLV stream to local clients
 * ============================================================================ */

void flv_proxy_set_url(const char *url) {
    pthread_mutex_lock(&g_flv_proxy_lock);
    if (url && url[0]) {
        strncpy(g_flv_proxy_url, url, sizeof(g_flv_proxy_url) - 1);
    } else {
        g_flv_proxy_url[0] = '\0';
    }
    pthread_mutex_unlock(&g_flv_proxy_lock);
}

int flv_proxy_is_active(void) {
    pthread_mutex_lock(&g_flv_proxy_lock);
    int active = g_flv_proxy_url[0] != '\0';
    pthread_mutex_unlock(&g_flv_proxy_lock);
    return active;
}

float flv_proxy_get_fps(void) {
    return g_flv_proxy_fps;
}

/* Parse "http://host:port/path" into components */
static int parse_http_url(const char *url, char *host, int host_size,
                           int *port, char *path, int path_size) {
    if (strncmp(url, "http://", 7) != 0) return -1;
    const char *hp = url + 7;
    const char *colon = strchr(hp, ':');
    const char *slash = strchr(hp, '/');

    if (colon && (!slash || colon < slash)) {
        int hlen = colon - hp;
        if (hlen >= host_size) hlen = host_size - 1;
        memcpy(host, hp, hlen);
        host[hlen] = '\0';
        *port = atoi(colon + 1);
    } else {
        *port = 80;
        int hlen = slash ? (slash - hp) : (int)strlen(hp);
        if (hlen >= host_size) hlen = host_size - 1;
        memcpy(host, hp, hlen);
        host[hlen] = '\0';
    }

    if (slash) {
        strncpy(path, slash, path_size - 1);
        path[path_size - 1] = '\0';
    } else {
        strncpy(path, "/", path_size - 1);
    }

    return 0;
}

/* Proxy thread: connect to ACProxyCam, relay FLV bytes to client */
static void *flv_proxy_thread(void *arg) {
    FlvProxyArg *pa = (FlvProxyArg *)arg;
    int client_fd = pa->client_fd;
    char url[256];
    strncpy(url, pa->url, sizeof(url) - 1);
    url[sizeof(url) - 1] = '\0';
    free(pa);

    char host[64], path[128];
    int port;
    if (parse_http_url(url, host, sizeof(host), &port, path, sizeof(path)) < 0) {
        fprintf(stderr, "FLV proxy: invalid URL: %s\n", url);
        close(client_fd);
        return NULL;
    }

    /* Connect to upstream (ACProxyCam) */
    int upstream = socket(AF_INET, SOCK_STREAM, 0);
    if (upstream < 0) {
        fprintf(stderr, "FLV proxy: socket() failed: %s\n", strerror(errno));
        close(client_fd);
        return NULL;
    }

    struct timeval tv = { .tv_sec = 10, .tv_usec = 0 };
    setsockopt(upstream, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    setsockopt(upstream, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, host, &addr.sin_addr);

    if (connect(upstream, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "FLV proxy: connect to %s:%d failed: %s\n",
                host, port, strerror(errno));
        close(upstream);
        close(client_fd);
        return NULL;
    }

    /* Send HTTP GET to upstream */
    char req[256];
    int rlen = snprintf(req, sizeof(req),
        "GET %s HTTP/1.1\r\nHost: %s:%d\r\nConnection: close\r\n\r\n",
        path, host, port);
    send(upstream, req, rlen, MSG_NOSIGNAL);

    /* Read upstream response header (skip it) */
    char hdr_buf[1024];
    int hdr_pos = 0;
    int body_start = -1;
    while (hdr_pos < (int)sizeof(hdr_buf) - 1) {
        ssize_t n = recv(upstream, hdr_buf + hdr_pos, 1, 0);
        if (n <= 0) break;
        hdr_pos++;
        if (hdr_pos >= 4 &&
            hdr_buf[hdr_pos-4] == '\r' && hdr_buf[hdr_pos-3] == '\n' &&
            hdr_buf[hdr_pos-2] == '\r' && hdr_buf[hdr_pos-1] == '\n') {
            body_start = hdr_pos;
            break;
        }
    }

    if (body_start < 0) {
        fprintf(stderr, "FLV proxy: no response from upstream\n");
        close(upstream);
        close(client_fd);
        return NULL;
    }

    /* Send our own HTTP headers to client (matching gkcam format) */
    const char *resp =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/plain\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Content-Length: 99999999999\r\n"
        "\r\n";
    if (send(client_fd, resp, strlen(resp), MSG_NOSIGNAL) < 0) {
        close(upstream);
        close(client_fd);
        return NULL;
    }

    fprintf(stderr, "FLV proxy: relaying from %s\n", url);

    /* FLV tag counter for FPS tracking */
    FlvTagCounter flv_counter;
    flv_counter_init(&flv_counter);

    /* Transparent byte relay with FLV tag counting */
    char buf[8192];
    while (g_flv_server.running) {
        ssize_t n = recv(upstream, buf, sizeof(buf), 0);
        if (n <= 0) break;

        /* Count FLV video tags for FPS tracking */
        flv_count_tags(&flv_counter, (const uint8_t *)buf, (int)n);

        ssize_t sent = 0;
        while (sent < n) {
            ssize_t w = send(client_fd, buf + sent, n - sent, MSG_NOSIGNAL);
            if (w < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    usleep(1000);
                    continue;
                }
                goto proxy_done;
            }
            sent += w;
        }
    }

proxy_done:
    fprintf(stderr, "FLV proxy: client disconnected\n");
    g_flv_proxy_fps = 0.0f;
    close(upstream);
    close(client_fd);
    return NULL;
}
