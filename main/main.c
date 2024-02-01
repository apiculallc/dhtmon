#include <sys/cdefs.h>
#include <stdio.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <dht.h>
#include "lwip/err.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "event_groups.h"
#include <stdbool.h>
#include <mqtt_client.h>
#include <nvs_flash.h>
#include <esp_timer.h>
#include <esp_task_wdt.h>
#include "driver/gpio.h"
#include "accesspoint.h"
#include "flash_config.h"

#define WAIT_CONNECT (5000)
#define TAG "DHT_MONITOR"
#define SEND_RETRIES (5)
#define SEND_RETRY_INTERVAL_MS (5000)
#define REPORT_INTERVAL_MS (SEND_RETRIES * SEND_RETRY_INTERVAL_MS + 100)
#define WIFI_CONNECTED_BIT     BIT0
#define MAX_SENSOR_READ_TRIES 3

struct ConfigData app_config;

static const dht_sensor_type_t sensor_type = DHT_TYPE_AM2301;
//static const dht_sensor_type_t sensor_type = DHT_TYPE_DHT11;
static const gpio_num_t dht_sensor_arr[] = {GPIO_NUM_4, GPIO_NUM_5};
static const gpio_num_t dht_btn_arr[] = {GPIO_NUM_5, GPIO_NUM_4};
static gpio_num_t dht_sensor, dht_btn;
const char *endpoint;
static EventGroupHandle_t wifi_event_group;
struct esp_mqtt_client *mqtt_client;

static char *temp; // temperature
static char *humid; // humidity

static void unified_event_handler(void *arg, esp_event_base_t event_base,
                                  int32_t event_id, void *event_data) {
    if (event_base == WIFI_EVENT) {
        ESP_LOGD(TAG, "WIFI_EVENT: %d", event_id);
        if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
            // restart if the connection is dropped
            ESP_LOGE(TAG, "can't connect to wifi");
            esp_restart();
        }
    } else if (event_base == IP_EVENT) {
        ESP_LOGD(TAG, "IP_EVENT: %d", event_id);
        if (event_id == IP_EVENT_STA_GOT_IP) {
            ip_event_got_ip_t *event = (ip_event_got_ip_t *) event_data;
            ESP_LOGI(TAG, "got ip: %s", ip4addr_ntoa((const ip4_addr_t *) &event->ip_info.ip));
            xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
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

    ESP_LOGI(TAG, "entering station mode...");

    if (strlen((char *) app_config.wifi_config.sta.password)) {
        app_config.wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    }

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &app_config.wifi_config));
    ESP_ERROR_CHECK(esp_wifi_connect());

    ESP_LOGI(TAG, "waiting for wifi connection");

    /* Waiting until either the connection is established (WIFI_CONNECTED_BIT) or connection failed for the maximum
     * number of re-tries (WIFI_FAIL_BIT). The bits are set by event_handler() (see above) */
    EventBits_t bits = xEventGroupWaitBits(wifi_event_group,
                                           WIFI_CONNECTED_BIT,
                                           pdTRUE,
                                           pdFALSE,
                                           pdMS_TO_TICKS(WAIT_CONNECT));
    /* xEventGroupWaitBits() returns the bits before the call returned, hence we can test which event actually
     * happened. */
    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "connected to ap SSID:%s", app_config.wifi_config.sta.ssid);
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

    int8_t sensor_read_attempts = 0;
    if (dht_read_data(sensor_type, dht_sensor, &humidity, &temperature) == ESP_OK) {
        ESP_LOGI(TAG, "Humidity: %d%% Temp: %dC", humidity / 10, temperature / 10);
    } else {
        ESP_LOGE(TAG, "Could not read data from sensor");
        sensor_read_attempts++;
        if (sensor_read_attempts > MAX_SENSOR_READ_TRIES) {
            esp_restart();
        }
        return;
    }

    int try = 0;
    for (; try < SEND_RETRIES; try++) {
        ESP_LOGI(TAG, "SENDING DATA ATTEMPT %d", try);
#define bufSz 10
        static char buf[bufSz];
        int sz = snprintf(buf, bufSz, "%d.%d", temperature / 10, temperature % 10);
        ESP_LOGI(TAG, "sending temp: '%s'", buf);
        int ret = esp_mqtt_client_publish(mqtt_client, temp, buf,
                                          sz, 1, 0);
        if (ret < 0) {
            ESP_LOGE(TAG, "can't send temperature to %s => %d", temp, ret);
            vTaskDelay(SEND_RETRY_INTERVAL_MS);
            continue;
        }
        sz = sprintf(buf, "%d.%d", humidity / 10, humidity % 10);
        ESP_LOGI(TAG, "sending humidity: '%s'", buf);
        ret = esp_mqtt_client_publish(mqtt_client, humid, buf,
                                      sz, 1, 0);
        if (ret < 0) {
            ESP_LOGE(TAG, "can't send temperature to %s => %d", temp, ret);
            vTaskDelay(SEND_RETRY_INTERVAL_MS);
            continue;
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
    esp_task_wdt_reset();
}

void read_dht_mainloop() {
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
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &unified_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &unified_event_handler, NULL));
    if (!connect_wifi()) {
        esp_restart();
    }
    esp_mqtt_client_config_t mqtt_cfg = {
            .uri = app_config.network.mqtt_uri,
    };
    mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    ESP_ERROR_CHECK(esp_mqtt_client_start(mqtt_client));

    ESP_LOGI(TAG, "init WDT");
    ESP_ERROR_CHECK(esp_task_wdt_init());
    TimerHandle_t timerChannel = xTimerCreate("DHT_READ",
                                              (pdMS_TO_TICKS(REPORT_INTERVAL_MS)),
                                              pdTRUE,
                                              (void *) 1,
                                              report_temp_humidity
    );
    assert(timerChannel);
    assert(xTimerStart(timerChannel, 0) == pdPASS);

}

_Noreturn void display_error() {
    for (;;) {
        gpio_set_level(BUILTIN_LED, 0);
        sleep(1);
        gpio_set_level(BUILTIN_LED, 1);
        sleep(1);
    }
}

static xQueueHandle gpio_evt_queue = NULL;
static int64_t latest_btn_event;
const int short_press = 1000;
static bool armed = false;
static int64_t evt;

/**
 * The GPIO event handler that will trigger and send the event to the queue once falling edge is detected and current
 * GPIO status is "low"
 */
void btnPressHandler(void *arg) {
    if (gpio_get_level(dht_btn) == 0) {
        evt = esp_timer_get_time();
        xQueueSendFromISR(gpio_evt_queue, &evt, NULL);
    }
}

/*
 * The task that is being triggered "async" to confirm that UUID was set.
 */
static void uuid_set_blinker(void *arg) {
    for (int i = 0; i <= 5; i++) {
        gpio_set_level(BUILTIN_LED, 0);
        sleep(1);
        gpio_set_level(BUILTIN_LED, 1);
        sleep(1);
    }
    armed = false;
}

/**
 * The event handler for GPIO.
 */
static void gpio_event_task(void *arg) {
    int64_t evt_time;
    for (;;) {
        if (xQueueReceive(gpio_evt_queue, &evt_time, pdMS_TO_TICKS(short_press * 2))) {
            ESP_LOGD(TAG, "button pressed");
            if ((evt_time - latest_btn_event) / 1000 > short_press) {
                latest_btn_event = evt_time;
                if (!armed) {
                    armed = true;
                    continue;
                }
                xTaskCreate(uuid_set_blinker, "uuid blinker set", 1024, NULL, 10, NULL);
            }
        }
    }
}


void app_main() {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        display_error();
    }
    ESP_ERROR_CHECK(ret);
    // Reads the various configuration parameters into the application config from NVS.
    ESP_LOGI(TAG, "Reading configuration");
    ret = read_app_config(&app_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Can't read config: %s", esp_err_to_name(ret));
        display_error();
    }

    temp = malloc(sizeof(CONFIG_APP_MQTT_TOPIC"/temp") + strlen(app_config.location) - 1);
    humid = malloc(sizeof(CONFIG_APP_MQTT_TOPIC"/humid") + strlen(app_config.location) - 1);

    sprintf(temp, CONFIG_APP_MQTT_TOPIC"/temp", app_config.location);
    sprintf(humid, CONFIG_APP_MQTT_TOPIC"/humid", app_config.location);

    ESP_LOGI(TAG, "temp: '%s';\nhumid: '%s'", temp, humid);

    gpio_config_t conf = {
            .pin_bit_mask = (1 << BUILTIN_LED),
            .mode = GPIO_MODE_DEF_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_ENABLE,
            .intr_type = GPIO_INTR_DISABLE
    };
    ESP_ERROR_CHECK(gpio_config(&conf));
    gpio_set_level(BUILTIN_LED, 1);
    bool gpioFound = false;
    for (int sensorIdx = 0; !gpioFound && sensorIdx < sizeof(dht_sensor_arr) / sizeof(gpio_num_t); sensorIdx++) {
        dht_sensor = dht_sensor_arr[sensorIdx];
        dht_btn = dht_btn_arr[sensorIdx];
        ESP_LOGI(TAG, "reading GPIO %d", dht_sensor);
        ESP_ERROR_CHECK(gpio_set_pull_mode(dht_sensor, GPIO_PULLUP_ONLY))
        int16_t humidity, temperature;
        for (int try = 0; try < 3 && !gpioFound; try++) {
            if (dht_read_data(sensor_type, dht_sensor, &humidity, &temperature) == ESP_OK) {
                ESP_LOGI(TAG, "found DHT GPIO %d", dht_sensor);
                ESP_LOGI(TAG, "found BTN GPIO %d", dht_btn);
                gpioFound = true;
                ESP_LOGI(TAG, "Humidity: %d%% Temp: %dC", humidity / 10, temperature / 10);
                gpio_set_level(BUILTIN_LED, 0);
                sleep(1);
                gpio_set_level(BUILTIN_LED, 1);
            } else {
                ESP_LOGW(TAG, "can't read GPIO %d, sleeping", dht_sensor);
                sleep(2);
            }
        }
    }
    // if we can't read data - no point of connecting to a wifi.

    if (!gpioFound) {
        ESP_LOGE(TAG, "can't read DHT");
        display_error();
    }

    gpio_set_pull_mode(dht_btn, GPIO_PULLUP_ONLY);
    if (gpio_get_level(dht_btn) == 0) {
        if (startAp(&app_config) != 0) {
            display_error();
        }
        gpio_set_level(BUILTIN_LED, 0);
    } else {
        read_dht_mainloop();
    }

    gpio_set_intr_type(dht_sensor, GPIO_INTR_LOW_LEVEL);
    // create a queue to process GPIO tasks
    gpio_evt_queue = xQueueCreate(5, sizeof(uint32_t));
    // poll events from the gpio_evt_queue and run processing outside of the IRQ event loop.
    xTaskCreate(gpio_event_task, "btn event handler", 2048, NULL, 10, NULL);

    // Register IRQ for the control button press events
    ESP_ERROR_CHECK(gpio_install_isr_service(0));

    gpio_isr_handler_add(dht_sensor, &btnPressHandler, NULL);
}