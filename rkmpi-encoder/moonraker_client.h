/*
 * Moonraker WebSocket Client
 *
 * Connects to Moonraker via WebSocket, subscribes to print status events,
 * and drives timelapse recording via direct function calls.
 *
 * Used in primary mode when timelapse is enabled. Replaces the Python
 * h264_server.py Moonraker integration with zero-dependency C.
 */

#ifndef MOONRAKER_CLIENT_H
#define MOONRAKER_CLIENT_H

#include "config.h"
#include <pthread.h>

typedef struct MoonrakerClient {
    /* Connection */
    char host[64];
    int port;
    int fd;                         /* TCP socket */
    volatile int connected;
    volatile int running;
    pthread_t thread;
    int request_id;                 /* JSON-RPC incrementing ID */

    /* Print state */
    char print_state[16];           /* standby/printing/paused/complete/cancelled/error */
    int current_layer;
    int total_layers;
    char filename[256];

    /* Timelapse state */
    volatile int timelapse_active;
    int timelapse_first_layer_captured;
    int timelapse_frames;
    pthread_t hyperlapse_thread;    /* For hyperlapse interval timer */
    volatile int hyperlapse_running;

    /* Toolhead position */
    float position[4];              /* X, Y, Z, E from toolhead.position */
    volatile int has_position;

    /* Config reference (read-only, owned by main) */
    AppConfig *config;
} MoonrakerClient;

/*
 * Start the Moonraker WebSocket client.
 * Spawns a background thread that connects, subscribes, and processes events.
 *
 * @param mc    Client state (caller-owned, must remain valid until stop)
 * @param host  Moonraker host (e.g. "127.0.0.1")
 * @param port  Moonraker port (e.g. 7125)
 * @param cfg   Application config (read-only reference)
 * @return 0 on success, -1 on error
 */
int moonraker_client_start(MoonrakerClient *mc, const char *host, int port,
                            AppConfig *cfg);

/*
 * Stop the Moonraker WebSocket client.
 * Closes connection and joins the background thread.
 */
void moonraker_client_stop(MoonrakerClient *mc);

/*
 * Check if the client is currently connected to Moonraker.
 *
 * @return 1 if connected, 0 if not
 */
int moonraker_client_is_connected(const MoonrakerClient *mc);

/*
 * Send G-code script to printer via Moonraker (fire-and-forget).
 * Uses the existing WebSocket connection to send printer.gcode.script.
 *
 * @param mc     Client state
 * @param gcode  G-code string (e.g. "G28" or "G1 X110 Y110 F3000")
 * @return 0 on send success, -1 on failure (not connected, send error)
 */
int moonraker_client_send_gcode(MoonrakerClient *mc, const char *gcode);

/*
 * Get current print state string.
 * Thread-safe: copies print_state into caller buffer.
 *
 * @param mc     Client state
 * @param buf    Output buffer
 * @param buflen Buffer size
 * @return 0 on success
 */
int moonraker_client_get_print_state(const MoonrakerClient *mc, char *buf, int buflen);

#endif /* MOONRAKER_CLIENT_H */
