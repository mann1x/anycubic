/*
 * HTTP Server Implementation
 *
 * Minimal socket-based HTTP server for MJPEG and FLV streaming.
 */

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
#include <time.h>

/* Global server instances */
MjpegServerThread g_mjpeg_server;
FlvServerThread g_flv_server;

/* Control port for homepage links (default: 8081) */
static int g_control_port = HTTP_CONTROL_PORT;

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

/* Send single JPEG snapshot from a buffer */
static void http_send_snapshot_from_buffer(int fd, FrameBuffer *buffer, size_t max_size) {
    uint8_t *jpeg_buf = malloc(max_size);
    if (!jpeg_buf) {
        http_send_404(fd);
        return;
    }

    uint64_t seq;
    size_t jpeg_size = frame_buffer_copy(buffer, jpeg_buf, max_size, &seq, NULL);

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
                                          FRAME_BUFFER_MAX_JPEG, NULL, &cur_ts);

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
                                          FRAME_BUFFER_MAX_JPEG, NULL, NULL);
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
                                         FRAME_BUFFER_MAX_DISPLAY, NULL, NULL);
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
                display_client_connect();
                log_info("HTTP[%d]: Display stream started\n", srv->port);
                break;

            case REQUEST_DISPLAY_SNAPSHOT:
                http_send_display_snapshot(client->fd);
                client->state = CLIENT_STATE_CLOSING;
                break;

            case REQUEST_FLV_STREAM:
                http_send_flv_headers(client->fd);
                client->state = CLIENT_STATE_STREAMING;
                client->header_sent = 1;

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

static void *mjpeg_server_thread(void *arg) {
    MjpegServerThread *st = (MjpegServerThread *)arg;
    HttpServer *srv = &st->server;

    uint8_t *jpeg_buf = malloc(FRAME_BUFFER_MAX_JPEG);
    uint8_t *display_buf = malloc(FRAME_BUFFER_MAX_DISPLAY);
    char header_buf[256];

    while (st->running && srv->running) {
#ifdef ENCODER_TIMING
        HTTP_TIMING_START(total_iter);
#endif
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(srv->listen_fd, &read_fds);

        int max_fd = srv->listen_fd;

        for (int i = 0; i < HTTP_MAX_CLIENTS; i++) {
            if (srv->clients[i].fd > 0 && srv->clients[i].state == CLIENT_STATE_IDLE) {
                FD_SET(srv->clients[i].fd, &read_fds);
                if (srv->clients[i].fd > max_fd) {
                    max_fd = srv->clients[i].fd;
                }
            }
        }

        HTTP_TIMING_START(select_time);
        struct timeval tv = { .tv_sec = 0, .tv_usec = 50000 };  /* 50ms timeout */
        int ret = select(max_fd + 1, &read_fds, NULL, NULL, &tv);
        HTTP_TIMING_END(&g_mjpeg_timing, select_time);

        if (ret > 0) {
            /* Check for new connections */
            if (FD_ISSET(srv->listen_fd, &read_fds)) {
                http_server_accept(srv);
            }

            /* Check for client requests */
            for (int i = 0; i < HTTP_MAX_CLIENTS; i++) {
                if (srv->clients[i].fd > 0 && FD_ISSET(srv->clients[i].fd, &read_fds)) {
                    http_handle_client_read(srv, &srv->clients[i]);
                }
            }
        }

        /* Stream frames to connected clients */
        uint64_t camera_seq = frame_buffer_get_sequence(&g_jpeg_buffer);
        uint64_t display_seq = frame_buffer_get_sequence(&g_display_buffer);

        for (int i = 0; i < HTTP_MAX_CLIENTS; i++) {
            HttpClient *client = &srv->clients[i];

            if (client->fd > 0 && client->state == CLIENT_STATE_CLOSING) {
                http_close_client(srv, i);
                continue;
            }

            if (client->fd > 0 && client->state == CLIENT_STATE_STREAMING) {
                FrameBuffer *buffer = NULL;
                uint8_t *buf = NULL;
                size_t buf_size = 0;
                uint64_t current_seq = 0;

                /* Select source buffer based on request type */
                if (client->request == REQUEST_MJPEG_STREAM) {
                    buffer = &g_jpeg_buffer;
                    buf = jpeg_buf;
                    buf_size = FRAME_BUFFER_MAX_JPEG;
                    current_seq = camera_seq;
                } else if (client->request == REQUEST_DISPLAY_STREAM) {
                    buffer = &g_display_buffer;
                    buf = display_buf;
                    buf_size = FRAME_BUFFER_MAX_DISPLAY;
                    current_seq = display_seq;
                }

                if (buffer && buf && current_seq > client->last_frame_seq) {
                    /* Warmup pacing: add delays during initial connection
                     * to spread CPU load and prevent spikes that could affect printing */
                    if (client->frames_sent < CLIENT_WARMUP_FRAMES) {
                        usleep(CLIENT_WARMUP_DELAY_MS * 1000);
                    }

                    uint64_t seq;
                    HTTP_TIMING_START(fb_copy_time);
                    size_t jpeg_size = frame_buffer_copy(buffer, buf, buf_size, &seq, NULL);
                    HTTP_TIMING_END(&g_mjpeg_timing, fb_copy_time);

                    if (jpeg_size > 0) {
                        /* Send multipart frame */
                        int hlen = snprintf(header_buf, sizeof(header_buf),
                            "--%s\r\nContent-Type: image/jpeg\r\nContent-Length: %zu\r\n\r\n",
                            MJPEG_BOUNDARY, jpeg_size);

                        HTTP_TIMING_START(net_send_time);
                        if (http_send(client->fd, header_buf, hlen) < 0 ||
                            http_send(client->fd, buf, jpeg_size) < 0 ||
                            http_send(client->fd, "\r\n", 2) < 0) {
                            client->state = CLIENT_STATE_CLOSING;
                        } else {
                            client->last_frame_seq = seq;
                            client->frames_sent++;
                        }
                        HTTP_TIMING_END(&g_mjpeg_timing, net_send_time);
                    }
                }
            }
        }

#ifdef ENCODER_TIMING
        HTTP_TIMING_END(&g_mjpeg_timing, total_iter);
        g_mjpeg_timing.count++;
        HTTP_TIMING_LOG("MJPEG", &g_mjpeg_timing);
#endif
    }

    free(display_buf);
    free(jpeg_buf);
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

        for (int i = 0; i < HTTP_MAX_CLIENTS; i++) {
            if (srv->clients[i].fd > 0 && srv->clients[i].state == CLIENT_STATE_IDLE) {
                FD_SET(srv->clients[i].fd, &read_fds);
                if (srv->clients[i].fd > max_fd) {
                    max_fd = srv->clients[i].fd;
                }
            }
        }

        HTTP_TIMING_START(select_time);
        struct timeval tv = { .tv_sec = 0, .tv_usec = 50000 };
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
                    }
                }
            }
        }

        /* Stream H.264 to connected FLV clients */
        uint64_t current_seq = frame_buffer_get_sequence(&g_h264_buffer);

        for (int i = 0; i < HTTP_MAX_CLIENTS; i++) {
            HttpClient *client = &srv->clients[i];

            if (client->fd > 0 && client->state == CLIENT_STATE_CLOSING) {
                http_close_client(srv, i);
                flv_muxer_reset(&muxers[i]);
                continue;
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
                                                         FRAME_BUFFER_MAX_H264, &seq, &is_keyframe);
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
