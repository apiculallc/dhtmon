#pragma once

#include "esp_wifi_types.h"

#define NAME_LEN 100
#define BUILTIN_LED GPIO_NUM_2

struct ConfigData {
    wifi_config_t wifi_config;
    char location[NAME_LEN];
    uint8_t LPZS; // zero byte
    struct network {
        char mqtt_uri[100];
    } network;
};
