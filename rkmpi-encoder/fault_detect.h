/*
 * Fault Detection Module
 *
 * Real-time 3D print fault detection using RKNN NPU hardware.
 * Three model types (CNN, ProtoNet, Multiclass) run on the NPU
 * to detect print failures from camera JPEG frames.
 *
 * RKNN runtime is loaded via dlopen() so the encoder binary
 * works on printers without NPU hardware.
 */

#ifndef FAULT_DETECT_H
#define FAULT_DETECT_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>

/* Model input dimensions (448x224 = 2:1 aspect for 16:9 cameras) */
#define FD_MODEL_INPUT_WIDTH  448
#define FD_MODEL_INPUT_HEIGHT 224
#define FD_MODEL_INPUT_BYTES (FD_MODEL_INPUT_WIDTH * FD_MODEL_INPUT_HEIGHT * 3)

/* Spatial heatmap grid max dimensions (actual read from model/prototypes) */
#define FD_SPATIAL_H_MAX     14
#define FD_SPATIAL_W_MAX     28
#define FD_SPATIAL_EMB_MAX   1024  /* max embedding dim (old=1024, new=232) */

/* ============================================================================
 * 392-bit mask type for 14x28 grid (7 x uint64_t = 448 bits)
 * ============================================================================ */

#define FD_MASK_WORDS 7   /* 7 x 64 = 448 bits >= 14*28 = 392 */

typedef struct {
    uint64_t w[FD_MASK_WORDS]; /* w[0]=bits 0-63, ... w[6]=bits 384-447 */
} fd_mask196_t;

static inline void fd_mask_clear(fd_mask196_t *m) {
    for (int i = 0; i < FD_MASK_WORDS; i++) m->w[i] = 0;
}

static inline void fd_mask_set_bit(fd_mask196_t *m, int bit) {
    if (bit >= 0 && bit < FD_MASK_WORDS * 64)
        m->w[bit / 64] |= (1ULL << (bit % 64));
}

static inline int fd_mask_test_bit(const fd_mask196_t *m, int bit) {
    if (bit < 0 || bit >= FD_MASK_WORDS * 64) return 0;
    return (m->w[bit / 64] & (1ULL << (bit % 64))) != 0;
}

static inline int fd_mask_is_zero(const fd_mask196_t *m) {
    uint64_t v = 0;
    for (int i = 0; i < FD_MASK_WORDS; i++) v |= m->w[i];
    return v == 0;
}

/* Set all bits [0..n-1] */
static inline fd_mask196_t fd_mask_all_ones(int n) {
    fd_mask196_t m;
    fd_mask_clear(&m);
    for (int i = 0; i < FD_MASK_WORDS; i++) {
        if (n >= 64) {
            m.w[i] = ~0ULL;
            n -= 64;
        } else if (n > 0) {
            m.w[i] = (1ULL << n) - 1;
            n = 0;
        }
    }
    return m;
}

/* Convert from legacy uint64_t (49-bit) */
static inline fd_mask196_t fd_mask_from_u64(uint64_t v) {
    fd_mask196_t m;
    fd_mask_clear(&m);
    m.w[0] = v;
    return m;
}

/* Convert to uint64_t (lossy — only lower 64 bits) */
static inline uint64_t fd_mask_to_u64(const fd_mask196_t *m) {
    return m->w[0];
}

/* Serialize to hex: "w6:w5:...:w0" (118 chars + null) */
static inline void fd_mask_to_hex(const fd_mask196_t *m, char *buf, size_t buf_size) {
    if (buf_size < 120) { if (buf_size > 0) buf[0] = '\0'; return; }
    snprintf(buf, buf_size,
             "%016llx:%016llx:%016llx:%016llx:%016llx:%016llx:%016llx",
             (unsigned long long)m->w[6], (unsigned long long)m->w[5],
             (unsigned long long)m->w[4], (unsigned long long)m->w[3],
             (unsigned long long)m->w[2], (unsigned long long)m->w[1],
             (unsigned long long)m->w[0]);
}

/* Parse hex "w6:w5:...:w0" → mask. Returns 0=ok, -1=error.
 * Also accepts legacy 4-word "w3:w2:w1:w0" and single hex formats. */
static inline int fd_mask_from_hex(const char *hex, fd_mask196_t *m) {
    fd_mask_clear(m);
    if (!hex || !hex[0]) return -1;
    /* Try 7-word colon-separated format */
    if (sscanf(hex, "%llx:%llx:%llx:%llx:%llx:%llx:%llx",
               (unsigned long long *)&m->w[6], (unsigned long long *)&m->w[5],
               (unsigned long long *)&m->w[4], (unsigned long long *)&m->w[3],
               (unsigned long long *)&m->w[2], (unsigned long long *)&m->w[1],
               (unsigned long long *)&m->w[0]) == 7)
        return 0;
    /* Fallback: legacy 4-word format */
    if (sscanf(hex, "%llx:%llx:%llx:%llx",
               (unsigned long long *)&m->w[3], (unsigned long long *)&m->w[2],
               (unsigned long long *)&m->w[1], (unsigned long long *)&m->w[0]) == 4)
        return 0;
    /* Fallback: single hex number (legacy uint64_t) */
    unsigned long long v = 0;
    if (sscanf(hex, "%llx", &v) == 1) {
        m->w[0] = v;
        return 0;
    }
    return -1;
}

/* Population count */
static inline int fd_mask_popcount(const fd_mask196_t *m) {
    int count = 0;
    for (int i = 0; i < FD_MASK_WORDS; i++) {
        uint64_t v = m->w[i];
        while (v) { count++; v &= v - 1; }
    }
    return count;
}

/* Z-dependent mask table limits */
#define FD_Z_MASK_MAX_ENTRIES 48

/* Model set limits */
#define FD_MAX_SETS        4    /* Max model sets to scan */
#define FD_MAX_PROFILES    8    /* Max threshold profiles per set */
#define FD_SET_NAME_LEN    64
#define FD_PROFILE_NAME_LEN 32
#define FD_DISPLAY_NAME_LEN 64

/* Multi-class fault type indices (alphabetical ImageFolder order) */
#define FD_MCLASS_CRACKING       0
#define FD_MCLASS_LAYERSHIFTING  1
#define FD_MCLASS_SPAGHETTI      2
#define FD_MCLASS_STRINGING      3
#define FD_MCLASS_SUCCESS        4
#define FD_MCLASS_UNDEREXTRUSION 5
#define FD_MCLASS_WARPING        6
#define FD_MCLASS_COUNT          7

/* Binary classification */
#define FD_CLASS_FAULT 0
#define FD_CLASS_OK    1

/* Detection status */
typedef enum {
    FD_STATUS_DISABLED = 0,
    FD_STATUS_ENABLED,      /* Idle, waiting for next cycle */
    FD_STATUS_ACTIVE,       /* Running inference */
    FD_STATUS_ERROR,
    FD_STATUS_NO_NPU,
    FD_STATUS_MEM_LOW
} fd_status_t;

/* Voting strategy */
typedef enum {
    FD_STRATEGY_OR = 0,     /* FAULT if any model says FAULT */
    FD_STRATEGY_MAJORITY,   /* FAULT if majority agree */
    FD_STRATEGY_ALL,        /* FAULT only if all agree */
    FD_STRATEGY_VERIFY,     /* 2-model OR, then multiclass confirms */
    FD_STRATEGY_CLASSIFY,   /* 2-model OR decides, multiclass adds type */
    FD_STRATEGY_CLASSIFY_AND, /* 2-model AND decides, multiclass adds type */
    FD_STRATEGY_AND,        /* FAULT only if CNN AND ProtoNet agree (no multiclass) */
    FD_STRATEGY_CNN,        /* CNN only */
    FD_STRATEGY_PROTONET,   /* ProtoNet only */
    FD_STRATEGY_MULTICLASS  /* Multiclass only */
} fd_strategy_t;

/* Model class type */
typedef enum {
    FD_MODEL_CNN = 0,
    FD_MODEL_PROTONET,
    FD_MODEL_MULTICLASS,
    FD_MODEL_SPATIAL,      /* spatial encoder (protonet without GAP) */
    FD_MODEL_SPATIAL_COARSE /* coarse spatial encoder (7x7x1024 for multi-scale) */
} fd_model_class_t;

/* Threshold profile — one profile covers ALL model types in the set */
typedef struct {
    char name[FD_PROFILE_NAME_LEN];       /* Profile key, e.g. "KS1" */
    char description[128];
    float cnn_threshold;                   /* CNN static threshold */
    float cnn_dynamic_threshold;           /* CNN lowered threshold (Proto gate) */
    float proto_threshold;                 /* ProtoNet margin threshold */
    float proto_dynamic_trigger;           /* ProtoNet margin that gates CNN */
    float multi_threshold;                 /* Multiclass binary threshold */
    float heatmap_boost_threshold;         /* Spatial heatmap boost trigger */
} fd_threshold_profile_t;

/* Model set info — discovered by scanning */
typedef struct {
    char dir_name[FD_SET_NAME_LEN];       /* Directory name, e.g. "Edge-FDM-Models-KS1" */
    char path[256];                        /* Full path to set directory */
    char display_name[FD_DISPLAY_NAME_LEN]; /* From metadata.json "name" field */
    char description[128];                 /* From metadata.json "description" field */
    int has_cnn;                           /* 1 if cnn/ subdir with valid .rknn */
    int has_protonet;                      /* 1 if protonet/ subdir with valid files */
    int has_multiclass;                    /* 1 if multiclass/ subdir with valid .rknn */
    char cnn_display_name[FD_DISPLAY_NAME_LEN];
    char proto_display_name[FD_DISPLAY_NAME_LEN];
    char multi_display_name[FD_DISPLAY_NAME_LEN];
    char cnn_file[64];                     /* model filename override */
    char proto_file[64];                   /* encoder filename override */
    char proto_prototypes[64];             /* prototypes filename override */
    char proto_spatial_prototypes[64];     /* spatial prototypes filename override */
    char multi_file[64];                   /* multiclass filename override */
    fd_threshold_profile_t profiles[FD_MAX_PROFILES];
    int num_profiles;
} fd_model_set_t;

/* Z-dependent mask entry */
typedef struct {
    float z_mm;        /* Z height in mm */
    fd_mask196_t mask; /* grid mask at this Z height */
} fd_z_mask_entry_t;

/* Active threshold config (runtime) */
typedef struct {
    int use_custom;                        /* 0 = profile, 1 = custom */
    char profile[FD_PROFILE_NAME_LEN];    /* Selected profile name */
    float cnn_threshold;                   /* Active values (profile or custom) */
    float cnn_dynamic_threshold;
    float proto_threshold;
    float proto_dynamic_trigger;
    float multi_threshold;
    float heatmap_boost_threshold;
} fd_active_thresholds_t;

/* Detection result (last inference cycle) */
typedef struct {
    int result;             /* FD_CLASS_FAULT or FD_CLASS_OK */
    float confidence;       /* Combined confidence [0.0, 1.0] */
    int fault_class;        /* FD_MCLASS_* index (multiclass only) */
    char fault_class_name[32]; /* Human-readable fault type */
    float total_ms;         /* Total inference time */
    float cnn_ms;
    float proto_ms;
    float multi_ms;
    int agreement;          /* Number of models agreeing */
    /* Per-model confidence detail */
    int cnn_ran;             /* 1 if CNN ran this cycle */
    int proto_ran;           /* 1 if ProtoNet ran this cycle */
    int multi_ran;           /* 1 if Multiclass ran this cycle */
    float cnn_raw;           /* CNN raw: softmax fail prob [0,1] */
    float proto_raw;         /* ProtoNet raw: cosine margin (~[-1,1]) */
    float multi_raw;         /* Multiclass raw: 1-p(success) [0,1] */
    float cnn_fault_lk;     /* CNN normalized fault likelihood [0,1] */
    float proto_fault_lk;   /* ProtoNet normalized fault likelihood [0,1] */
    float multi_fault_lk;   /* Multiclass normalized fault likelihood [0,1] */
    int cnn_vote;            /* 1=fault, 0=ok */
    int proto_vote;          /* 1=fault, 0=ok */
    int multi_vote;          /* 1=fault, 0=ok */
    /* Spatial heatmap */
    int has_heatmap;
    int spatial_h;           /* actual grid rows (7 or 14) */
    int spatial_w;           /* actual grid cols (7 or 14) */
    float heatmap[FD_SPATIAL_H_MAX][FD_SPATIAL_W_MAX]; /* cosine margin per location */
    float heatmap_max;       /* max fault margin in grid */
    int heatmap_max_h;       /* row index of max */
    int heatmap_max_w;       /* col index of max */
    float spatial_ms;        /* spatial inference time */
    int boost_active;        /* 1 if spatial boost conditions met */
    int boost_overrode;      /* 1 if boost actually changed OK→FAULT */
    int boost_strong_cells;  /* number of strong cells when boost fired */
    int boost_total_cells;   /* total active cells when boost fired */
    /* Center-crop region in normalized [0,1] coords */
    float crop_x, crop_y, crop_w, crop_h;
} fd_result_t;

/* Detection state (thread-safe snapshot) */
typedef struct {
    fd_status_t status;
    fd_result_t last_result;
    uint64_t last_check_time;   /* Unix timestamp of last check */
    uint64_t cycle_count;       /* Total inference cycles */
    char error_msg[128];        /* Last error message */
} fd_state_t;

/* Detection configuration */
typedef struct {
    int enabled;
    int cnn_enabled;
    int proto_enabled;
    int multi_enabled;
    fd_strategy_t strategy;
    int interval_s;             /* Normal check interval (default 5) */
    int verify_interval_s;      /* Verification interval (default 2) */
    char model_set[FD_SET_NAME_LEN]; /* Selected model set directory name */
    int min_free_mem_mb;        /* Min free memory to run (default 20) */
    int pace_ms;                /* Inter-step pause ms to reduce CPU spikes (0=off) */
    fd_active_thresholds_t thresholds; /* Active thresholds (profile or custom) */
    int heatmap_enabled;        /* 0=off, 1=on (spatial heatmap on faults) */
    int beep_pattern;           /* Buzzer alert: 0=none, 1-5=patterns */
    int setup_mode;             /* 1 = force heatmap every cycle (for calibration wizard) */
    fd_mask196_t heatmap_mask;  /* grid mask: 1=active cell, 0=excluded from confidence */
    fd_z_mask_entry_t z_masks[FD_Z_MASK_MAX_ENTRIES];
    int z_mask_count;           /* 0 = disabled, use static heatmap_mask */
    /* File overrides from metadata.json (populated by scan) */
    char cnn_file[64];
    char proto_file[64];
    char proto_prototypes[64];
    char multi_file[64];
} fd_config_t;

/* ============================================================================
 * Public API
 * ============================================================================ */

/* Initialize fault detection: scan models directory, dlopen RKNN runtime.
 * Call once at startup. Returns 0 on success, -1 on error. */
int fault_detect_init(const char *models_base_dir);

/* Start detection thread. Runs a validation pass first (loads/unloads each
 * enabled model). Returns 0 on success, -1 if validation fails. */
int fault_detect_start(void);

/* Stop detection thread (blocks until thread exits). */
void fault_detect_stop(void);

/* Release all resources. Call at shutdown. */
void fault_detect_cleanup(void);

/* Feed JPEG frame from main capture loop.
 * Only copies data when detection thread needs it (flag-based). */
void fault_detect_feed_jpeg(const uint8_t *data, size_t size);

/* Get current state (thread-safe copy). */
fd_state_t fault_detect_get_state(void);

/* Get current config. */
fd_config_t fault_detect_get_config(void);

/* Update config. If thread is running, changes take effect next cycle. */
void fault_detect_set_config(const fd_config_t *config);

/* Scan for model sets. Returns number of sets found.
 * sets array must have room for max_sets entries. */
int fault_detect_scan_sets(fd_model_set_t *sets, int max_sets);

/* Check if NPU runtime is available (dlopen succeeded). */
int fault_detect_npu_available(void);

/* Check if fault detection is installed (models directory exists). */
int fault_detect_installed(void);

/* Warmup CMA by loading/unloading all configured models.
 * Call before VENC operations to pre-heat CMA allocation paths.
 * Returns number of models successfully loaded, or -1 on error. */
int fault_detect_warmup(void);

/* Check if the FD thread is waiting for a frame (non-blocking). */
int fault_detect_needs_frame(void);

/* Set current Z height (called from Moonraker position updates). */
void fault_detect_set_current_z(float z_mm);

/* Set Z-dependent mask table. entries must be sorted by z_mm ascending.
 * Pass NULL/0 to clear. Copies entries into internal storage. */
void fault_detect_set_z_masks(const fd_z_mask_entry_t *entries, int count);

/* Strategy name helpers */
const char *fd_strategy_name(fd_strategy_t strategy);
fd_strategy_t fd_strategy_from_name(const char *name);

/* Fault class name helper */
const char *fd_fault_class_name(int fault_class);

/* Get current spatial grid dimensions (0,0 if not loaded yet) */
void fault_detect_get_spatial_dims(int *h, int *w);

/* Get center-crop region in normalized [0,1] coords.
 * Returns {0,0,1,1} if not yet computed (no image decoded). */
void fault_detect_get_crop(float *x, float *y, float *w, float *h);

/* Get the last JPEG frame used for FD inference.
 * Copies into caller's buffer. Returns bytes copied, 0 if none available.
 * cycle_out receives the cycle_count of the frame (NULL to skip). */
size_t fault_detect_get_fd_frame(uint8_t *buf, size_t max_size, uint64_t *cycle_out);

#endif /* FAULT_DETECT_H */
