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
        /* Try different response formats (gkapi returns {"open":0|1}) */
        const cJSON *mode = cJSON_GetObjectItemCaseSensitive(res, "open");
        if (!mode) mode = cJSON_GetObjectItemCaseSensitive(res, "lan_print_mode");
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

/* ============================================================================
 * WiFi Route Priority Fix
 *
 * When eth1 and wlan0 are on the same subnet, DHCP may add wlan0 routes
 * with the same metric as eth1. This re-adds wlan0 routes with metric 100
 * so eth1 is preferred, with wlan0 as automatic fallback.
 * ============================================================================ */

/* Extract IP prefix (first 3 octets) for an interface from ifconfig output.
 * Returns 1 and fills prefix (e.g. "192.168.1") on success, 0 if not found. */
static int get_iface_prefix(const char *ifconfig_output, const char *iface,
                            char *prefix, size_t prefix_size) {
    /* Find the interface block */
    const char *p = ifconfig_output;
    while (p) {
        /* Look for interface name at start of line */
        if (strncmp(p, iface, strlen(iface)) == 0) {
            char next = p[strlen(iface)];
            if (next == ' ' || next == '\t' || next == ':') {
                /* Found interface, look for inet addr in this block */
                const char *block_end = p;
                while (*block_end) {
                    if (*block_end == '\n' && block_end[1] != ' ' && block_end[1] != '\t')
                        break;
                    block_end++;
                }

                /* Search for IP address */
                const char *addr = strstr(p, "inet addr:");
                if (!addr || addr > block_end) {
                    addr = strstr(p, "inet ");
                    if (addr && addr < block_end) addr += 5;
                } else {
                    addr += 10;
                }

                if (addr && addr < block_end) {
                    /* Parse IP and extract first 3 octets */
                    int a, b, c, d;
                    if (sscanf(addr, "%d.%d.%d.%d", &a, &b, &c, &d) == 4) {
                        snprintf(prefix, prefix_size, "%d.%d.%d", a, b, c);
                        return 1;
                    }
                }
                return 0;
            }
        }
        p = strchr(p, '\n');
        if (p) p++;
    }
    return 0;
}

int wifi_fix_route_priority(void) {
    /* Get ifconfig output */
    FILE *f = popen("ifconfig 2>/dev/null", "r");
    if (!f) return 0;

    char output[4096];
    size_t total = 0;
    while (total < sizeof(output) - 1) {
        size_t n = fread(output + total, 1, sizeof(output) - 1 - total, f);
        if (n == 0) break;
        total += n;
    }
    output[total] = '\0';
    pclose(f);

    /* Get IP prefixes for eth1 and wlan0 */
    char eth1_prefix[32], wlan_prefix[32];
    if (!get_iface_prefix(output, "eth1", eth1_prefix, sizeof(eth1_prefix)) ||
        !get_iface_prefix(output, "wlan0", wlan_prefix, sizeof(wlan_prefix))) {
        return 1;  /* One or both interfaces not present, nothing to fix */
    }

    if (strcmp(eth1_prefix, wlan_prefix) != 0) {
        return 1;  /* Different subnets, nothing to fix */
    }

    /* Check if wlan0 has a metric-0 route for this subnet */
    f = popen("route -n 2>/dev/null", "r");
    if (!f) return 0;

    char route_output[4096];
    total = 0;
    while (total < sizeof(route_output) - 1) {
        size_t n = fread(route_output + total, 1, sizeof(route_output) - 1 - total, f);
        if (n == 0) break;
        total += n;
    }
    route_output[total] = '\0';
    pclose(f);

    /* Parse routes - look for wlan0 with metric 0 */
    int needs_fix = 0;
    char wlan_gw[48] = {0};
    char subnet[48];
    snprintf(subnet, sizeof(subnet), "%s.0", wlan_prefix);

    char *line = route_output;
    while (line && *line) {
        char *next = strchr(line, '\n');
        if (next) *next = '\0';

        char dest[32], gw[32], mask[32], flags[16], iface[16];
        int metric;
        /* Destination Gateway Genmask Flags Metric Ref Use Iface */
        if (sscanf(line, "%31s %31s %31s %15s %d %*d %*d %15s",
                   dest, gw, mask, flags, &metric, iface) >= 6) {
            if (strcmp(iface, "wlan0") == 0) {
                if (strcmp(dest, subnet) == 0 && metric == 0) {
                    needs_fix = 1;
                }
                if (strcmp(dest, "0.0.0.0") == 0 && metric == 0) {
                    snprintf(wlan_gw, sizeof(wlan_gw), "%s", gw);
                }
            }
        }

        if (next) { *next = '\n'; line = next + 1; }
        else break;
    }

    if (!needs_fix) {
        return 1;  /* Already fixed or no conflict */
    }

    fprintf(stderr, "WiFi: Fixing route priority - eth1 and wlan0 both on %s.0/24, preferring eth1\n",
            eth1_prefix);

    /* Remove and re-add subnet route with metric 100 */
    char cmd[256];
    snprintf(cmd, sizeof(cmd),
             "route del -net %s.0 netmask 255.255.255.0 dev wlan0 2>/dev/null; "
             "route add -net %s.0 netmask 255.255.255.0 dev wlan0 metric 100 2>/dev/null",
             wlan_prefix, wlan_prefix);
    system(cmd);

    /* Fix default route too if wlan0 has one with metric 0 */
    if (wlan_gw[0]) {
        snprintf(cmd, sizeof(cmd),
                 "route del default gw %s dev wlan0 2>/dev/null; "
                 "route add default gw %s dev wlan0 metric 100 2>/dev/null",
                 wlan_gw, wlan_gw);
        system(cmd);
    }

    return 1;
}

/* ============================================================================
 * RTL8723DS WiFi Driver Optimization
 *
 * Enables A-MSDU aggregation and disables power management to reduce
 * kernel thread CPU usage by ~10-15% when streaming over WiFi.
 * ============================================================================ */

/* Read a sysfs parameter file, return trimmed content or NULL */
static char *read_sysfs(const char *path, char *buf, size_t bufsize) {
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    if (!fgets(buf, bufsize, f)) {
        fclose(f);
        return NULL;
    }
    fclose(f);
    /* Trim trailing whitespace */
    size_t len = strlen(buf);
    while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r' || buf[len - 1] == ' '))
        buf[--len] = '\0';
    return buf;
}

/* Write a value to a sysfs parameter file */
static int write_sysfs(const char *path, const char *value) {
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    fputs(value, f);
    fclose(f);
    return 0;
}

int wifi_optimize_driver(void) {
    /* Check if wlan0 exists */
    FILE *f = popen("ifconfig wlan0 2>/dev/null", "r");
    if (!f) return 1;
    char buf[256];
    int has_wlan = (fgets(buf, sizeof(buf), f) != NULL);
    pclose(f);
    if (!has_wlan) return 1;  /* No wlan0, nothing to optimize */

    /* Check if RTL8723DS module is loaded */
    static const char *amsdu_path = "/sys/module/RTL8723DS/parameters/rtw_amsdu_mode";
    static const char *ampdu_amsdu_path = "/sys/module/RTL8723DS/parameters/rtw_tx_ampdu_amsdu";

    char val[32];
    if (!read_sysfs(amsdu_path, val, sizeof(val))) {
        return 1;  /* Not RTL8723DS, skip */
    }

    int changed = 0;

    /* Enable A-MSDU aggregation */
    if (strcmp(val, "1") != 0) {
        if (write_sysfs(amsdu_path, "1") == 0) changed = 1;
    }

    /* Enable A-MPDU + A-MSDU */
    if (read_sysfs(ampdu_amsdu_path, val, sizeof(val)) && strcmp(val, "1") != 0) {
        if (write_sysfs(ampdu_amsdu_path, "1") == 0) changed = 1;
    }

    /* Disable power save */
    f = popen("iw dev wlan0 get power_save 2>/dev/null", "r");
    if (f) {
        char ps_buf[128] = {0};
        size_t total = 0;
        while (total < sizeof(ps_buf) - 1) {
            size_t n = fread(ps_buf + total, 1, sizeof(ps_buf) - 1 - total, f);
            if (n == 0) break;
            total += n;
        }
        ps_buf[total] = '\0';
        pclose(f);

        /* Check if power save is on (case-insensitive search for "on") */
        for (char *p = ps_buf; *p; p++) {
            if ((*p == 'o' || *p == 'O') && (p[1] == 'n' || p[1] == 'N')) {
                system("iw dev wlan0 set power_save off 2>/dev/null");
                changed = 1;
                break;
            }
        }
    }

    if (changed) {
        fprintf(stderr, "WiFi: Optimized RTL8723DS - A-MSDU enabled, power save off\n");
    }

    return 1;
}

/* ============================================================================
 * LAN Mode
 * ============================================================================ */

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
