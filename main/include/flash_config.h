#pragma once

#include "esp_err.h"
#include "config.h"

static char *const wifi_ssid = "wifi_ssid";

static char *const wifi_pass = "wifi_password";

static char *const app_location = "app_location";

static char *const app_mqtt_key = "app_mqtt_url";

static char *const app_config_section = "app_config";

esp_err_t read_app_config(struct ConfigData *app_config);

esp_err_t update_app_config(char *ssid, char *password, char *location);

esp_err_t read_web_file(char *name, char data[], size_t *len);