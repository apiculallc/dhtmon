#include <sys/param.h>
#include <esp_timer.h>
#include "string.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_wifi.h"
#include "esp_system.h"
#include "esp_event.h"
#include "esp_event_loop.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "ctype.h"

#include "config.h"
#include "flash_config.h"
#include "accesspoint.h"

#define SOFT_AP_PASSWORD "test1234567890"

#define SOFT_AP_IP_ADDRESS_1 192
#define SOFT_AP_IP_ADDRESS_2 168
#define SOFT_AP_IP_ADDRESS_3 5
#define SOFT_AP_IP_ADDRESS_4 18

#define SOFT_AP_GW_ADDRESS_1 192
#define SOFT_AP_GW_ADDRESS_2 168
#define SOFT_AP_GW_ADDRESS_3 5
#define SOFT_AP_GW_ADDRESS_4 20

#define SOFT_AP_NM_ADDRESS_1 255
#define SOFT_AP_NM_ADDRESS_2 255
#define SOFT_AP_NM_ADDRESS_3 255
#define SOFT_AP_NM_ADDRESS_4 0

#define SERVER_PORT 80

#define TAG "ESP_AP"

static httpd_handle_t httpServerInstance = NULL;

static char http_data_file[3000];
static size_t http_data_file_sz = sizeof http_data_file;

static char uuid_string[NAME_LEN];

void urldecode2(char *dst, char *src) {
    char a, b;
    while (*src) {
        if ((*src == '%') &&
            ((a = src[1]) && (b = src[2])) &&
            (isxdigit(a) && isxdigit(b))) {
            if (a >= 'a')
                a -= 'a' - 'A';
            if (a >= 'A')
                a -= ('A' - 10);
            else
                a -= '0';
            if (b >= 'a')
                b -= 'a' - 'A';
            if (b >= 'A')
                b -= ('A' - 10);
            else
                b -= '0';
            *dst++ = 16 * a + b;
            src += 3;
        } else if (*src == '+') {
            *dst++ = ' ';
            src++;
        } else {
            *dst++ = *src++;
        }
    }
    *dst++ = '\0';
}

esp_err_t indexHandler(httpd_req_t *httpRequest) {
    ESP_LOGI("HANDLER", "This is the handler for the <%s> URI", httpRequest->uri);
    ESP_ERROR_CHECK(read_web_file("index.html", http_data_file, &http_data_file_sz));
    // copy UUID
    strncpy(&http_data_file[1919], uuid_string, NAME_LEN - 1);
    strncpy(&http_data_file[1957], uuid_string, NAME_LEN - 1);
    httpd_resp_set_status(httpRequest, HTTPD_200);
    httpd_resp_set_type(httpRequest, HTTPD_TYPE_TEXT);
    httpd_resp_send(httpRequest, http_data_file, http_data_file_sz);
    return ESP_OK;
}

esp_err_t updateHandler(httpd_req_t *httpRequest) {
    ESP_LOGI("HANDLER", "This is the handler for the <%s> URI", httpRequest->uri);
    char post_form[1000];
    size_t recv_size = MIN(httpRequest->content_len, sizeof(post_form));
    int ret = httpd_req_recv(httpRequest, post_form, recv_size);
    switch (ret) {
        case 0:
            ESP_LOGE(TAG, "socket closed");
            return HTTPD_SOCK_ERR_FAIL;
        case HTTPD_SOCK_ERR_TIMEOUT:
        case HTTPD_SOCK_ERR_FAIL:
        case HTTPD_SOCK_ERR_INVALID:
            ESP_LOGE(TAG, "HTTP ERROR: %d", ret);
            return ret;
        default:
            post_form[ret] = 0;
            ESP_LOGI(TAG, "Response data: %s", post_form);
    }
    char *ssid = 0, *password = 0, *confirm = 0, *location = 0;
    int name_start = 0, value_start = 0;
    for (int i = 0; i < recv_size; i++) {
        switch (post_form[i]) {
            case '&':
                post_form[i] = 0; // make sure string comparison will work;
                if (strcmp("ssid", &post_form[name_start]) == 0) {
                    ssid = &post_form[value_start];
                } else if (strcmp("password", &post_form[name_start]) == 0) {
                    password = &post_form[value_start];
                } else if (strcmp("password_rep", &post_form[name_start]) == 0) {
                    confirm = &post_form[value_start];
                } else if (strcmp("location", &post_form[name_start]) == 0) {
                    location = &post_form[value_start];
                }
                name_start = i + 1;
                value_start = -1;
                break;
            case '=':;
                post_form[i] = 0;
                value_start = i + 1;
                break;
            default:
                continue;
        };
    }
    post_form[recv_size + 1] = 0;
    if (name_start < recv_size && value_start > 0) {
        if (strcmp("ssid", &post_form[name_start]) == 0) {
            ssid = &post_form[value_start];
        } else if (strcmp("password", &post_form[name_start]) == 0) {
            password = &post_form[value_start];
        } else if (strcmp("password_rep", &post_form[name_start]) == 0) {
            confirm = &post_form[value_start];
        } else if (strcmp("location", &post_form[name_start]) == 0) {
            location = &post_form[value_start];
        }
    }
    urldecode2(password, password);
    urldecode2(location, location);
    urldecode2(ssid, ssid);
    ESP_LOGI(TAG, "AP: %s, PWD: %s, CONF: %s", ssid, password, confirm);
    ESP_ERROR_CHECK(update_app_config(ssid, password, location));
    ESP_ERROR_CHECK(read_web_file("done.html", http_data_file, &http_data_file_sz));
    httpd_resp_send(httpRequest, http_data_file, http_data_file_sz);
    esp_restart();
}

static httpd_uri_t indexURI = {
        .uri       = "/",
        .method    = HTTP_GET,
        .handler   = indexHandler,
        .user_ctx  = NULL,
};

static httpd_uri_t updateURI = {
        .uri       = "/update",
        .method    = HTTP_POST,
        .handler   = updateHandler,
        .user_ctx  = NULL,
};

void startHttpServer(void) {
    httpd_config_t httpServerConfiguration = HTTPD_DEFAULT_CONFIG();
    httpServerConfiguration.server_port = SERVER_PORT;
    if (httpd_start(&httpServerInstance, &httpServerConfiguration) == ESP_OK) {
        ESP_ERROR_CHECK(httpd_register_uri_handler(httpServerInstance, &indexURI));
        ESP_ERROR_CHECK(httpd_register_uri_handler(httpServerInstance, &updateURI));
    }
}

void stopHttpServer(void) {
    if (httpServerInstance != NULL) {
        ESP_ERROR_CHECK(httpd_stop(httpServerInstance));
    }
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data) {
    if (event_id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t *event = (wifi_event_ap_staconnected_t *) event_data;
        ESP_LOGI(TAG, "station "MACSTR" join, AID=%d",
                 MAC2STR(event->mac), event->aid);
        startHttpServer();
    } else if (event_id == WIFI_EVENT_AP_STADISCONNECTED) {
        wifi_event_ap_stadisconnected_t *event = (wifi_event_ap_stadisconnected_t *) event_data;
        ESP_LOGI(TAG, "station "MACSTR" leave, AID=%d",
                 MAC2STR(event->mac), event->aid);
        stopHttpServer();
    }
}

int startAp(struct ConfigData *app_cfg) {
    // ===================================================
    tcpip_adapter_init();
    ESP_ERROR_CHECK(tcpip_adapter_dhcps_stop(TCPIP_ADAPTER_IF_AP));
    tcpip_adapter_ip_info_t ipAddressInfo;
    memset(&ipAddressInfo, 0, sizeof(ipAddressInfo));
    IP4_ADDR(
            &ipAddressInfo.ip,
            SOFT_AP_IP_ADDRESS_1,
            SOFT_AP_IP_ADDRESS_2,
            SOFT_AP_IP_ADDRESS_3,
            SOFT_AP_IP_ADDRESS_4);
    IP4_ADDR(
            &ipAddressInfo.gw,
            SOFT_AP_GW_ADDRESS_1,
            SOFT_AP_GW_ADDRESS_2,
            SOFT_AP_GW_ADDRESS_3,
            SOFT_AP_GW_ADDRESS_4);
    IP4_ADDR(
            &ipAddressInfo.netmask,
            SOFT_AP_NM_ADDRESS_1,
            SOFT_AP_NM_ADDRESS_2,
            SOFT_AP_NM_ADDRESS_3,
            SOFT_AP_NM_ADDRESS_4);
    ESP_ERROR_CHECK(tcpip_adapter_set_ip_info(TCPIP_ADAPTER_IF_AP, &ipAddressInfo));
    ESP_ERROR_CHECK(tcpip_adapter_dhcps_start(TCPIP_ADAPTER_IF_AP));

    ESP_ERROR_CHECK(esp_event_loop_create_default());

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));

    wifi_config_t wifi_config = {
            .ap = {
                    .password = SOFT_AP_PASSWORD,
                    .max_connection = 1,
                    .authmode = WIFI_AUTH_WPA_WPA2_PSK
            },
    };

    strncpy((char *) wifi_config.ap.ssid, app_cfg->location, sizeof wifi_config.ap.ssid);

    ESP_LOGI(TAG, "B64 AP: %s", wifi_config.ap.ssid);

    wifi_config.ap.ssid_len = strlen((const char *) wifi_config.ap.ssid);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "wifi_init_softap finished. SSID:%s password:%s",
             wifi_config.ap.ssid, SOFT_AP_PASSWORD);

    return 0;
}