#include "flash_config.h"
#include "nvs.h"
#include "esp_log.h"
#include "string.h"

static bool nvs_init_done = false;

#define TAG "flash_config"


esp_err_t read_app_config(struct ConfigData *app_config) {
    nvs_handle_t config_storage_handle;
    esp_err_t last = ESP_OK;
    if (!nvs_init_done) {
        esp_err_t err;
        err = nvs_open(app_config_section, NVS_READONLY, &config_storage_handle);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Error (%s) opening NVS handle!\n", esp_err_to_name(err));
            return err;
        }
        struct strings {
            char *name;
            char *dest;
            size_t len;
        } strings[] = {
                {.name = wifi_ssid, .dest = (char *) app_config->wifi_config.sta.ssid, .len=sizeof app_config->wifi_config.sta.ssid},
                {.name = wifi_pass, .dest = (char *) app_config->wifi_config.sta.password, .len=sizeof app_config->wifi_config.sta.password},
                {.name = app_location, .dest = app_config->location, .len=NAME_LEN},
                {.name = app_mqtt_key, .dest = app_config->network.mqtt_uri, .len=sizeof app_config->network.mqtt_uri},
        };
        for (int i = 0; i < sizeof strings / sizeof strings[0]; i++) {
            ESP_LOGI(TAG, "[%i] Reading config parameter %s", i, strings[i].name);
            err = nvs_get_str(config_storage_handle, strings[i].name, strings[i].dest, &strings[i].len);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Error (%s) reading %s\n", esp_err_to_name(err), strings[i].name);
                last = err;
            }
            ESP_LOGI(TAG, "Read parameter %s: [%s] success", strings[i].name, strings[i].dest);
        }
        nvs_close(config_storage_handle);
        nvs_init_done = true;
    }
    return last;
}

esp_err_t read_web_file(char *name, char data[], size_t *len) {
    nvs_handle_t config_storage_handle;
    esp_err_t err = nvs_open("web_config", NVS_READONLY, &config_storage_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Can't open NVS storage: %s", esp_err_to_name(err));
        return err;
    }
    if ((err = nvs_get_blob(config_storage_handle, name, data, len)) != ESP_OK) {
        ESP_LOGE(TAG, "Can't read blob %s: %s", name, esp_err_to_name(err));
        nvs_close(config_storage_handle);
        return err;
    }
    nvs_close(config_storage_handle);
    return ESP_OK;
}

esp_err_t update_app_config(char *ssid, char *password, char *location) {
    nvs_handle_t config_storage_handle;
    esp_err_t err = nvs_open(app_config_section, NVS_READWRITE, &config_storage_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "can't open app_config: %s", esp_err_to_name(err));
        return err;
    }
    if (ssid != 0 && strlen(ssid) > 0) {
        if ((err = nvs_set_str(config_storage_handle, wifi_ssid, ssid)) != ESP_OK) {
            ESP_LOGE(TAG, "can't set ssid: %s", esp_err_to_name(err));
            nvs_close(config_storage_handle);
            return err;
        }
    } else {
        ESP_LOGW(TAG, "ignoring empty ssid");
    }
    if (password != 0 && strlen(password) > 0) {
        if ((err = nvs_set_str(config_storage_handle, wifi_pass, password)) != ESP_OK) {
            ESP_LOGE(TAG, "can't set password: %s", esp_err_to_name(err));
            nvs_close(config_storage_handle);
            return err;
        }
    } else {
        ESP_LOGW(TAG, "ignoring empty password");
    }
    if (location != 0 && strlen(location) > 0) {
        if ((err = nvs_set_str(config_storage_handle, app_location, location)) != ESP_OK) {
            ESP_LOGE(TAG, "can't set location: %s", esp_err_to_name(err));
            nvs_close(config_storage_handle);
            return err;
        }
    } else {
        ESP_LOGW(TAG, "ignoring empty location");
    }
    nvs_commit(config_storage_handle);
    nvs_close(config_storage_handle);
    return ESP_OK;
}