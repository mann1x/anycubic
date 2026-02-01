/*
 * JSON Configuration Utilities
 *
 * Load MQTT credentials and device configuration from JSON files.
 */

#ifndef JSON_UTIL_H
#define JSON_UTIL_H

/* Configuration file paths */
#define DEVICE_ACCOUNT_PATH  "/userdata/app/gk/config/device_account.json"
#define API_CONFIG_PATH      "/userdata/app/gk/config/api.cfg"

/* Maximum string lengths */
#define MAX_DEVICE_ID_LEN    64
#define MAX_USERNAME_LEN     128
#define MAX_PASSWORD_LEN     128
#define MAX_MODEL_ID_LEN     32

/* MQTT credentials structure */
typedef struct {
    char device_id[MAX_DEVICE_ID_LEN];
    char username[MAX_USERNAME_LEN];
    char password[MAX_PASSWORD_LEN];
    int valid;  /* 1 if credentials loaded successfully */
} MQTTCredentials;

/* Device configuration */
typedef struct {
    char model_id[MAX_MODEL_ID_LEN];
    int valid;  /* 1 if config loaded successfully */
} DeviceConfig;

/* Load MQTT credentials from device_account.json */
int json_load_mqtt_credentials(MQTTCredentials *creds);

/* Load model ID from api.cfg */
int json_load_device_config(DeviceConfig *config);

/* Helper: read entire file into buffer (caller must free) */
char *json_read_file(const char *path);

#endif /* JSON_UTIL_H */
