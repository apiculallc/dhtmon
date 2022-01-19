#include <stdio.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <dht.h>
#include "lwip/err.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "event_groups.h"
#include "esp_task_wdt.h"
#include <stdbool.h>
#include "driver/gpio.h"

#define WAIT_CONNECT (5000)
#define WIFI_CONNECT_RETRIES (10)
#define WIFI_CONNECT_RETRY_DELAY_MS (10000)
#define TAG "DHT_MONITOR"
#define SEND_RETRIES (5)
#define SEND_RETRY_INTERVAL_MS (5000)
#define REPORT_INTERVAL_MS (SEND_RETRIES * SEND_RETRY_INTERVAL_MS + 100)
#define WIFI_CONNECTED_BIT     BIT0
#define WIFI_FAIL_BIT          BIT1
#define BUILTIN_LED GPIO_NUM_2

static int s_retry_num = 0;
static const dht_sensor_type_t sensor_type = DHT_TYPE_AM2301;
//static const dht_sensor_type_t sensor_type = DHT_TYPE_DHT11;
static const gpio_num_t dht_gpio = 4;
const char *endpoint;
static EventGroupHandle_t wifi_event_group;
const struct timeval socket_timeout = {
        .tv_sec = 5,
        .tv_usec = 0
};

static void unified_event_handler(void *arg, esp_event_base_t event_base,
                                  int32_t event_id, void *event_data) {
    if (event_base == WIFI_EVENT) {
        ESP_LOGD(TAG, "WIFI_EVENT: %d", event_id);
        switch (event_id) {
            case WIFI_EVENT_STA_DISCONNECTED:
                if (s_retry_num < CONFIG_ESP_MAXIMUM_RETRY) {
                    ESP_LOGI(TAG, "retrying AP connection");
                    s_retry_num++;
                    esp_wifi_connect();
                    xEventGroupClearBits(wifi_event_group, WIFI_CONNECTED_BIT);
                } else {
                    // reboot ESP as we're likely have issues with connecting
                    esp_restart();
                }
                ESP_LOGI(TAG, "AP disconnected");
                break;
        }
    } else if (event_base == IP_EVENT) {
        ESP_LOGD(TAG, "IP_EVENT: %d", event_id);
        ip_event_got_ip_t *event;
        switch (event_id) {
            case IP_EVENT_STA_GOT_IP:
                event = (ip_event_got_ip_t *) event_data;
                ESP_LOGI(TAG, "got ip: %s", ip4addr_ntoa((const ip4_addr_t *) &event->ip_info.ip));
                s_retry_num = 0;
                xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
                break;
        }
    }
}

static bool connect_wifi() {
    if (!tcpip_adapter_is_netif_up(TCPIP_ADAPTER_IF_STA)) {
        ESP_LOGD(TAG, "starting TCP/IP adapter");
        ESP_ERROR_CHECK(tcpip_adapter_up(TCPIP_ADAPTER_IF_STA));
        ESP_LOGD(TAG, "TCP/IP adapter started");
    }
    tcpip_adapter_dhcp_status_t dhcp_status;
    if (tcpip_adapter_dhcpc_get_status(TCPIP_ADAPTER_IF_STA, &dhcp_status) == ERR_OK) {
        if (dhcp_status != TCPIP_ADAPTER_DHCP_STARTED) {
            ESP_LOGD(TAG, "starting DHCP client");
            ESP_ERROR_CHECK(tcpip_adapter_dhcpc_start(TCPIP_ADAPTER_IF_STA));
            ESP_LOGD(TAG, "DHCP client started");
        } else {
            ESP_LOGE(TAG, "DHCP status %d", dhcp_status);
        }
    }

    s_retry_num = 0;
    ESP_LOGI(TAG, "entering station mode...");

    wifi_config_t wifi_config = {
            .sta = {
                    .ssid = CONFIG_ESP_WIFI_SSID,
                    .password = CONFIG_ESP_WIFI_PASS
            },
    };

    /* Setting a password implies station will connect to all security modes including WEP/WPA.
     * However these modes are deprecated and not advisable to be used. Incase your Access point
     * doesn't support WPA2, these mode can be enabled by commenting below line */
    if (strlen((char *) wifi_config.sta.password)) {
        wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    }

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_connect());

    ESP_LOGI(TAG, "waiting for wifi connection");

    /* Waiting until either the connection is established (WIFI_CONNECTED_BIT) or connection failed for the maximum
     * number of re-tries (WIFI_FAIL_BIT). The bits are set by event_handler() (see above) */
    EventBits_t bits = xEventGroupWaitBits(wifi_event_group,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdTRUE,
                                           pdFALSE,
                                           pdMS_TO_TICKS(WAIT_CONNECT));
    /* xEventGroupWaitBits() returns the bits before the call returned, hence we can test which event actually
     * happened. */
    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "connected to ap SSID:%s", CONFIG_ESP_WIFI_SSID);
    } else if (bits & WIFI_FAIL_BIT) {
        ESP_LOGE(TAG, "Failed to connect to SSID:%s", CONFIG_ESP_WIFI_SSID);
    } else {
        ESP_LOGE(TAG, "NEITHER WIFI_CONNECTED_BIT NOR WIFI_FAIL_BIT ARE SET");
    }
    return bits & WIFI_CONNECTED_BIT;
}


void report_temp_humidity(TimerHandle_t pxTimer) {
    ESP_LOGD(TAG, "TIMER TASK");
    int16_t temperature = 0;
    int16_t humidity = 0;

    // DHT sensors that come mounted on a PCB generally have
    // pull-up resistors on the data pin.  It is recommended
    // to provide an external pull-up resistor otherwise...

    gpio_set_pull_mode(dht_gpio, GPIO_PULLUP_ONLY);

    if (dht_read_data(sensor_type, dht_gpio, &humidity, &temperature) == ESP_OK) {
        ESP_LOGI(TAG, "Humidity: %d%% Temp: %dC\n", humidity / 10, temperature / 10);
    } else {
        printf("Could not read data from sensor\n");
    }
    const struct addrinfo hints = {
            .ai_family = AF_INET,
            .ai_socktype = SOCK_STREAM,
    };
    struct addrinfo *res;
    struct in_addr *addr;
    static char host[255] = CONFIG_APP_NET_HOST;
    static char port[6] = CONFIG_APP_NET_PORT; // 0 - 65535
    static char uuid[] = CONFIG_APP_NET_UUID; // device ID

    static uint8_t buf[4 + (sizeof uuid) + 1];
    buf[4] = sizeof uuid;
    memcpy(&buf[5], uuid, sizeof uuid);
    int try = 0;
    for (; try < SEND_RETRIES; try++) {
        ESP_LOGI(TAG, "SENDING DATA ATTEMPT %d", try);
        int err = getaddrinfo(host, port, &hints, &res);
        if (err != 0 || res == NULL) {
            freeaddrinfo(res);
            ESP_LOGE(TAG, "DNS lookup failed err=%d res=%p", errno, res);
            vTaskDelay(pdMS_TO_TICKS(SEND_RETRY_INTERVAL_MS));
            continue;
        }

        uint16_t dstPort = ntohs(*(&((struct sockaddr_in *) res->ai_addr)->sin_port));

        addr = &((struct sockaddr_in *) res->ai_addr)->sin_addr;
        ESP_LOGI(TAG, "DNS lookup succeeded. IP=%s", inet_ntoa(*addr));

        int sndrSocket = socket(res->ai_family, res->ai_socktype, IPPROTO_TCP);
        if (sndrSocket < 0) {
            ESP_LOGE(TAG, "error opening socket to %s : %hu -> %d, %d", inet_ntoa(*addr), dstPort, sndrSocket,
                     errno);
            vTaskDelay(pdMS_TO_TICKS(SEND_RETRY_INTERVAL_MS));
            continue;
        }
        if ((setsockopt(sndrSocket, SOL_SOCKET, SO_RCVTIMEO, &socket_timeout,
                        sizeof socket_timeout)) < 0) {
            ESP_LOGE(TAG, "can't set receive timeout: %d", errno);
        }

        if (setsockopt(sndrSocket, SOL_SOCKET, SO_SNDTIMEO, &socket_timeout,
                       sizeof socket_timeout) < 0) {
            ESP_LOGE(TAG, "can't set send timeout: %d", errno);
        }
        err = connect(sndrSocket, res->ai_addr, res->ai_addrlen);
        if (err != 0) {
            ESP_LOGE(TAG, "error connecting to %s : %hu -> %d : %d", inet_ntoa(*addr), dstPort, sndrSocket, errno);
            if (sndrSocket != -1) {
                close(sndrSocket);
            }
            vTaskDelay(pdMS_TO_TICKS(SEND_RETRY_INTERVAL_MS));
            continue;
        }
        buf[0] = temperature & 0xff;
        buf[1] = (temperature >> 8) & 0xff;
        buf[2] = humidity & 0xff;
        buf[3] = (humidity >> 8) & 0xff;
        ESP_LOGI(TAG, "data: %d:%d:%d:%d", buf[0], buf[1], buf[2], buf[3]);

        err = send(sndrSocket, buf, sizeof buf, 0);
        if (err < 0) {
            ESP_LOGE(TAG, "can't send data to socket %d", errno);
            if (sndrSocket != -1) {
                close(sndrSocket);
            }
            vTaskDelay(pdMS_TO_TICKS(SEND_RETRY_INTERVAL_MS));
            continue;
        } else {
            ESP_LOGI(TAG, "WRITE : %d : '%s'", err, buf);
        }

        static char buf[3];
        memset(buf, 0, 3);
        err = recv(sndrSocket, buf, 2, 0);
        if (err < 0) {
            ESP_LOGE(TAG, "can't read data from socket %d", errno);
            if (sndrSocket != -1) {
                close(sndrSocket);
            }
            vTaskDelay(pdMS_TO_TICKS(SEND_RETRY_INTERVAL_MS));
            continue;
        } else {
            ESP_LOGI(TAG, "READ : %d : '%s'", err, buf);
        }

        if (sndrSocket != -1) {
            close(sndrSocket);
        }
        try = SEND_RETRIES + 1;
    }
    // check if the retry counter was exhausted because of the consequent errors
    if (try == SEND_RETRIES) {
        ESP_LOGE(TAG, "FAILED TO SEND DATA, REBOOT");
        esp_restart();
    } else {
        gpio_set_level(BUILTIN_LED, 0);
        vTaskDelay(pdMS_TO_TICKS(250));
        gpio_set_level(BUILTIN_LED, 1);
    }
}

void app_main() {
    ESP_LOGI(TAG, "init tcp/ip stack");
    tcpip_adapter_init();
    ESP_LOGI(TAG, "create default loop");
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    // init watchdog
    ESP_ERROR_CHECK(esp_task_wdt_init());
    ESP_LOGI(TAG, "init wifi");
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "create custom event handler");
    wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &unified_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &unified_event_handler, NULL));
    bool connected = false;
    for (int i = 0; i < WIFI_CONNECT_RETRIES; i++) {
        connected = connect_wifi();
        if (connected) {
            break;
        }
        ESP_LOGE(TAG, "can't connect to AP " CONFIG_ESP_WIFI_SSID);
        vTaskDelay(pdMS_TO_TICKS(WIFI_CONNECT_RETRY_DELAY_MS));
    }
    if (!connected) {
        esp_restart();
    }
    gpio_config_t conf = {
            .pin_bit_mask = (1 << BUILTIN_LED),
            .mode = GPIO_MODE_DEF_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_ENABLE,
            .intr_type = GPIO_INTR_DISABLE
    };
    ESP_ERROR_CHECK(gpio_config(&conf));
    TimerHandle_t timerChannel = xTimerCreate("ChannelSwitch",
                                              (pdMS_TO_TICKS(REPORT_INTERVAL_MS)),
                                              pdTRUE,
                                              (void *) 1,
                                              report_temp_humidity
    );
    assert(timerChannel);
    assert(xTimerStart(timerChannel, 0) == pdPASS);
    gpio_set_level(BUILTIN_LED, 1);
}

