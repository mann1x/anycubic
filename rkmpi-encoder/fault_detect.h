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

/* Model input dimensions */
#define FD_MODEL_INPUT_SIZE  224
#define FD_MODEL_INPUT_BYTES (FD_MODEL_INPUT_SIZE * FD_MODEL_INPUT_SIZE * 3)

/* Maximum models per class */
#define FD_MAX_MODELS 8

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
    FD_STRATEGY_CNN,        /* CNN only */
    FD_STRATEGY_PROTONET,   /* ProtoNet only */
    FD_STRATEGY_MULTICLASS  /* Multiclass only */
} fd_strategy_t;

/* Model class type */
typedef enum {
    FD_MODEL_CNN = 0,
    FD_MODEL_PROTONET,
    FD_MODEL_MULTICLASS
} fd_model_class_t;

/* Scanned model info */
typedef struct {
    char name[64];          /* Subdirectory name, e.g. "sweep-ls005_v1" */
    char path[256];         /* Full path to model directory */
    fd_model_class_t cls;   /* Model class type */
} fd_model_info_t;

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
    char cnn_model[64];         /* Selected CNN model subdir */
    char proto_model[64];       /* Selected ProtoNet model subdir */
    char multi_model[64];       /* Selected Multiclass model subdir */
    int min_free_mem_mb;        /* Min free memory to run (default 20) */
    int pace_ms;                /* Inter-step pause ms to reduce CPU spikes (0=off) */
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

/* Scan model directories. Returns number of models found.
 * models array must have room for max_models entries. */
int fault_detect_scan_models(fd_model_info_t *models, int max_models);

/* Check if NPU runtime is available (dlopen succeeded). */
int fault_detect_npu_available(void);

/* Strategy name helpers */
const char *fd_strategy_name(fd_strategy_t strategy);
fd_strategy_t fd_strategy_from_name(const char *name);

/* Fault class name helper */
const char *fd_fault_class_name(int fault_class);

#endif /* FAULT_DETECT_H */
