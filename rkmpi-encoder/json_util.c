/*
 * JSON Configuration Utilities Implementation
 */

#include "json_util.h"
#include "cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

char *json_read_file(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) {
        return NULL;
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (size <= 0 || size > 65536) {  /* Sanity check */
        fclose(f);
        return NULL;
    }

    char *buffer = malloc(size + 1);
    if (!buffer) {
        fclose(f);
        return NULL;
    }

    size_t read = fread(buffer, 1, size, f);
    fclose(f);

    if (read != (size_t)size) {
        free(buffer);
        return NULL;
    }

    buffer[size] = '\0';
    return buffer;
}

int json_load_mqtt_credentials(MQTTCredentials *creds) {
    if (!creds) {
        return -1;
    }

    memset(creds, 0, sizeof(*creds));

    char *content = json_read_file(DEVICE_ACCOUNT_PATH);
    if (!content) {
        return -1;
    }

    cJSON *root = cJSON_Parse(content);
    free(content);

    if (!root) {
        return -1;
    }

    cJSON *device_id = cJSON_GetObjectItemCaseSensitive(root, "deviceId");
    cJSON *username = cJSON_GetObjectItemCaseSensitive(root, "username");
    cJSON *password = cJSON_GetObjectItemCaseSensitive(root, "password");

    if (cJSON_IsString(device_id) && device_id->valuestring) {
        strncpy(creds->device_id, device_id->valuestring, MAX_DEVICE_ID_LEN - 1);
    }
    if (cJSON_IsString(username) && username->valuestring) {
        strncpy(creds->username, username->valuestring, MAX_USERNAME_LEN - 1);
    }
    if (cJSON_IsString(password) && password->valuestring) {
        strncpy(creds->password, password->valuestring, MAX_PASSWORD_LEN - 1);
    }

    cJSON_Delete(root);

    /* Check if we got at least device_id */
    creds->valid = (creds->device_id[0] != '\0');
    return creds->valid ? 0 : -1;
}

int json_load_device_config(DeviceConfig *config) {
    if (!config) {
        return -1;
    }

    memset(config, 0, sizeof(*config));

    char *content = json_read_file(API_CONFIG_PATH);
    if (!content) {
        return -1;
    }

    cJSON *root = cJSON_Parse(content);
    free(content);

    if (!root) {
        return -1;
    }

    /* Navigate to cloud.modelId */
    cJSON *cloud = cJSON_GetObjectItemCaseSensitive(root, "cloud");
    if (cloud) {
        cJSON *model_id = cJSON_GetObjectItemCaseSensitive(cloud, "modelId");
        if (cJSON_IsString(model_id) && model_id->valuestring) {
            strncpy(config->model_id, model_id->valuestring, MAX_MODEL_ID_LEN - 1);
        }
    }

    cJSON_Delete(root);

    config->valid = (config->model_id[0] != '\0');
    return config->valid ? 0 : -1;
}
