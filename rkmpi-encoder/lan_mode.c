/*
 * LAN Mode Management
 *
 * One-shot RPC calls to gkapi for LAN print mode control.
 * Protocol: JSON-RPC over TCP with ETX (0x03) message delimiter.
 */

#include "lan_mode.h"
#include "cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/select.h>

#define RPC_HOST    "127.0.0.1"
#define RPC_PORT    18086
#define RPC_ETX     0x03
#define RPC_TIMEOUT 5   /* seconds */

/* Send RPC request and receive response (one-shot).
 * Returns malloc'd response string or NULL on error. Caller must free. */
static char *rpc_oneshot(const char *request) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return NULL;

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(RPC_PORT),
    };
    inet_pton(AF_INET, RPC_HOST, &addr.sin_addr);

    /* Non-blocking connect with timeout */
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    int ret = connect(fd, (struct sockaddr *)&addr, sizeof(addr));
    if (ret < 0 && errno != EINPROGRESS) {
        close(fd);
        return NULL;
    }

    if (ret < 0) {
        /* Wait for connect */
        fd_set wfds;
        FD_ZERO(&wfds);
        FD_SET(fd, &wfds);
        struct timeval tv = { .tv_sec = RPC_TIMEOUT };
        if (select(fd + 1, NULL, &wfds, NULL, &tv) <= 0) {
            close(fd);
            return NULL;
        }
        int err = 0;
        socklen_t len = sizeof(err);
        getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len);
        if (err) {
            close(fd);
            return NULL;
        }
    }

    /* Back to blocking for send/recv */
    fcntl(fd, F_SETFL, flags);

    /* Set recv timeout */
    struct timeval tv = { .tv_sec = RPC_TIMEOUT };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    /* Send request with ETX */
    size_t req_len = strlen(request);
    char *msg = malloc(req_len + 1);
    if (!msg) {
        close(fd);
        return NULL;
    }
    memcpy(msg, request, req_len);
    msg[req_len] = RPC_ETX;

    ssize_t sent = send(fd, msg, req_len + 1, MSG_NOSIGNAL);
    free(msg);
    if (sent != (ssize_t)(req_len + 1)) {
        close(fd);
        return NULL;
    }

    /* Receive response (read until ETX or timeout) */
    char buf[4096];
    size_t total = 0;

    while (total < sizeof(buf) - 1) {
        ssize_t n = recv(fd, buf + total, sizeof(buf) - 1 - total, 0);
        if (n <= 0) break;
        total += n;

        /* Check for ETX delimiter */
        for (size_t i = total - n; i < total; i++) {
            if (buf[i] == RPC_ETX) {
                buf[i] = '\0';
                close(fd);
                return strdup(buf);
            }
        }
    }

    close(fd);
    buf[total] = '\0';
    return total > 0 ? strdup(buf) : NULL;
}

int lan_mode_query(void) {
    const char *request = "{\"id\":2016,\"method\":\"Printer/QueryLanPrintStatus\",\"params\":null}";
    char *response = rpc_oneshot(request);
    if (!response) {
        fprintf(stderr, "LAN mode: RPC query failed\n");
        return -1;
    }

    /* Parse response - look for "lan_print_mode" or "lanPrintMode" in result */
    cJSON *root = cJSON_Parse(response);
    free(response);
    if (!root) return -1;

    int result = -1;
    const cJSON *res = cJSON_GetObjectItemCaseSensitive(root, "result");
    if (res) {
        /* Try different response formats */
        const cJSON *mode = cJSON_GetObjectItemCaseSensitive(res, "lan_print_mode");
        if (!mode) mode = cJSON_GetObjectItemCaseSensitive(res, "lanPrintMode");
        if (!mode) mode = cJSON_GetObjectItemCaseSensitive(res, "mode");

        if (mode) {
            if (cJSON_IsNumber(mode)) {
                result = mode->valueint ? 1 : 0;
            } else if (cJSON_IsString(mode) && mode->valuestring) {
                result = (strcmp(mode->valuestring, "1") == 0 ||
                         strcmp(mode->valuestring, "true") == 0) ? 1 : 0;
            }
        }
    }

    cJSON_Delete(root);
    return result;
}

int lan_mode_enable(void) {
    /* First check if already enabled */
    int status = lan_mode_query();
    if (status == 1) {
        fprintf(stderr, "LAN mode: Already enabled\n");
        return 0;
    }

    const char *request = "{\"id\":2016,\"method\":\"Printer/OpenLanPrint\",\"params\":null}";
    char *response = rpc_oneshot(request);
    if (!response) {
        fprintf(stderr, "LAN mode: RPC enable failed\n");
        return -1;
    }

    /* Check for success */
    cJSON *root = cJSON_Parse(response);
    free(response);
    if (!root) return -1;

    const cJSON *err = cJSON_GetObjectItemCaseSensitive(root, "error");
    int success = (err == NULL || cJSON_IsNull(err)) ? 0 : -1;

    cJSON_Delete(root);

    if (success == 0) {
        fprintf(stderr, "LAN mode: Enabled successfully\n");
    } else {
        fprintf(stderr, "LAN mode: Enable request returned error\n");
    }

    return success;
}
