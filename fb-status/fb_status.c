/*
 * fb_status - Lightweight framebuffer status display for Anycubic printers
 *
 * Generic tool for displaying status messages on the printer's LCD.
 * Auto-detects printer model and applies correct screen orientation.
 *
 * Usage:
 *   fb_status show "message" [options]  - Display status message
 *   fb_status save                      - Save current screen
 *   fb_status hide [options]            - Restore saved screen
 *   fb_status busy                      - Set printer busy (via Native API)
 *   fb_status free                      - Set printer free (via Native API)
 *   fb_status pipe [options]            - Piped input mode for long-running scripts
 *
 * Options:
 *   -c, --color COLOR     Text color (name or hex, e.g., green, FF00FF)
 *   -g, --bg COLOR        Background color (name or hex, default: 222222)
 *   -s, --size SIZE       Font size in pixels (default: 32)
 *   -p, --position POS    Position: top, center, bottom (default: bottom)
 *   -t, --timeout SECS    Auto-hide after N seconds
 *   -f, --free            Also set printer free when hiding
 *   -b, --busy            Also set printer busy when showing
 *   -B, --bold            Use bold font variant
 *   -I, --italic          Use italic font variant
 *   -F, --font PATH       Custom font file path
 *   -m, --message MSG     Initial message (for pipe mode)
 *   -q, --quiet           No response output (fire-and-forget mode)
 *
 * Pipe mode commands (via stdin):
 *   busy                  Set printer busy
 *   free                  Set printer free
 *   show <message>        Display message (supports \n for multiline)
 *   color <color>         Set text color for next show
 *   bg <color>            Set background color
 *   size <n>              Set font size
 *   position <pos>        Set position (top/center/bottom)
 *   bold                  Enable bold font
 *   italic                Enable italic font
 *   regular               Reset to regular font
 *   font <path>           Load different font file
 *   hide                  Temporarily restore saved screen
 *   close                 Restore screen and exit
 *   close <secs>          Wait N seconds, then close
 *   close <secs> <msg>    Show message, wait, close
 *   close -f [secs] [msg] Close and set free
 *   rpc <timeout> <json> [options]  Execute RPC with progress display
 *
 * RPC command options:
 *   --match "pattern"     Pattern to match: {F}=float {D}=int {S}=string {*}=skip
 *   --extract <mode>      Extract mode: count, unique, last, sum
 *   --total <n>           Total count for percentage calculation
 *   --template "text"     Display template with {count} {total} {percent} {bar} {$1}...
 *   --interval <ms>       Minimum update interval (default: 1000ms)
 *   --on-error "text"     Template to show on error
 *
 * RPC example (bed mesh probing):
 *   rpc 3600 {"id":5,"method":"Leviq2/Probe","params":{}} \
 *       --match "probe at {F},{F}" --extract unique --total 64 \
 *       --template "Probing\n{count}/{total}\n{percent}%"
 *
 * Colors: green, red, yellow, blue, white, orange, cyan, magenta, gray, black
 *         or hex RGB: FF0000 (red), 00FF00 (green), 0000FF (blue)
 *
 * Compile:
 *   arm-rockchip830-linux-uclibcgnueabihf-gcc -O2 -o fb_status fb_status.c -lm
 */

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/file.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <math.h>
#include <ctype.h>
#include <time.h>

// Display wake paths
#define BRIGHTNESS_PATH "/sys/class/backlight/backlight/brightness"
#define TOUCH_DEVICE "/dev/input/event0"
#define WAKE_TOUCH_DURATION_MS 50

// Screen orientation modes (based on printer model)
typedef enum {
    ORIENT_NORMAL = 0,
    ORIENT_FLIP_180 = 1,    // KS1, KS1M
    ORIENT_ROTATE_90 = 2,   // K3, K2P, K3V2
    ORIENT_ROTATE_270 = 3   // K3M
} ScreenOrientation;

typedef enum {
    POS_TOP = 0,
    POS_CENTER = 1,
    POS_BOTTOM = 2
} BoxPosition;

typedef enum {
    STYLE_REGULAR = 0,
    STYLE_BOLD = 1,
    STYLE_ITALIC = 2,
    STYLE_BOLD_ITALIC = 3
} FontStyle;

// Model IDs
#define MODEL_ID_K2P   "20021"
#define MODEL_ID_K3    "20024"
#define MODEL_ID_KS1   "20025"
#define MODEL_ID_K3M   "20026"
#define MODEL_ID_K3V2  "20027"
#define MODEL_ID_KS1M  "20029"

#define API_CFG_PATH "/userdata/app/gk/config/api.cfg"
#define NATIVE_API_PORT 18086

// Default font paths (Rinkhals fonts)
#define FONT_DIR "/opt/rinkhals/ui/assets"
#define FONT_REGULAR "AlibabaSans-Regular.ttf"
#define FONT_BOLD "AlibabaSans-Bold.ttf"
#define FONT_FALLBACK "/oem/usr/share/simsun_en.ttf"

#define BACKUP_PATH "/tmp/fb_status_screen.bmp"
#define LOCK_PATH "/tmp/fb_status_screen.bmp.lock"
#define PID_PATH "/tmp/fb_status.pid"
#define FFMPEG_PATH "/ac_lib/lib/third_bin/ffmpeg"
#define FFMPEG_LIB_PATH "LD_LIBRARY_PATH=/ac_lib/lib/third_lib"

#define DEFAULT_FONT_SIZE 32.0f
#define DEFAULT_BG_COLOR 0xFF222222

#define MAX_CMD_LEN 4096

// Predefined colors (BGRX format)
typedef struct {
    const char *name;
    uint32_t color;
} ColorDef;

static const ColorDef color_table[] = {
    {"green",    0xFF00FF00},
    {"red",      0xFF0000FF},
    {"yellow",   0xFF00FFFF},
    {"blue",     0xFFFF0000},
    {"white",    0xFFFFFFFF},
    {"black",    0xFF000000},
    {"orange",   0xFF00A5FF},
    {"cyan",     0xFFFFFF00},
    {"magenta",  0xFFFF00FF},
    {"gray",     0xFF808080},
    {"grey",     0xFF808080},
    {"pink",     0xFFCBC0FF},
    {"purple",   0xFF800080},
    {"lime",     0xFF00FF00},
    {"aqua",     0xFFFFFF00},
    {"navy",     0xFF800000},
    {"teal",     0xFF808000},
    {"maroon",   0xFF000080},
    {"olive",    0xFF008080},
    {"silver",   0xFFC0C0C0},
    {NULL, 0}
};

typedef struct {
    int fd;
    uint32_t *pixels;
    int width;
    int height;
    size_t size;
} Framebuffer;

// Pipe mode state
typedef struct {
    uint32_t text_color;
    uint32_t bg_color;
    float font_size;
    BoxPosition position;
    FontStyle style;
    char custom_font[512];
    int quiet;
    int lock_fd;
    Framebuffer fb;
    // Cached font data to avoid reloading on every display update
    unsigned char *cached_font_data;
    size_t cached_font_size;
    stbtt_fontinfo cached_font_info;
    int cached_font_valid;
    FontStyle cached_style;
    char cached_font_path[512];
    // Double buffering: saved screen copy for flicker-free updates
    uint32_t *saved_screen;
    size_t saved_screen_size;
    // Persistent work buffer (allocated once, reused for each update)
    uint32_t *work_buffer;
    // Previous box bounds for partial update optimization
    int prev_box_x, prev_box_y, prev_box_w, prev_box_h;
    uint32_t prev_bg_color;
    // Update throttling
    int64_t last_update_ms;
    int min_update_interval_ms;  // Default 500ms
} PipeState;

// Box bounds for partial update optimization
typedef struct {
    int x, y, w, h;
} BoxBounds;

// Forward declarations
static int64_t get_time_ms(void);

// RPC extract modes
typedef enum {
    EXTRACT_COUNT = 0,      // Count total matches
    EXTRACT_UNIQUE = 1,     // Count unique captured values
    EXTRACT_LAST = 2,       // Keep last captured value
    EXTRACT_SUM = 3         // Sum numeric captures
} ExtractMode;

#define RPC_MAX_CAPTURES 4
#define RPC_MAX_UNIQUE 256
#define RPC_BUFFER_SIZE 8192

// RPC progress tracking state
typedef struct {
    // Configuration (from command options)
    char pattern[256];          // Glob pattern with {F}, {D}, {S}, {*}
    ExtractMode extract_mode;
    int total;                  // For percentage calculation
    char template[512];         // Display template
    int interval_ms;            // Rate limit (default 1000ms)
    char error_template[256];
    int request_id;             // Request ID from JSON (for response matching)

    // Runtime state
    int count;                  // Match count
    double captures[RPC_MAX_CAPTURES];  // Last captured values
    int capture_count;          // Number of captures in pattern

    // Unique tracking (for EXTRACT_UNIQUE mode)
    char unique_keys[RPC_MAX_UNIQUE][64];
    int unique_count;

    // Timing
    int64_t start_time_ms;
    int64_t last_update_ms;

    // ETA tracking (time per item for progress estimation)
    int64_t last_item_time_ms;      // When the last item was completed
    int64_t total_item_time_ms;     // Total time spent on completed items
    int items_for_eta;              // Number of items used in ETA calculation

    // Socket buffer
    char buffer[RPC_BUFFER_SIZE];
    int buffer_len;
} RpcState;

// Global orientation
static ScreenOrientation g_orientation = ORIENT_NORMAL;

// Signal handling for cleanup
static volatile int g_running = 1;

static void signal_handler(int sig) {
    (void)sig;
    g_running = 0;
}

static int fb_open(Framebuffer *fb) {
    fb->fd = open("/dev/fb0", O_RDWR);
    if (fb->fd < 0) {
        perror("open /dev/fb0");
        return -1;
    }

    struct fb_var_screeninfo vinfo;
    if (ioctl(fb->fd, FBIOGET_VSCREENINFO, &vinfo) < 0) {
        perror("ioctl FBIOGET_VSCREENINFO");
        close(fb->fd);
        return -1;
    }

    fb->width = vinfo.xres;
    fb->height = vinfo.yres;
    fb->size = fb->width * fb->height * sizeof(uint32_t);

    fb->pixels = mmap(NULL, fb->size, PROT_READ | PROT_WRITE, MAP_SHARED, fb->fd, 0);
    if (fb->pixels == MAP_FAILED) {
        perror("mmap");
        close(fb->fd);
        return -1;
    }

    return 0;
}

static void fb_close(Framebuffer *fb) {
    if (fb->pixels && fb->pixels != MAP_FAILED) {
        munmap(fb->pixels, fb->size);
        fb->pixels = NULL;
    }
    if (fb->fd >= 0) {
        close(fb->fd);
        fb->fd = -1;
    }
}

static ScreenOrientation detect_orientation(void) {
    FILE *f = fopen(API_CFG_PATH, "r");
    if (!f) {
        fprintf(stderr, "Cannot open %s, using default orientation\n", API_CFG_PATH);
        return ORIENT_ROTATE_90;
    }

    char buf[4096];
    size_t len = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[len] = '\0';

    char *p = strstr(buf, "\"modelId\"");
    if (!p) return ORIENT_ROTATE_90;

    p = strchr(p, ':');
    if (!p) return ORIENT_ROTATE_90;
    p++;
    while (*p == ' ' || *p == '"') p++;

    char model_id[16] = {0};
    int i = 0;
    while (*p && *p != '"' && *p != ',' && i < 15) {
        model_id[i++] = *p++;
    }

    fprintf(stderr, "Model: %s -> ", model_id);

    if (strcmp(model_id, MODEL_ID_KS1) == 0 || strcmp(model_id, MODEL_ID_KS1M) == 0) {
        fprintf(stderr, "FLIP_180\n");
        return ORIENT_FLIP_180;
    } else if (strcmp(model_id, MODEL_ID_K3M) == 0) {
        fprintf(stderr, "ROTATE_270\n");
        return ORIENT_ROTATE_270;
    } else {
        fprintf(stderr, "ROTATE_90\n");
        return ORIENT_ROTATE_90;
    }
}

static int send_native_api(const char *json_cmd) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return -1;

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(NATIVE_API_PORT),
        .sin_addr.s_addr = inet_addr("127.0.0.1")
    };

    struct timeval tv = {.tv_sec = 5, .tv_usec = 0};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(sock);
        return -1;
    }

    char cmd_buf[512];
    snprintf(cmd_buf, sizeof(cmd_buf), "%s\x03", json_cmd);
    send(sock, cmd_buf, strlen(cmd_buf), 0);

    char resp[1024];
    recv(sock, resp, sizeof(resp), 0);
    close(sock);
    return 0;
}

static int set_printer_busy(int busy) {
    char cmd[256];
    snprintf(cmd, sizeof(cmd),
        "{\"id\":1,\"method\":\"Printer/ReportUIWorkStatus\",\"params\":{\"busy\":%d}}",
        busy ? 1 : 0);
    int ret = send_native_api(cmd);
    if (ret == 0) fprintf(stderr, "Printer %s\n", busy ? "BUSY" : "FREE");
    return ret;
}

// PID file management
static int read_pid_file(void) {
    FILE *f = fopen(PID_PATH, "r");
    if (!f) return -1;

    int pid = -1;
    if (fscanf(f, "%d", &pid) != 1) {
        pid = -1;
    }
    fclose(f);
    return pid;
}

static int write_pid_file(void) {
    FILE *f = fopen(PID_PATH, "w");
    if (!f) {
        perror("Cannot create PID file");
        return -1;
    }
    fprintf(f, "%d\n", getpid());
    fclose(f);
    return 0;
}

static void remove_pid_file(void) {
    unlink(PID_PATH);
}

static int is_process_running(int pid) {
    if (pid <= 0) return 0;
    return kill(pid, 0) == 0;
}

static int check_existing_instance(void) {
    int existing_pid = read_pid_file();
    if (existing_pid > 0 && is_process_running(existing_pid)) {
        fprintf(stderr, "Another fb_status instance is running (PID %d)\n", existing_pid);
        return -1;
    }
    // Stale PID file, remove it
    if (existing_pid > 0) {
        remove_pid_file();
    }
    return 0;
}

// File locking for backup image
static int acquire_lock(void) {
    int fd = open(LOCK_PATH, O_CREAT | O_RDWR, 0644);
    if (fd < 0) {
        perror("Cannot create lock file");
        return -1;
    }

    if (flock(fd, LOCK_EX | LOCK_NB) < 0) {
        if (errno == EWOULDBLOCK) {
            fprintf(stderr, "Backup image is locked by another process\n");
        } else {
            perror("Cannot acquire lock");
        }
        close(fd);
        return -1;
    }

    return fd;
}

static void release_lock(int fd) {
    if (fd >= 0) {
        flock(fd, LOCK_UN);
        close(fd);
    }
}

// Emit a single input event
static void emit_input_event(int fd, int type, int code, int val) {
    struct input_event ie;
    memset(&ie, 0, sizeof(ie));
    gettimeofday(&ie.time, NULL);
    ie.type = type;
    ie.code = code;
    ie.value = val;
    write(fd, &ie, sizeof(ie));
}

// Inject touch at specified coordinates
static int inject_touch(int x, int y, int duration_ms) {
    int fd = open(TOUCH_DEVICE, O_WRONLY);
    if (fd < 0) {
        return -1;
    }

    // Touch down - MT Protocol B
    emit_input_event(fd, EV_ABS, ABS_MT_SLOT, 0);
    emit_input_event(fd, EV_ABS, ABS_MT_TRACKING_ID, 1);
    emit_input_event(fd, EV_ABS, ABS_MT_POSITION_X, x);
    emit_input_event(fd, EV_ABS, ABS_MT_POSITION_Y, y);
    emit_input_event(fd, EV_ABS, ABS_MT_TOUCH_MAJOR, 50);
    emit_input_event(fd, EV_ABS, ABS_MT_PRESSURE, 100);
    emit_input_event(fd, EV_KEY, BTN_TOUCH, 1);
    emit_input_event(fd, EV_SYN, SYN_REPORT, 0);

    usleep(duration_ms * 1000);

    // Touch up
    emit_input_event(fd, EV_ABS, ABS_MT_TRACKING_ID, -1);
    emit_input_event(fd, EV_KEY, BTN_TOUCH, 0);
    emit_input_event(fd, EV_SYN, SYN_REPORT, 0);

    close(fd);
    return 0;
}

// Wake display by injecting touch at safe coordinates
// Touch wakes K3SysUi which restores brightness AND proper background
// (Standby mode sets brightness=0 AND changes background to gray)
// Safe coordinates are in the upper-right corner (status icon area) to avoid triggering UI actions
static void wake_display(void) {
    // Get framebuffer dimensions for safe touch coordinates
    int fb_width = 800, fb_height = 480;  // defaults
    FILE *vsize = fopen("/sys/class/graphics/fb0/virtual_size", "r");
    if (vsize) {
        fscanf(vsize, "%d,%d", &fb_width, &fb_height);
        fclose(vsize);
    }

    // Calculate safe wake coordinates based on orientation
    // Touch at upper-right corner (status icon area) - won't trigger UI buttons
    int safe_x, safe_y;
    switch (g_orientation) {
        case ORIENT_FLIP_180:  // KS1, KS1M - 180° flip
            safe_x = 2;
            safe_y = fb_height - 2;
            break;
        case ORIENT_ROTATE_270:  // K3M - 270° rotation
            safe_x = 2;
            safe_y = 2;
            break;
        case ORIENT_ROTATE_90:  // K3, K2P, K3V2 - 90° rotation
        case ORIENT_NORMAL:
        default:
            safe_x = fb_width - 2;
            safe_y = 2;
            break;
    }

    // Inject touch to wake display and reset K3SysUi sleep timer
    inject_touch(safe_x, safe_y, WAKE_TOUCH_DURATION_MS);

    // Small delay to let display wake up
    usleep(100000);  // 100ms
}

static int save_screen(void) {
    char cmd[512];
    snprintf(cmd, sizeof(cmd),
        "%s %s -f fbdev -i /dev/fb0 -frames:v 1 -y %s </dev/null >/dev/null 2>&1",
        FFMPEG_LIB_PATH, FFMPEG_PATH, BACKUP_PATH);
    return system(cmd);
}

static int restore_screen(void) {
    char cmd[512];
    snprintf(cmd, sizeof(cmd),
        "%s %s -i %s -f fbdev /dev/fb0 </dev/null >/dev/null 2>&1",
        FFMPEG_LIB_PATH, FFMPEG_PATH, BACKUP_PATH);
    return system(cmd);
}

// Cleanup backup file and lock
static void cleanup_backup(void) {
    unlink(BACKUP_PATH);
    unlink(LOCK_PATH);
}

// Save screen to memory buffer for double-buffering (no flickering)
static int save_screen_to_buffer(Framebuffer *fb, uint32_t **buffer, size_t *buffer_size) {
    *buffer_size = fb->size;
    *buffer = malloc(*buffer_size);
    if (!*buffer) {
        fprintf(stderr, "Failed to allocate screen buffer\n");
        return -1;
    }
    memcpy(*buffer, fb->pixels, *buffer_size);
    return 0;
}

// Restore screen from memory buffer (instant, no flickering)
static void restore_screen_from_buffer(Framebuffer *fb, uint32_t *buffer, size_t buffer_size) {
    if (buffer && buffer_size == fb->size) {
        memcpy(fb->pixels, buffer, buffer_size);
    }
}

// Parse hex color (RGB or RRGGBB)
static int parse_hex_color(const char *str, uint32_t *color) {
    // Skip optional # or 0x prefix
    if (str[0] == '#') str++;
    else if (str[0] == '0' && (str[1] == 'x' || str[1] == 'X')) str += 2;

    // Check if all characters are hex digits
    int len = strlen(str);
    for (int i = 0; i < len; i++) {
        if (!isxdigit(str[i])) return 0;
    }

    if (len != 6 && len != 3) return 0;

    unsigned int r, g, b;
    if (len == 6) {
        sscanf(str, "%02x%02x%02x", &r, &g, &b);
    } else {
        // Short form: RGB -> RRGGBB
        sscanf(str, "%1x%1x%1x", &r, &g, &b);
        r = r * 17; g = g * 17; b = b * 17;
    }

    // Convert to BGRX format
    *color = 0xFF000000 | (r << 16) | (g << 8) | b;
    return 1;
}

// Parse color name or hex value, returns success and sets color
static int parse_color_ex(const char *name, uint32_t *color) {
    if (!name || !*name) return 0;

    // Try hex first
    if (parse_hex_color(name, color)) {
        return 1;
    }

    // Try named colors
    for (const ColorDef *c = color_table; c->name; c++) {
        if (strcasecmp(name, c->name) == 0) {
            *color = c->color;
            return 1;
        }
    }

    return 0;
}

// Parse color name or hex value
static uint32_t parse_color(const char *name) {
    uint32_t color;
    if (parse_color_ex(name, &color)) {
        return color;
    }
    fprintf(stderr, "Unknown color '%s', using green\n", name);
    return color_table[0].color;
}

static unsigned char *load_font(const char *path, size_t *size) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;

    fseek(f, 0, SEEK_END);
    *size = ftell(f);
    fseek(f, 0, SEEK_SET);

    unsigned char *data = malloc(*size);
    if (!data) { fclose(f); return NULL; }

    fread(data, 1, *size, f);
    fclose(f);
    return data;
}

// Build font path based on style
static unsigned char *load_styled_font(const char *custom_path, FontStyle style, size_t *size) {
    unsigned char *data = NULL;

    // If custom path specified, use it directly
    if (custom_path && *custom_path) {
        data = load_font(custom_path, size);
        if (data) {
            fprintf(stderr, "Font: %s (%zu bytes)\n", custom_path, *size);
            return data;
        }
        fprintf(stderr, "Warning: Cannot load %s\n", custom_path);
    }

    // Try styled variants from default directory
    char path[512];
    const char *variants[] = {NULL, NULL, NULL, NULL};

    switch (style) {
        case STYLE_BOLD:
            variants[0] = "AlibabaSans-Bold.ttf";
            variants[1] = "AlibabaSans-Medium.ttf";
            break;
        case STYLE_ITALIC:
            variants[0] = "AlibabaSans-Italic.ttf";
            variants[1] = "AlibabaSans-RegularItalic.ttf";
            break;
        case STYLE_BOLD_ITALIC:
            variants[0] = "AlibabaSans-BoldItalic.ttf";
            variants[1] = "AlibabaSans-Bold.ttf";
            break;
        default:
            variants[0] = FONT_REGULAR;
            break;
    }

    // Try each variant
    for (int i = 0; variants[i]; i++) {
        snprintf(path, sizeof(path), "%s/%s", FONT_DIR, variants[i]);
        data = load_font(path, size);
        if (data) {
            fprintf(stderr, "Font: %s (%zu bytes)\n", path, *size);
            return data;
        }
    }

    // Fallback to regular
    snprintf(path, sizeof(path), "%s/%s", FONT_DIR, FONT_REGULAR);
    data = load_font(path, size);
    if (data) {
        fprintf(stderr, "Font: %s (fallback, %zu bytes)\n", path, *size);
        return data;
    }

    // Last resort fallback
    data = load_font(FONT_FALLBACK, size);
    if (data) {
        fprintf(stderr, "Font: %s (fallback, %zu bytes)\n", FONT_FALLBACK, *size);
    }
    return data;
}

static inline void transform_coords(Framebuffer *fb, int *x, int *y) {
    switch (g_orientation) {
        case ORIENT_FLIP_180:
            *x = fb->width - 1 - *x;
            *y = fb->height - 1 - *y;
            break;
        case ORIENT_ROTATE_90:
        case ORIENT_ROTATE_270:
            // TODO: implement rotation
            break;
        default:
            break;
    }
}

static void draw_rect(Framebuffer *fb, int x, int y, int w, int h, uint32_t color) {
    for (int py = y; py < y + h && py < fb->height; py++) {
        if (py < 0) continue;
        for (int px = x; px < x + w && px < fb->width; px++) {
            if (px < 0) continue;
            int tx = px, ty = py;
            transform_coords(fb, &tx, &ty);
            if (tx >= 0 && tx < fb->width && ty >= 0 && ty < fb->height) {
                fb->pixels[ty * fb->width + tx] = color;
            }
        }
    }
}

static void blend_pixel(Framebuffer *fb, int x, int y, uint32_t color, unsigned char alpha) {
    if (x < 0 || x >= fb->width || y < 0 || y >= fb->height) return;

    int tx = x, ty = y;
    transform_coords(fb, &tx, &ty);
    if (tx < 0 || tx >= fb->width || ty < 0 || ty >= fb->height) return;

    uint32_t *dst = &fb->pixels[ty * fb->width + tx];

    int db = (*dst) & 0xFF;
    int dg = (*dst >> 8) & 0xFF;
    int dr = (*dst >> 16) & 0xFF;

    int sb = (color) & 0xFF;
    int sg = (color >> 8) & 0xFF;
    int sr = (color >> 16) & 0xFF;

    int a = alpha, ia = 255 - a;
    *dst = 0xFF000000 | (((sr * a + dr * ia) / 255) << 16) |
                        (((sg * a + dg * ia) / 255) << 8) |
                         ((sb * a + db * ia) / 255);
}

// Decode UTF-8 character and return codepoint, advance pointer
// Returns 0xFFFD (replacement char) on invalid sequences
static int utf8_decode(const unsigned char **ptr, const unsigned char *end) {
    const unsigned char *p = *ptr;
    if (p >= end) return 0;

    unsigned char c = *p++;
    int codepoint;
    int extra_bytes;

    if ((c & 0x80) == 0) {
        // ASCII: 0xxxxxxx
        codepoint = c;
        extra_bytes = 0;
    } else if ((c & 0xE0) == 0xC0) {
        // 2-byte: 110xxxxx 10xxxxxx
        codepoint = c & 0x1F;
        extra_bytes = 1;
    } else if ((c & 0xF0) == 0xE0) {
        // 3-byte: 1110xxxx 10xxxxxx 10xxxxxx
        codepoint = c & 0x0F;
        extra_bytes = 2;
    } else if ((c & 0xF8) == 0xF0) {
        // 4-byte: 11110xxx 10xxxxxx 10xxxxxx 10xxxxxx
        codepoint = c & 0x07;
        extra_bytes = 3;
    } else {
        // Invalid start byte
        *ptr = p;
        return 0xFFFD;
    }

    for (int i = 0; i < extra_bytes; i++) {
        if (p >= end || (*p & 0xC0) != 0x80) {
            *ptr = p;
            return 0xFFFD;
        }
        codepoint = (codepoint << 6) | (*p++ & 0x3F);
    }

    *ptr = p;
    return codepoint;
}

// Measure width of a single line (UTF-8 aware)
static int measure_line_width(stbtt_fontinfo *font, const char *line, int len, float scale) {
    int width = 0;
    const unsigned char *p = (const unsigned char *)line;
    const unsigned char *end = p + len;
    int prev_cp = 0;

    while (p < end) {
        int cp = utf8_decode(&p, end);
        if (cp == 0) break;

        int advance, lsb;
        stbtt_GetCodepointHMetrics(font, cp, &advance, &lsb);
        width += (int)(advance * scale);

        // Kerning with previous character
        if (prev_cp) {
            width += (int)(scale * stbtt_GetCodepointKernAdvance(font, prev_cp, cp));
        }
        prev_cp = cp;
    }
    return width;
}

// Render a single line of text (UTF-8 aware)
static void render_line(Framebuffer *fb, stbtt_fontinfo *font, const char *line, int len,
                       int start_x, int y, float scale, uint32_t color) {
    float xpos = start_x;
    const unsigned char *p = (const unsigned char *)line;
    const unsigned char *end = p + len;
    int prev_cp = 0;

    while (p < end) {
        int cp = utf8_decode(&p, end);
        if (cp == 0) break;

        int x0, y0, x1, y1;
        stbtt_GetCodepointBitmapBox(font, cp, scale, scale, &x0, &y0, &x1, &y1);

        int glyph_w = x1 - x0;
        int glyph_h = y1 - y0;

        if (glyph_w > 0 && glyph_h > 0) {
            unsigned char *bitmap = malloc(glyph_w * glyph_h);
            if (bitmap) {
                stbtt_MakeCodepointBitmap(font, bitmap, glyph_w, glyph_h, glyph_w, scale, scale, cp);

                for (int by = 0; by < glyph_h; by++) {
                    for (int bx = 0; bx < glyph_w; bx++) {
                        unsigned char alpha = bitmap[by * glyph_w + bx];
                        if (alpha > 0) {
                            blend_pixel(fb, (int)xpos + x0 + bx, y + y0 + by, color, alpha);
                        }
                    }
                }
                free(bitmap);
            }
        }

        int advance, lsb;
        stbtt_GetCodepointHMetrics(font, cp, &advance, &lsb);
        xpos += advance * scale;

        // Kerning with previous character
        if (prev_cp) {
            xpos += scale * stbtt_GetCodepointKernAdvance(font, prev_cp, cp);
        }
        prev_cp = cp;
    }
}

// Process escape sequences in text (e.g., \n -> newline)
static void process_escapes(char *text) {
    char *src = text;
    char *dst = text;
    while (*src) {
        if (*src == '\\' && *(src + 1) == 'n') {
            *dst++ = '\n';
            src += 2;
        } else if (*src == '\\' && *(src + 1) == 't') {
            *dst++ = '\t';
            src += 2;
        } else if (*src == '\\' && *(src + 1) == '\\') {
            *dst++ = '\\';
            src += 2;
        } else {
            *dst++ = *src++;
        }
    }
    *dst = '\0';
}

static int show_status(Framebuffer *fb, const char *text, uint32_t text_color,
                       uint32_t bg_color, BoxPosition position, float font_size,
                       const char *custom_font, FontStyle style) {
    size_t fsize;
    unsigned char *font_data = load_styled_font(custom_font, style, &fsize);
    if (!font_data) {
        fprintf(stderr, "Failed to load any font\n");
        return -1;
    }

    stbtt_fontinfo font;
    if (!stbtt_InitFont(&font, font_data, stbtt_GetFontOffsetForIndex(font_data, 0))) {
        fprintf(stderr, "Failed to init font\n");
        free(font_data);
        return -1;
    }

    float scale = stbtt_ScaleForPixelHeight(&font, font_size);

    int ascent, descent, line_gap;
    stbtt_GetFontVMetrics(&font, &ascent, &descent, &line_gap);

    int baseline = (int)(ascent * scale);
    int line_height = (int)((ascent - descent + line_gap) * scale);
    int single_line_height = (int)((ascent - descent) * scale);

    // Parse lines (split by \n)
    #define MAX_LINES 20
    const char *lines[MAX_LINES];
    int line_lens[MAX_LINES];
    int line_widths[MAX_LINES];
    int num_lines = 0;

    const char *p = text;
    while (*p && num_lines < MAX_LINES) {
        lines[num_lines] = p;
        const char *end = p;
        while (*end && *end != '\n') end++;
        line_lens[num_lines] = end - p;
        line_widths[num_lines] = measure_line_width(&font, p, line_lens[num_lines], scale);
        num_lines++;
        if (*end == '\n') end++;
        p = end;
        if (!*p) break;
    }

    // Find max width
    int max_width = 0;
    for (int i = 0; i < num_lines; i++) {
        if (line_widths[i] > max_width) max_width = line_widths[i];
    }

    // Box dimensions
    int padding = 20;
    int box_width = max_width + padding * 2;
    int total_text_height = single_line_height + (num_lines - 1) * line_height;
    int box_height = total_text_height + padding * 2;

    // Position
    int box_x = (fb->width - box_width) / 2;
    int box_y;
    switch (position) {
        case POS_TOP: box_y = 30; break;
        case POS_CENTER: box_y = (fb->height - box_height) / 2; break;
        default: box_y = fb->height - box_height - 30; break;
    }

    // Draw background
    draw_rect(fb, box_x, box_y, box_width, box_height, bg_color);

    // Draw border
    draw_rect(fb, box_x, box_y, box_width, 2, text_color);
    draw_rect(fb, box_x, box_y + box_height - 2, box_width, 2, text_color);
    draw_rect(fb, box_x, box_y, 2, box_height, text_color);
    draw_rect(fb, box_x + box_width - 2, box_y, 2, box_height, text_color);

    // Render each line (centered horizontally)
    int y = box_y + padding + baseline;
    for (int i = 0; i < num_lines; i++) {
        int line_x = box_x + padding + (max_width - line_widths[i]) / 2;  // center each line
        render_line(fb, &font, lines[i], line_lens[i], line_x, y, scale, text_color);
        y += line_height;
    }

    free(font_data);
    return 0;
}

// Render status using pre-loaded font (for pipe mode with caching)
// If bounds is non-NULL, returns the box bounds for partial update optimization
static int show_status_with_font(Framebuffer *fb, const char *text, uint32_t text_color,
                                  uint32_t bg_color, BoxPosition position, float font_size,
                                  stbtt_fontinfo *font, BoxBounds *bounds) {
    float scale = stbtt_ScaleForPixelHeight(font, font_size);

    int ascent, descent, line_gap;
    stbtt_GetFontVMetrics(font, &ascent, &descent, &line_gap);

    int baseline = (int)(ascent * scale);
    int line_height = (int)((ascent - descent + line_gap) * scale);
    int single_line_height = (int)((ascent - descent) * scale);

    // Parse lines (split by \n)
    #define MAX_LINES_CACHED 20
    const char *lines[MAX_LINES_CACHED];
    int line_lens[MAX_LINES_CACHED];
    int line_widths[MAX_LINES_CACHED];
    int num_lines = 0;

    const char *p = text;
    while (*p && num_lines < MAX_LINES_CACHED) {
        lines[num_lines] = p;
        const char *end = p;
        while (*end && *end != '\n') end++;
        line_lens[num_lines] = end - p;
        line_widths[num_lines] = measure_line_width(font, p, line_lens[num_lines], scale);
        num_lines++;
        if (*end == '\n') end++;
        p = end;
        if (!*p) break;
    }

    // Find max width
    int max_width = 0;
    for (int i = 0; i < num_lines; i++) {
        if (line_widths[i] > max_width) max_width = line_widths[i];
    }

    // Box dimensions
    int padding = 20;
    int box_width = max_width + padding * 2;
    int total_text_height = single_line_height + (num_lines - 1) * line_height;
    int box_height = total_text_height + padding * 2;

    // Position
    int box_x = (fb->width - box_width) / 2;
    int box_y;
    switch (position) {
        case POS_TOP: box_y = 30; break;
        case POS_CENTER: box_y = (fb->height - box_height) / 2; break;
        default: box_y = fb->height - box_height - 30; break;
    }

    // Return bounds if requested
    if (bounds) {
        bounds->x = box_x;
        bounds->y = box_y;
        bounds->w = box_width;
        bounds->h = box_height;
    }

    // Draw background
    draw_rect(fb, box_x, box_y, box_width, box_height, bg_color);

    // Draw border
    draw_rect(fb, box_x, box_y, box_width, 2, text_color);
    draw_rect(fb, box_x, box_y + box_height - 2, box_width, 2, text_color);
    draw_rect(fb, box_x, box_y, 2, box_height, text_color);
    draw_rect(fb, box_x + box_width - 2, box_y, 2, box_height, text_color);

    // Render each line (centered horizontally)
    int y = box_y + padding + baseline;
    for (int i = 0; i < num_lines; i++) {
        int line_x = box_x + padding + (max_width - line_widths[i]) / 2;
        render_line(fb, font, lines[i], line_lens[i], line_x, y, scale, text_color);
        y += line_height;
    }

    return 0;
}

// Load/cache font for pipe mode state
static int ensure_font_cached(PipeState *state) {
    const char *font_path = state->custom_font[0] ? state->custom_font : NULL;

    // Check if we need to reload
    int need_reload = !state->cached_font_valid ||
                      state->cached_style != state->style ||
                      (font_path && strcmp(state->cached_font_path, font_path) != 0) ||
                      (!font_path && state->cached_font_path[0]);

    if (!need_reload) {
        return 0;  // Font is already cached and valid
    }

    // Free old cached font
    if (state->cached_font_data) {
        free(state->cached_font_data);
        state->cached_font_data = NULL;
    }
    state->cached_font_valid = 0;

    // Load new font
    state->cached_font_data = load_styled_font(font_path, state->style, &state->cached_font_size);
    if (!state->cached_font_data) {
        fprintf(stderr, "Failed to load font for cache\n");
        return -1;
    }

    if (!stbtt_InitFont(&state->cached_font_info, state->cached_font_data,
                        stbtt_GetFontOffsetForIndex(state->cached_font_data, 0))) {
        fprintf(stderr, "Failed to init cached font\n");
        free(state->cached_font_data);
        state->cached_font_data = NULL;
        return -1;
    }

    // Update cache state
    state->cached_font_valid = 1;
    state->cached_style = state->style;
    if (font_path) {
        strncpy(state->cached_font_path, font_path, sizeof(state->cached_font_path) - 1);
        state->cached_font_path[sizeof(state->cached_font_path) - 1] = '\0';
    } else {
        state->cached_font_path[0] = '\0';
    }

    return 0;
}

// Show status using cached font with optimized double-buffering
// Improvements over original:
// 1. Persistent work_buffer (allocated once, reused - no malloc/free per update)
// 2. Update throttling (reduces flicker during rapid updates)
// 3. VSync before copy (reduces tearing if supported)
// Note: Partial updates disabled - orientation transforms make it unreliable
static int pipe_show_status(PipeState *state, const char *text) {
    if (ensure_font_cached(state) < 0) {
        return -1;
    }

    // saved_screen must be set at pipe mode init
    if (!state->saved_screen) {
        fprintf(stderr, "No saved screen available\n");
        return -1;
    }

    // Update throttling - skip update if too soon since last one
    int64_t now = get_time_ms();
    if (state->min_update_interval_ms > 0 && state->last_update_ms > 0) {
        int64_t elapsed = now - state->last_update_ms;
        if (elapsed < state->min_update_interval_ms) {
            return 0;  // Skip this update
        }
    }
    state->last_update_ms = now;

    // Allocate persistent work buffer if not already done
    if (!state->work_buffer) {
        state->work_buffer = malloc(state->fb.size);
        if (!state->work_buffer) {
            fprintf(stderr, "Failed to allocate work buffer\n");
            return -1;
        }
    }

    // Step 1: Copy saved screen to work buffer (restores background)
    memcpy(state->work_buffer, state->saved_screen, state->saved_screen_size);

    // Step 2: Create temporary framebuffer pointing to work buffer
    Framebuffer work_fb = state->fb;
    work_fb.pixels = state->work_buffer;

    // Step 3: Draw text on work buffer
    show_status_with_font(&work_fb, text, state->text_color, state->bg_color,
                          state->position, state->font_size,
                          &state->cached_font_info, NULL);

    // Step 4: Try VSync before copy (reduces tearing if supported)
    #ifndef FBIO_WAITFORVSYNC
    #define FBIO_WAITFORVSYNC _IOW('F', 0x20, __u32)
    #endif
    uint32_t dummy = 0;
    ioctl(state->fb.fd, FBIO_WAITFORVSYNC, &dummy);  // Ignore errors

    // Step 5: Copy entire work buffer to framebuffer atomically
    memcpy(state->fb.pixels, state->work_buffer, state->fb.size);

    return 0;
}

// Response helper for pipe mode
static void pipe_respond(int quiet, const char *fmt, ...) {
    if (quiet) return;

    va_list args;
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
    fflush(stdout);
}

//=============================================================================
// RPC Command Support - Pattern matching and template rendering
//=============================================================================

// Get current time in milliseconds
static int64_t get_time_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

// Initialize RPC state with defaults
static void rpc_state_init(RpcState *rpc) {
    memset(rpc, 0, sizeof(RpcState));
    rpc->interval_ms = 1000;  // Default 1 second update interval
    rpc->template[0] = '\0';  // No display by default - caller must specify --template
}

// Check if character is a digit
static int is_digit(char c) {
    return c >= '0' && c <= '9';
}

// Parse a float from string, return chars consumed (0 if no match)
static int parse_float(const char *str, double *value) {
    const char *p = str;
    int has_digits = 0;

    // Optional negative sign
    if (*p == '-') p++;

    // Integer part
    while (is_digit(*p)) { p++; has_digits = 1; }

    // Optional decimal part
    if (*p == '.') {
        p++;
        while (is_digit(*p)) { p++; has_digits = 1; }
    }

    if (!has_digits) return 0;

    if (value) *value = atof(str);
    return p - str;
}

// Parse an integer from string, return chars consumed
static int parse_int(const char *str, double *value) {
    const char *p = str;
    int has_digits = 0;

    if (*p == '-') p++;
    while (is_digit(*p)) { p++; has_digits = 1; }

    if (!has_digits) return 0;

    if (value) *value = atoi(str);
    return p - str;
}

// Match pattern against input string
// Pattern syntax: {F}=float, {D}=int, {S}=string, {*}=skip
// Returns 1 if matched, 0 if not. Fills captures array.
static int pattern_match(const char *pattern, const char *input,
                         double *captures, int max_captures, int *capture_count) {
    const char *p = pattern;
    const char *i = input;
    int cap_idx = 0;

    while (*p && *i) {
        if (*p == '{') {
            // Check for placeholder
            if (strncmp(p, "{F}", 3) == 0) {
                // Float capture
                double val;
                int consumed = parse_float(i, &val);
                if (consumed == 0) return 0;
                if (cap_idx < max_captures && captures) {
                    captures[cap_idx++] = val;
                }
                i += consumed;
                p += 3;
            } else if (strncmp(p, "{D}", 3) == 0) {
                // Integer capture
                double val;
                int consumed = parse_int(i, &val);
                if (consumed == 0) return 0;
                if (cap_idx < max_captures && captures) {
                    captures[cap_idx++] = val;
                }
                i += consumed;
                p += 3;
            } else if (strncmp(p, "{S}", 3) == 0) {
                // String capture - find next literal char in pattern
                p += 3;
                char end_char = *p ? *p : '\0';
                const char *start = i;
                while (*i && *i != end_char) i++;
                // For {S} we don't capture the value (could extend later)
                (void)start;
                cap_idx++;  // Still count as a capture slot
            } else if (strncmp(p, "{*}", 3) == 0) {
                // Skip - find next literal char
                p += 3;
                char end_char = *p ? *p : '\0';
                if (end_char) {
                    while (*i && *i != end_char) i++;
                } else {
                    // {*} at end - match rest
                    while (*i) i++;
                }
            } else {
                // Not a valid placeholder, treat as literal
                if (*p != *i) return 0;
                p++; i++;
            }
        } else {
            // Literal match
            if (*p != *i) return 0;
            p++; i++;
        }
    }

    // Pattern must be fully consumed for a match
    // Input can have trailing data
    if (*p) return 0;

    if (capture_count) *capture_count = cap_idx;
    return 1;
}

// Add unique key, return 1 if new, 0 if already exists
static int rpc_add_unique(RpcState *rpc, const char *key) {
    // Check if exists
    for (int i = 0; i < rpc->unique_count; i++) {
        if (strcmp(rpc->unique_keys[i], key) == 0) {
            return 0;  // Already exists
        }
    }
    // Add new
    if (rpc->unique_count < RPC_MAX_UNIQUE) {
        strncpy(rpc->unique_keys[rpc->unique_count], key, 63);
        rpc->unique_keys[rpc->unique_count][63] = '\0';
        rpc->unique_count++;
        return 1;
    }
    return 0;
}

// Generate text-based progress bar using Unicode block characters
// Format: 0%████████░░░░░░░░50%
// Requires a font with Unicode support (e.g., DejaVu Sans Mono)
// Returns number of characters written
static int generate_progress_bar(char *buf, size_t buf_size, int percent, int width) {
    if (width <= 0) width = 16;  // Default width (shorter to fit with percentages)
    if (width > 24) width = 24;  // Max width
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;

    int filled = (percent * width + 50) / 100;  // Round to nearest
    int empty = width - filled;

    // Format: 0%████░░░░50%
    char *p = buf;
    char *end = buf + buf_size - 8;  // Leave room for percentage + null

    // Start with "0%"
    if (p + 2 < end) {
        *p++ = '0';
        *p++ = '%';
    }

    // Unicode block characters:
    // █ (U+2588) = full block (3 bytes in UTF-8: E2 96 88)
    // ░ (U+2591) = light shade (3 bytes in UTF-8: E2 96 91)
    for (int i = 0; i < filled && p + 3 < end; i++) {
        *p++ = (char)0xE2;
        *p++ = (char)0x96;
        *p++ = (char)0x88;
    }
    for (int i = 0; i < empty && p + 3 < end; i++) {
        *p++ = (char)0xE2;
        *p++ = (char)0x96;
        *p++ = (char)0x91;
    }

    // End with actual percentage
    p += snprintf(p, end - p, "%d%%", percent);
    *p = '\0';

    return p - buf;
}

// Render template with substitutions
// Supports: {count}, {total}, {percent}, {elapsed}, {bar}, {bar:N}, {$1}, {$2}, {$3}, {$4}
static void render_template(const char *tmpl, char *output, size_t output_size,
                            RpcState *rpc) {
    const char *p = tmpl;
    char *o = output;
    char *end = output + output_size - 1;

    while (*p && o < end) {
        if (*p == '{') {
            char var[32];
            const char *close = strchr(p, '}');
            if (close && (close - p - 1) < (int)sizeof(var)) {
                int var_len = close - p - 1;
                strncpy(var, p + 1, var_len);
                var[var_len] = '\0';

                char replacement[64] = "";
                int replaced = 1;

                if (strcmp(var, "count") == 0) {
                    int count = rpc->extract_mode == EXTRACT_UNIQUE ? rpc->unique_count : rpc->count;
                    // Cap at total for display (safety - avoid showing e.g., 10/9)
                    if (rpc->total > 0 && count > rpc->total) count = rpc->total;
                    snprintf(replacement, sizeof(replacement), "%d", count);
                } else if (strcmp(var, "total") == 0) {
                    snprintf(replacement, sizeof(replacement), "%d", rpc->total);
                } else if (strcmp(var, "percent") == 0) {
                    int count = rpc->extract_mode == EXTRACT_UNIQUE ? rpc->unique_count : rpc->count;
                    int pct = rpc->total > 0 ? (count * 100 / rpc->total) : 0;
                    snprintf(replacement, sizeof(replacement), "%d", pct);
                } else if (strcmp(var, "elapsed") == 0) {
                    int elapsed_sec = (get_time_ms() - rpc->start_time_ms) / 1000;
                    snprintf(replacement, sizeof(replacement), "%d", elapsed_sec);
                } else if (strncmp(var, "bar", 3) == 0) {
                    // {bar} or {bar:N} where N is width
                    int width = 20;  // Default
                    if (var[3] == ':') {
                        width = atoi(var + 4);
                    }
                    int count = rpc->extract_mode == EXTRACT_UNIQUE ? rpc->unique_count : rpc->count;
                    int pct = rpc->total > 0 ? (count * 100 / rpc->total) : 0;
                    generate_progress_bar(replacement, sizeof(replacement), pct, width);
                } else if (strcmp(var, "eta") == 0) {
                    // Calculate ETA based on average time per item
                    int count = rpc->extract_mode == EXTRACT_UNIQUE ? rpc->unique_count : rpc->count;
                    int remaining = rpc->total - count;
                    if (remaining > 0 && rpc->items_for_eta > 0) {
                        int64_t avg_time_ms = rpc->total_item_time_ms / rpc->items_for_eta;
                        int eta_secs = (int)((remaining * avg_time_ms) / 1000);
                        int eta_mins = eta_secs / 60;
                        eta_secs = eta_secs % 60;
                        if (eta_mins > 0) {
                            snprintf(replacement, sizeof(replacement), "%d:%02d", eta_mins, eta_secs);
                        } else {
                            snprintf(replacement, sizeof(replacement), "0:%02d", eta_secs);
                        }
                    } else if (count == 0 && rpc->total > 0) {
                        // No data yet - show "..."
                        snprintf(replacement, sizeof(replacement), "...");
                    } else {
                        // Complete or unknown
                        snprintf(replacement, sizeof(replacement), "0:00");
                    }
                } else if (var[0] == '$' && is_digit(var[1])) {
                    int idx = var[1] - '1';  // $1 = index 0
                    if (idx >= 0 && idx < RPC_MAX_CAPTURES) {
                        snprintf(replacement, sizeof(replacement), "%.2f", rpc->captures[idx]);
                        // Remove trailing zeros after decimal
                        char *dot = strchr(replacement, '.');
                        if (dot) {
                            char *e = replacement + strlen(replacement) - 1;
                            while (e > dot && *e == '0') *e-- = '\0';
                            if (e == dot) *e = '\0';
                        }
                    }
                } else {
                    replaced = 0;
                }

                if (replaced) {
                    int rep_len = strlen(replacement);
                    if (o + rep_len < end) {
                        strcpy(o, replacement);
                        o += rep_len;
                    }
                    p = close + 1;
                    continue;
                }
            }
        }
        *o++ = *p++;
    }
    *o = '\0';
}

// Process a chunk of RPC response data, looking for pattern matches
static void rpc_process_data(RpcState *rpc, const char *data, int len) {
    // Append to buffer
    int space = RPC_BUFFER_SIZE - rpc->buffer_len - 1;
    if (len > space) len = space;
    memcpy(rpc->buffer + rpc->buffer_len, data, len);
    rpc->buffer_len += len;
    rpc->buffer[rpc->buffer_len] = '\0';

    // Try to find pattern matches in buffer
    // We slide through looking for the pattern
    if (rpc->pattern[0] == '\0') return;

    char *p = rpc->buffer;
    while (*p) {
        double caps[RPC_MAX_CAPTURES] = {0};
        int cap_count = 0;

        if (pattern_match(rpc->pattern, p, caps, RPC_MAX_CAPTURES, &cap_count)) {
            // Found a match
            rpc->count++;

            // Copy captures
            rpc->capture_count = cap_count;
            for (int i = 0; i < cap_count && i < RPC_MAX_CAPTURES; i++) {
                if (rpc->extract_mode == EXTRACT_SUM) {
                    rpc->captures[i] += caps[i];
                } else {
                    rpc->captures[i] = caps[i];
                }
            }

            // For unique mode, build key from first two captures (X,Y)
            // Use %.1f precision to avoid false duplicates from floating point differences
            if (rpc->extract_mode == EXTRACT_UNIQUE && cap_count >= 2) {
                char key[64];
                snprintf(key, sizeof(key), "%.1f,%.1f", caps[0], caps[1]);
                if (rpc_add_unique(rpc, key)) {
                    // New unique item - update ETA tracking
                    int64_t now = get_time_ms();
                    if (rpc->last_item_time_ms > 0) {
                        int64_t item_time = now - rpc->last_item_time_ms;
                        rpc->total_item_time_ms += item_time;
                        rpc->items_for_eta++;
                    }
                    rpc->last_item_time_ms = now;
                }
            }

            // Move past the match start (at least 1 char to avoid infinite loop)
            p++;
        } else {
            p++;
        }
    }

    // Keep only last 256 bytes to avoid unbounded growth
    // (patterns should match within this window)
    if (rpc->buffer_len > 512) {
        int keep = 256;
        memmove(rpc->buffer, rpc->buffer + rpc->buffer_len - keep, keep);
        rpc->buffer_len = keep;
        rpc->buffer[keep] = '\0';
    }
}

// Parse RPC command options from argument string
// Format: rpc <timeout> <json> [--match pat] [--extract mode] [--total n] [--template t] [--interval ms]
static int parse_rpc_options(const char *args, int *timeout, char *json_cmd, size_t json_size,
                             RpcState *rpc) {
    rpc_state_init(rpc);

    const char *p = args;

    // Skip leading whitespace
    while (*p && (*p == ' ' || *p == '\t')) p++;

    // Parse timeout
    *timeout = atoi(p);
    while (*p && *p != ' ' && *p != '\t') p++;
    while (*p && (*p == ' ' || *p == '\t')) p++;

    // Parse JSON command (find the {...} part)
    if (*p != '{') {
        return -1;  // JSON must start with {
    }

    // Find matching closing brace (handle nesting)
    int depth = 0;
    const char *json_start = p;
    while (*p) {
        if (*p == '{') depth++;
        else if (*p == '}') {
            depth--;
            if (depth == 0) {
                p++;
                break;
            }
        }
        p++;
    }
    int json_len = p - json_start;
    if (json_len >= (int)json_size) json_len = json_size - 1;
    strncpy(json_cmd, json_start, json_len);
    json_cmd[json_len] = '\0';

    // Extract request ID from JSON for response matching
    // Look for "id":N pattern
    rpc->request_id = 0;
    const char *id_str = strstr(json_cmd, "\"id\":");
    if (id_str) {
        id_str += 5;  // Skip "id":
        while (*id_str == ' ') id_str++;
        rpc->request_id = atoi(id_str);
    }

    // Parse options
    while (*p) {
        while (*p && (*p == ' ' || *p == '\t')) p++;
        if (!*p) break;

        if (strncmp(p, "--match", 7) == 0) {
            p += 7;
            while (*p && (*p == ' ' || *p == '\t')) p++;
            // Pattern is quoted or until next --
            if (*p == '"') {
                p++;
                const char *end = strchr(p, '"');
                if (end) {
                    int len = end - p;
                    if (len >= (int)sizeof(rpc->pattern)) len = sizeof(rpc->pattern) - 1;
                    strncpy(rpc->pattern, p, len);
                    rpc->pattern[len] = '\0';
                    p = end + 1;
                }
            } else {
                const char *end = p;
                while (*end && *end != ' ' && strncmp(end, "--", 2) != 0) end++;
                int len = end - p;
                if (len >= (int)sizeof(rpc->pattern)) len = sizeof(rpc->pattern) - 1;
                strncpy(rpc->pattern, p, len);
                rpc->pattern[len] = '\0';
                p = end;
            }
        } else if (strncmp(p, "--extract", 9) == 0) {
            p += 9;
            while (*p && (*p == ' ' || *p == '\t')) p++;
            if (strncmp(p, "count", 5) == 0) { rpc->extract_mode = EXTRACT_COUNT; p += 5; }
            else if (strncmp(p, "unique", 6) == 0) { rpc->extract_mode = EXTRACT_UNIQUE; p += 6; }
            else if (strncmp(p, "last", 4) == 0) { rpc->extract_mode = EXTRACT_LAST; p += 4; }
            else if (strncmp(p, "sum", 3) == 0) { rpc->extract_mode = EXTRACT_SUM; p += 3; }
        } else if (strncmp(p, "--total", 7) == 0) {
            p += 7;
            while (*p && (*p == ' ' || *p == '\t')) p++;
            rpc->total = atoi(p);
            while (*p && *p != ' ' && *p != '\t') p++;
        } else if (strncmp(p, "--template", 10) == 0) {
            p += 10;
            while (*p && (*p == ' ' || *p == '\t')) p++;
            if (*p == '"') {
                p++;
                const char *end = strchr(p, '"');
                if (end) {
                    int len = end - p;
                    if (len >= (int)sizeof(rpc->template)) len = sizeof(rpc->template) - 1;
                    strncpy(rpc->template, p, len);
                    rpc->template[len] = '\0';
                    p = end + 1;
                }
            }
        } else if (strncmp(p, "--interval", 10) == 0) {
            p += 10;
            while (*p && (*p == ' ' || *p == '\t')) p++;
            rpc->interval_ms = atoi(p);
            while (*p && *p != ' ' && *p != '\t') p++;
        } else if (strncmp(p, "--on-error", 10) == 0) {
            p += 10;
            while (*p && (*p == ' ' || *p == '\t')) p++;
            if (*p == '"') {
                p++;
                const char *end = strchr(p, '"');
                if (end) {
                    int len = end - p;
                    if (len >= (int)sizeof(rpc->error_template)) len = sizeof(rpc->error_template) - 1;
                    strncpy(rpc->error_template, p, len);
                    rpc->error_template[len] = '\0';
                    p = end + 1;
                }
            }
        } else {
            // Unknown option, skip word
            while (*p && *p != ' ' && *p != '\t') p++;
        }
    }

    return 0;
}

// Execute RPC command with progress display
static int rpc_execute(PipeState *state, const char *args) {
    int timeout;
    char json_cmd[2048];
    RpcState rpc;

    if (parse_rpc_options(args, &timeout, json_cmd, sizeof(json_cmd), &rpc) < 0) {
        pipe_respond(state->quiet, "ERR: Invalid rpc command format\n");
        return -1;
    }

    // Connect to Native API
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        pipe_respond(state->quiet, "ERR: Socket creation failed\n");
        return -1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(NATIVE_API_PORT);
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");

    // Set socket timeout
    struct timeval tv;
    tv.tv_sec = timeout;
    tv.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(sock);
        pipe_respond(state->quiet, "ERR: Connect to API failed\n");
        return -1;
    }

    // Send command with ETX terminator
    char send_buf[2048];
    int send_len = snprintf(send_buf, sizeof(send_buf), "%s\003", json_cmd);
    if (write(sock, send_buf, send_len) != send_len) {
        close(sock);
        pipe_respond(state->quiet, "ERR: Send failed\n");
        return -1;
    }

    // Initialize timing
    rpc.start_time_ms = get_time_ms();
    rpc.last_update_ms = 0;

    // Show initial display if template provided
    if (rpc.template[0]) {
        char display[512];
        render_template(rpc.template, display, sizeof(display), &rpc);
        process_escapes(display);
        pipe_show_status(state, display);
    }

    // Debug: log to file
    FILE *dbg = fopen("/tmp/fb_rpc_debug.log", "w");
    if (dbg) fprintf(dbg, "RPC: Connected, sent %d bytes, timeout=%ds\n", send_len, timeout);
    if (dbg) fflush(dbg);

    // Read response with progress updates
    char read_buf[256];
    int result_found = 0;
    int error_found = 0;
    int read_count = 0;
    int total_bytes = 0;

    if (dbg) { fprintf(dbg, "RPC: Starting read loop\n"); fflush(dbg); }
    while (!result_found && !error_found) {
        int n = read(sock, read_buf, sizeof(read_buf) - 1);
        read_count++;
        if (dbg && read_count <= 50) {
            fprintf(dbg, "RPC: read #%d returned %d bytes (total=%d)\n", read_count, n, total_bytes + n);
            fprintf(dbg, "DATA: %.*s\n", n > 200 ? 200 : n, read_buf);
            fflush(dbg);
        }
        if (n <= 0) {
            if (dbg) fprintf(dbg, "RPC: read returned %d (errno=%d) after %d reads, %d bytes\n",
                    n, errno, read_count, total_bytes);
            if (dbg) fflush(dbg);
            // Connection closed = command completed successfully
            // API closes connection when done, doesn't always send explicit result
            // Only check for explicit error messages
            if (strstr(rpc.buffer, "\"error\"")) {
                error_found = 1;
            } else {
                // Connection closed without error = success
                result_found = 1;
            }
            break;
        }
        total_bytes += n;

        read_buf[n] = '\0';

        // Check for result or error in the JUST-READ data BEFORE processing
        // This avoids missing the result due to buffer truncation in rpc_process_data()
        char id_pattern[32];
        snprintf(id_pattern, sizeof(id_pattern), "\"id\":%d", rpc.request_id);

        char *id_match = strstr(read_buf, id_pattern);
        if (id_match) {
            // Found response for our request ID in this chunk
            if (strstr(id_match, "\"result\"")) {
                result_found = 1;
                if (dbg) fprintf(dbg, "RPC: Found result for id=%d after %d bytes\n", rpc.request_id, total_bytes);
                if (dbg) fflush(dbg);
            }
            if (strstr(id_match, "\"error\"")) {
                error_found = 1;
                if (dbg) fprintf(dbg, "RPC: Found error for id=%d after %d bytes: %.200s\n", rpc.request_id, total_bytes, read_buf);
                if (dbg) fflush(dbg);
            }
        }

        // Process data for pattern matching (may truncate buffer)
        rpc_process_data(&rpc, read_buf, n);

        // Note: Do NOT exit early when reaching total count - the RPC must wait for
        // the actual API response (connection close or JSON result). The count is
        // only for display progress, not for determining completion.

        // Update display if interval elapsed
        int64_t now = get_time_ms();
        if (rpc.template[0] && (now - rpc.last_update_ms >= rpc.interval_ms)) {
            rpc.last_update_ms = now;

            char display[512];
            render_template(rpc.template, display, sizeof(display), &rpc);
            process_escapes(display);
            pipe_show_status(state, display);
        }
    }

    close(sock);
    if (dbg) {
        fprintf(dbg, "RPC: Complete. result=%d error=%d reads=%d bytes=%d\n",
                result_found, error_found, read_count, total_bytes);
        fclose(dbg);
    }

    // Final display update
    if (rpc.template[0]) {
        char display[512];
        if (error_found && rpc.error_template[0]) {
            render_template(rpc.error_template, display, sizeof(display), &rpc);
        } else {
            render_template(rpc.template, display, sizeof(display), &rpc);
        }
        process_escapes(display);
        pipe_show_status(state, display);
    }

    if (error_found) {
        pipe_respond(state->quiet, "ERR: RPC command failed\n");
        return -1;
    }

    pipe_respond(state->quiet, "OK\n");
    return 0;
}

// Parse close command arguments: close [-f] [secs] [message]
static void parse_close_args(const char *args, int *set_free, int *timeout, char *message, size_t msg_size) {
    *set_free = 0;
    *timeout = 0;
    message[0] = '\0';

    if (!args || !*args) return;

    const char *p = args;

    // Skip leading whitespace
    while (*p == ' ') p++;

    // Check for -f flag
    if (strncmp(p, "-f", 2) == 0 && (p[2] == ' ' || p[2] == '\0')) {
        *set_free = 1;
        p += 2;
        while (*p == ' ') p++;
    }

    // Check for timeout (number)
    if (isdigit(*p)) {
        *timeout = atoi(p);
        while (isdigit(*p)) p++;
        while (*p == ' ') p++;
    }

    // Rest is message
    if (*p) {
        strncpy(message, p, msg_size - 1);
        message[msg_size - 1] = '\0';
        process_escapes(message);
    }
}

// Pipe mode command loop
static int run_pipe_mode(PipeState *state, const char *initial_message) {
    char line[MAX_CMD_LEN];
    char cmd[64];
    char arg[MAX_CMD_LEN];

    // Show initial message if provided
    if (initial_message && *initial_message) {
        char msg_copy[MAX_CMD_LEN];
        strncpy(msg_copy, initial_message, sizeof(msg_copy) - 1);
        msg_copy[sizeof(msg_copy) - 1] = '\0';
        process_escapes(msg_copy);

        if (pipe_show_status(state, msg_copy) == 0) {
            pipe_respond(state->quiet, "OK\n");
        } else {
            pipe_respond(state->quiet, "ERR: Failed to show initial message\n");
        }
    } else {
        pipe_respond(state->quiet, "OK\n");  // Ready
    }

    // Command loop
    while (g_running && fgets(line, sizeof(line), stdin)) {
        // Remove trailing newline
        size_t len = strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r')) {
            line[--len] = '\0';
        }

        if (len == 0) continue;  // Empty line

        // Parse command and argument
        cmd[0] = '\0';
        arg[0] = '\0';

        char *space = strchr(line, ' ');
        if (space) {
            size_t cmd_len = space - line;
            if (cmd_len >= sizeof(cmd)) cmd_len = sizeof(cmd) - 1;
            strncpy(cmd, line, cmd_len);
            cmd[cmd_len] = '\0';

            // Skip spaces after command
            const char *arg_start = space + 1;
            while (*arg_start == ' ') arg_start++;
            strncpy(arg, arg_start, sizeof(arg) - 1);
            arg[sizeof(arg) - 1] = '\0';
        } else {
            strncpy(cmd, line, sizeof(cmd) - 1);
            cmd[sizeof(cmd) - 1] = '\0';
        }

        // Process commands
        if (strcmp(cmd, "busy") == 0) {
            if (set_printer_busy(1) == 0) {
                pipe_respond(state->quiet, "OK\n");
            } else {
                pipe_respond(state->quiet, "ERR: Failed to set busy\n");
            }
        }
        else if (strcmp(cmd, "free") == 0) {
            if (set_printer_busy(0) == 0) {
                pipe_respond(state->quiet, "OK\n");
            } else {
                pipe_respond(state->quiet, "ERR: Failed to set free\n");
            }
        }
        else if (strcmp(cmd, "show") == 0) {
            if (arg[0] == '\0') {
                pipe_respond(state->quiet, "ERR: show requires a message\n");
            } else {
                process_escapes(arg);
                // Double-buffered update (no flickering)
                if (pipe_show_status(state, arg) == 0) {
                    pipe_respond(state->quiet, "OK\n");
                } else {
                    pipe_respond(state->quiet, "ERR: Failed to show message\n");
                }
            }
        }
        else if (strcmp(cmd, "color") == 0) {
            uint32_t color;
            if (parse_color_ex(arg, &color)) {
                state->text_color = color;
                pipe_respond(state->quiet, "OK\n");
            } else {
                pipe_respond(state->quiet, "ERR: Unknown color '%s'\n", arg);
            }
        }
        else if (strcmp(cmd, "bg") == 0) {
            uint32_t color;
            if (parse_color_ex(arg, &color)) {
                state->bg_color = color;
                pipe_respond(state->quiet, "OK\n");
            } else {
                pipe_respond(state->quiet, "ERR: Unknown color '%s'\n", arg);
            }
        }
        else if (strcmp(cmd, "size") == 0) {
            float size = atof(arg);
            if (size >= 8 && size <= 200) {
                state->font_size = size;
                pipe_respond(state->quiet, "OK\n");
            } else {
                pipe_respond(state->quiet, "ERR: Size must be 8-200\n");
            }
        }
        else if (strcmp(cmd, "position") == 0) {
            if (strcasecmp(arg, "top") == 0) {
                state->position = POS_TOP;
                pipe_respond(state->quiet, "OK\n");
            } else if (strcasecmp(arg, "center") == 0) {
                state->position = POS_CENTER;
                pipe_respond(state->quiet, "OK\n");
            } else if (strcasecmp(arg, "bottom") == 0) {
                state->position = POS_BOTTOM;
                pipe_respond(state->quiet, "OK\n");
            } else {
                pipe_respond(state->quiet, "ERR: Position must be top, center, or bottom\n");
            }
        }
        else if (strcmp(cmd, "bold") == 0) {
            state->style = (state->style == STYLE_ITALIC) ? STYLE_BOLD_ITALIC : STYLE_BOLD;
            pipe_respond(state->quiet, "OK\n");
        }
        else if (strcmp(cmd, "italic") == 0) {
            state->style = (state->style == STYLE_BOLD) ? STYLE_BOLD_ITALIC : STYLE_ITALIC;
            pipe_respond(state->quiet, "OK\n");
        }
        else if (strcmp(cmd, "regular") == 0) {
            state->style = STYLE_REGULAR;
            pipe_respond(state->quiet, "OK\n");
        }
        else if (strcmp(cmd, "font") == 0) {
            if (arg[0] == '\0') {
                state->custom_font[0] = '\0';  // Reset to default
                pipe_respond(state->quiet, "OK\n");
            } else {
                // Check if file exists
                if (access(arg, R_OK) == 0) {
                    strncpy(state->custom_font, arg, sizeof(state->custom_font) - 1);
                    state->custom_font[sizeof(state->custom_font) - 1] = '\0';
                    pipe_respond(state->quiet, "OK\n");
                } else {
                    pipe_respond(state->quiet, "ERR: Cannot read font file '%s'\n", arg);
                }
            }
        }
        else if (strcmp(cmd, "hide") == 0) {
            // Restore original screen from memory buffer
            restore_screen_from_buffer(&state->fb, state->saved_screen, state->saved_screen_size);
            pipe_respond(state->quiet, "OK\n");
        }
        else if (strcmp(cmd, "close") == 0) {
            int close_free = 0;
            int timeout = 0;
            char message[MAX_CMD_LEN];

            parse_close_args(arg, &close_free, &timeout, message, sizeof(message));

            // Show final message if provided
            if (message[0]) {
                pipe_show_status(state, message);
            }

            // Wait if timeout specified
            if (timeout > 0) {
                sleep(timeout);
            }

            // Restore original screen from memory buffer
            restore_screen_from_buffer(&state->fb, state->saved_screen, state->saved_screen_size);

            // Set free if requested
            if (close_free) {
                set_printer_busy(0);
            }

            pipe_respond(state->quiet, "OK\n");
            break;  // Exit command loop
        }
        else if (strcmp(cmd, "rpc") == 0) {
            // RPC command with progress display
            // Format: rpc <timeout> <json> [--match pat] [--extract mode] [--total n] [--template t]
            if (arg[0] == '\0') {
                pipe_respond(state->quiet, "ERR: rpc requires arguments\n");
            } else {
                rpc_execute(state, arg);
            }
        }
        else {
            pipe_respond(state->quiet, "ERR: Unknown command '%s'\n", cmd);
        }
    }

    return 0;
}

static void print_usage(const char *prog) {
    fprintf(stderr,
"fb_status - Framebuffer Status Display for Anycubic Printers\n\n"
"Usage:\n"
"  %s show \"message\" [options]  Display status message\n"
"  %s save                       Save current screen\n"
"  %s hide [options]             Restore saved screen\n"
"  %s busy                       Set printer busy\n"
"  %s free                       Set printer free\n"
"  %s pipe [options]             Piped input mode\n\n"
"Options:\n"
"  -c, --color COLOR     Text color (name or hex RGB)\n"
"  -g, --bg COLOR        Background color (default: 222222)\n"
"  -s, --size SIZE       Font size in pixels (default: 32)\n"
"  -p, --position POS    Position: top, center, bottom\n"
"  -t, --timeout SECS    Auto-hide after N seconds\n"
"  -b, --busy            Also set printer busy (with show)\n"
"  -f, --free            Also set printer free (with hide)\n"
"  -B, --bold            Use bold font\n"
"  -I, --italic          Use italic font\n"
"  -F, --font PATH       Custom font file path\n"
"  -m, --message MSG     Initial message (pipe mode)\n"
"  -q, --quiet           No response output (pipe mode)\n\n"
"Pipe mode commands (via stdin):\n"
"  busy                  Set printer busy\n"
"  free                  Set printer free\n"
"  show <message>        Display message (\\n for newline)\n"
"  color <color>         Set text color\n"
"  bg <color>            Set background color\n"
"  size <n>              Set font size\n"
"  position <pos>        Set position\n"
"  bold/italic/regular   Set font style\n"
"  font <path>           Set font file\n"
"  hide                  Restore saved screen temporarily\n"
"  close [-f] [secs] [msg]  Close and exit\n"
"  rpc <timeout> <json> [options]  Execute RPC with progress\n\n"
"RPC options:\n"
"  --match \"pattern\"     Pattern with {F}=float {D}=int {S}=string {*}=skip\n"
"  --extract <mode>      count, unique, last, or sum\n"
"  --total <n>           Total for percentage calculation\n"
"  --template \"text\"     Display template with {count} {total} {percent} {bar} {$1}...\n"
"  --interval <ms>       Update rate limit (default: 1000ms)\n"
"  --on-error \"text\"     Template on error\n\n"
"Colors: green, red, yellow, blue, white, black, orange, cyan,\n"
"        magenta, gray, pink, purple, or hex: FF0000, #00FF00\n\n"
"Examples:\n"
"  %s show \"Calibrating...\" -c green -b\n"
"  %s show \"Line 1\\nLine 2\" -c cyan\n"
"  %s pipe -m \"Starting...\" -b\n"
"  echo -e \"show Working...\\nclose -f 2 Done!\" | %s pipe -q\n"
"  rpc 3600 {\"method\":\"Probe\"} --match \"probe at {F},{F}\" --extract unique \\\n"
"      --total 64 --template \"Probing\\n{count}/{total}\\n{percent}%%\"\n",
    prog, prog, prog, prog, prog, prog, prog, prog, prog, prog);
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    const char *cmd = argv[1];
    const char *message = NULL;
    const char *color_name = "green";
    const char *bg_name = "222222";
    const char *custom_font = NULL;
    const char *initial_message = NULL;
    BoxPosition position = POS_BOTTOM;
    FontStyle style = STYLE_REGULAR;
    float font_size = DEFAULT_FONT_SIZE;
    int timeout_secs = 0;
    int set_busy = 0;
    int set_free = 0;
    int quiet = 0;

    // Parse arguments
    int arg_idx = 2;

    if (strcmp(cmd, "show") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Error: 'show' requires a message\n");
            return 1;
        }
        message = argv[2];
        arg_idx = 3;
    }

    // Parse options
    for (int i = arg_idx; i < argc; i++) {
        if ((strcmp(argv[i], "-c") == 0 || strcmp(argv[i], "--color") == 0) && i + 1 < argc) {
            color_name = argv[++i];
        }
        else if ((strcmp(argv[i], "-g") == 0 || strcmp(argv[i], "--bg") == 0) && i + 1 < argc) {
            bg_name = argv[++i];
        }
        else if ((strcmp(argv[i], "-s") == 0 || strcmp(argv[i], "--size") == 0) && i + 1 < argc) {
            font_size = atof(argv[++i]);
            if (font_size < 8) font_size = 8;
            if (font_size > 200) font_size = 200;
        }
        else if ((strcmp(argv[i], "-p") == 0 || strcmp(argv[i], "--position") == 0) && i + 1 < argc) {
            i++;
            if (strcasecmp(argv[i], "top") == 0) position = POS_TOP;
            else if (strcasecmp(argv[i], "center") == 0) position = POS_CENTER;
            else position = POS_BOTTOM;
        }
        else if ((strcmp(argv[i], "-t") == 0 || strcmp(argv[i], "--timeout") == 0) && i + 1 < argc) {
            timeout_secs = atoi(argv[++i]);
        }
        else if ((strcmp(argv[i], "-F") == 0 || strcmp(argv[i], "--font") == 0) && i + 1 < argc) {
            custom_font = argv[++i];
        }
        else if ((strcmp(argv[i], "-m") == 0 || strcmp(argv[i], "--message") == 0) && i + 1 < argc) {
            initial_message = argv[++i];
        }
        else if (strcmp(argv[i], "-b") == 0 || strcmp(argv[i], "--busy") == 0) {
            set_busy = 1;
        }
        else if (strcmp(argv[i], "-f") == 0 || strcmp(argv[i], "--free") == 0) {
            set_free = 1;
        }
        else if (strcmp(argv[i], "-q") == 0 || strcmp(argv[i], "--quiet") == 0) {
            quiet = 1;
        }
        else if (strcmp(argv[i], "-B") == 0 || strcmp(argv[i], "--bold") == 0) {
            style = (style == STYLE_ITALIC) ? STYLE_BOLD_ITALIC : STYLE_BOLD;
        }
        else if (strcmp(argv[i], "-I") == 0 || strcmp(argv[i], "--italic") == 0) {
            style = (style == STYLE_BOLD) ? STYLE_BOLD_ITALIC : STYLE_ITALIC;
        }
    }

    int ret = 0;

    // Commands without framebuffer
    if (strcmp(cmd, "save") == 0) {
        // Wake display before saving to ensure we capture visible content
        g_orientation = detect_orientation();
        wake_display();
        ret = save_screen();
        if (ret == 0) printf("Screen saved\n");
        return ret;
    }
    else if (strcmp(cmd, "hide") == 0) {
        ret = restore_screen();
        cleanup_backup();
        if (ret == 0) printf("Screen restored\n");
        if (set_free) set_printer_busy(0);
        return ret;
    }
    else if (strcmp(cmd, "busy") == 0) {
        return set_printer_busy(1);
    }
    else if (strcmp(cmd, "free") == 0) {
        return set_printer_busy(0);
    }
    else if (strcmp(cmd, "pipe") == 0) {
        // Pipe mode

        // Check for existing instance
        if (check_existing_instance() < 0) {
            return 1;
        }

        // Write PID file
        if (write_pid_file() < 0) {
            return 1;
        }

        // Acquire lock on backup image
        int lock_fd = acquire_lock();
        if (lock_fd < 0) {
            remove_pid_file();
            return 1;
        }

        // Set up signal handlers for cleanup
        signal(SIGINT, signal_handler);
        signal(SIGTERM, signal_handler);
        signal(SIGHUP, signal_handler);

        // Initialize pipe state
        PipeState state = {
            .text_color = parse_color(color_name),
            .bg_color = parse_color(bg_name),
            .font_size = font_size,
            .position = position,
            .style = style,
            .quiet = quiet,
            .lock_fd = lock_fd,
            .fb = {0},
            .work_buffer = NULL,
            .prev_box_x = 0, .prev_box_y = 0, .prev_box_w = 0, .prev_box_h = 0,
            .prev_bg_color = 0,
            .last_update_ms = 0,
            .min_update_interval_ms = 250  // 250ms minimum between updates (4 FPS max)
        };

        if (custom_font) {
            strncpy(state.custom_font, custom_font, sizeof(state.custom_font) - 1);
        }

        // Open framebuffer
        if (fb_open(&state.fb) < 0) {
            release_lock(lock_fd);
            remove_pid_file();
            return 1;
        }

        // Detect orientation
        g_orientation = detect_orientation();

        // Wake display before saving to ensure we capture visible content
        wake_display();

        // Save screen to memory buffer (for double-buffering)
        if (save_screen_to_buffer(&state.fb, &state.saved_screen, &state.saved_screen_size) != 0) {
            fprintf(stderr, "Failed to save screen to memory\n");
            fb_close(&state.fb);
            release_lock(lock_fd);
            remove_pid_file();
            return 1;
        }

        // Set busy if requested
        if (set_busy) {
            set_printer_busy(1);
        }

        // Run command loop
        ret = run_pipe_mode(&state, initial_message);

        // Cleanup - restore original screen from memory buffer
        restore_screen_from_buffer(&state.fb, state.saved_screen, state.saved_screen_size);

        // Free all allocated memory
        if (state.saved_screen) {
            free(state.saved_screen);
        }
        if (state.work_buffer) {
            free(state.work_buffer);
        }
        if (state.cached_font_data) {
            free(state.cached_font_data);
        }
        fb_close(&state.fb);
        release_lock(lock_fd);
        remove_pid_file();

        return ret;
    }

    // Show command
    if (strcmp(cmd, "show") != 0) {
        print_usage(argv[0]);
        return 1;
    }

    Framebuffer fb = {0};
    if (fb_open(&fb) < 0) {
        return 1;
    }

    g_orientation = detect_orientation();

    // Wake display before showing message (ensures it's visible)
    wake_display();

    if (set_busy) {
        set_printer_busy(1);
    }

    uint32_t text_color = parse_color(color_name);
    uint32_t bg_color = parse_color(bg_name);

    // Process escape sequences in message
    char msg_copy[MAX_CMD_LEN];
    strncpy(msg_copy, message, sizeof(msg_copy) - 1);
    msg_copy[sizeof(msg_copy) - 1] = '\0';
    process_escapes(msg_copy);

    ret = show_status(&fb, msg_copy, text_color, bg_color, position, font_size, custom_font, style);

    fb_close(&fb);

    // Auto-hide with timeout
    if (ret == 0 && timeout_secs > 0) {
        fprintf(stderr, "Auto-hide in %d seconds...\n", timeout_secs);
        sleep(timeout_secs);
        restore_screen();
        cleanup_backup();
        if (set_free) set_printer_busy(0);
    }

    return ret;
}
