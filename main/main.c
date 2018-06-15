#include <stdint.h>
#include <string.h>
#include <stdbool.h>
#include <stdio.h>
#include <time.h>
#include <sys/time.h>
#include "nvs_flash.h"

#include "esp_bt.h"
#include "esp_gap_ble_api.h"
#include "esp_gattc_api.h"
#include "esp_gatt_defs.h"
#include "esp_bt_main.h"
#include "esp_gatt_common_api.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event_loop.h"
#include "esp_attr.h"
#include "esp_sleep.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "lwip/err.h"
#include "apps/sntp/sntp.h"

#include <esp_mqtt.h>
// #include "mqtt_client.h"

#include "cJSON.h"

#define BLE_TAG "BLE"
#define WIFI_TAG "WiFi"
#define MQTT_TAG "MQTT"
// #define EXAMPLE_WIFI_SSID CONFIG_WIFI_SSID
// #define EXAMPLE_WIFI_PASS CONFIG_WIFI_PASSWORD
#define EXAMPLE_WIFI_SSID "OlVeHotspot"
#define EXAMPLE_WIFI_PASS "1ViBDzr1ZK"
// #define EXAMPLE_WIFI_SSID "wifi-free"
// #define EXAMPLE_WIFI_PASS ""

// IBM MQTT credentials: token rgpCKDpsJA76OgV7zI
// #define MQTT_HOST "mqtt://p28mg8.messaging.internetofthings.ibmcloud.com"
// #define MQTT_USER "use-token-auth"
// #define MQTT_PASS "rgpCKDpsJA76OgV7zI"
// #define MQTT_PORT 1883
// #define MQTT_PORT_CHAR "1883"
// #define MQTT_CLIENT_ID "d:p28mg8:ESP32:office"
// #define MQTT_TOPIC_IBM "iot-2/evt/reading/fmt/json"

// ubidots MQTT credentials:
#define MQTT_HOST "mqtt://things.ubidots.com"
#define MQTT_USER "A1E-Cg5BcJyCyq9AImrvvG08MlYmxydLD2"
#define MQTT_PASS " "
#define MQTT_PORT 1883
#define MQTT_PORT_CHAR "1883"
#define MQTT_CLIENT_ID "ESP32:Civi:Ostrov"

// static char MQTT_TOPIC_UBIDOTS[64];

/* FreeRTOS event group to signal when we are connected & ready to make a request */
static EventGroupHandle_t wifi_event_group;
/* The event group allows multiple bits for each event,
   but we only care about one event - are we connected
   to the AP with an IP? */
const int WIFI_CONNECTED_BIT = BIT0;
const int MQTT_CONNECTED_BIT = BIT1;
/* Variable holding number of times ESP32 restarted since first boot.
 * It is placed into RTC memory using RTC_DATA_ATTR and
 * maintains its value when ESP32 wakes from deep sleep.
 */
// RTC_DATA_ATTR static int boot_count = 0;

// esp_mqtt_client_handle_t mqttclient;

static const uint8_t bit16_UUID_Eddystone[2] = {0xaa, 0xfe};
static const uint8_t service_data_EddystoneUID[3] = {0xaa, 0xfe, 0x00};
static const uint8_t name_space_VIDA[10] = {0xec, 0x0c, 0xb4, 0xca, 0x9f, 0xa6, 0x84, 0xd6, 0xab, 0xbf};
static const uint8_t duration = 4;

/* declare static functions */
static void esp_gap_cb(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param);
static void initialize_ble(void);
static void obtain_time(void);
static void initialize_sntp(void);
static void initialize_wifi(void);
static esp_err_t event_handler(void *ctx, system_event_t *event);


static esp_ble_scan_params_t ble_scan_params = {
    .scan_type              = BLE_SCAN_TYPE_ACTIVE,
    .own_addr_type          = BLE_ADDR_TYPE_PUBLIC,
    .scan_filter_policy     = BLE_SCAN_FILTER_ALLOW_ALL,
    .scan_interval          = 0x0050, // 50 ms; N*0.625ms
    .scan_window            = 0x0050  // 50 ms
};

typedef struct VIDABeacon {
  uint8_t bda[6];
  char address[12];
  char name[29];
  uint16_t groupID;
  uint8_t individualID;
  uint8_t demographic;
  uint16_t reserved;
  bool male;
  bool school_group;
  bool VIDArd;
  uint8_t age_group;
  int sumRSSI;
  uint8_t times_found;
  int averageRSSI;
} VIDABeacon;


// MQTT callback functions for esp_mqtt.h
static void status_callback(esp_mqtt_status_t status) {
  switch (status) {
    case ESP_MQTT_STATUS_CONNECTED:
      ESP_LOGI(MQTT_TAG, "Connected to broker.");
      xEventGroupSetBits(wifi_event_group, MQTT_CONNECTED_BIT);
      break;
    case ESP_MQTT_STATUS_DISCONNECTED:
      // reconnect
      ESP_LOGI(MQTT_TAG, "Disconnected from broker. Reconnecting.");
      esp_mqtt_start(MQTT_HOST, MQTT_PORT_CHAR, MQTT_CLIENT_ID, MQTT_USER, MQTT_PASS);
      xEventGroupClearBits(wifi_event_group, MQTT_CONNECTED_BIT);
      break;
  }
}

static void message_callback(const char *topic, uint8_t *payload, size_t len) {
  ESP_LOGI(MQTT_TAG, "incoming: %s => %s (%d)", topic, payload, (int)len);
}

// static esp_err_t mqtt_event_handler(esp_mqtt_event_handle_t event)
// {
//     mqttclient = event->client;
//     // int msg_id;
//     // your_context_t *context = event->context;
//     switch (event->event_id) {
//         case MQTT_EVENT_CONNECTED:
//             ESP_LOGI(MQTT_TAG, "MQTT_EVENT_CONNECTED");
//             cJSON *root = NULL;
//             char *out = NULL;
//             root = cJSON_CreateObject();
//             cJSON_AddItemToObject(root, "rssi", cJSON_CreateNumber(-81));
//             out = cJSON_Print(root);
//             esp_mqtt_client_publish(mqttclient, "/v1.6/devices/ESP32:office", out, strlen(out), 2, false);
//             // esp_mqtt_client_publish(mqttclient, "iot-2/evt/testing/fmt/json", out, strlen(out), 2, false);
//             // msg_id = esp_mqtt_client_subscribe(client, "/topic/qos0", 0);
//             // ESP_LOGI(MQTT_TAG, "sent subscribe successful, msg_id=%d", msg_id);
//             //
//             // msg_id = esp_mqtt_client_subscribe(client, "/topic/qos1", 1);
//             // ESP_LOGI(MQTT_TAG, "sent subscribe successful, msg_id=%d", msg_id);
//             //
//             // msg_id = esp_mqtt_client_unsubscribe(client, "/topic/qos1");
//             // ESP_LOGI(MQTT_TAG, "sent unsubscribe successful, msg_id=%d", msg_id);
//             break;
//         case MQTT_EVENT_DISCONNECTED:
//             ESP_LOGI(MQTT_TAG, "MQTT_EVENT_DISCONNECTED");
//             break;
//
//         case MQTT_EVENT_SUBSCRIBED:
//             ESP_LOGI(MQTT_TAG, "MQTT_EVENT_SUBSCRIBED, msg_id=%d", event->msg_id);
//             // msg_id = esp_mqtt_client_publish(client, "/topic/qos0", "data", 0, 0, 0);
//             // ESP_LOGI(MQTT_TAG, "sent publish successful, msg_id=%d", msg_id);
//             break;
//         case MQTT_EVENT_UNSUBSCRIBED:
//             ESP_LOGI(MQTT_TAG, "MQTT_EVENT_UNSUBSCRIBED, msg_id=%d", event->msg_id);
//             break;
//         case MQTT_EVENT_PUBLISHED:
//             ESP_LOGI(MQTT_TAG, "MQTT_EVENT_PUBLISHED, msg_id=%d", event->msg_id);
//             break;
//         case MQTT_EVENT_DATA:
//             ESP_LOGI(MQTT_TAG, "MQTT_EVENT_DATA");
//             printf("TOPIC=%.*s\r\n", event->topic_len, event->topic);
//             printf("DATA=%.*s\r\n", event->data_len, event->data);
//             break;
//         case MQTT_EVENT_ERROR:
//             ESP_LOGI(MQTT_TAG, "MQTT_EVENT_ERROR");
//             break;
//     }
//     return ESP_OK;
// }

static void esp_gap_cb(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param)
{
    uint8_t *adv_name = NULL;
    uint8_t adv_name_len = 0;
    uint8_t *bit16_UUID = NULL;
    uint8_t bit16_UUID_len = 0;
    uint8_t *service_data = NULL;
    uint8_t service_data_len = 0;
    uint8_t *name_space = NULL;
    uint8_t *instance = NULL;
    time_t now;
    struct tm timeinfo;

    static uint64_t start = 0;
    static VIDABeacon beacons[50];
    static uint8_t beacons_counter = 0;
    uint8_t dp_counter = 0;


    switch (event) {
    case ESP_GAP_BLE_SCAN_PARAM_SET_COMPLETE_EVT:
        time(&now);
        localtime_r(&now, &timeinfo);
        while (timeinfo.tm_sec % 5 != 0) {
          vTaskDelay(1000 / portTICK_PERIOD_MS);
          time(&now);
          localtime_r(&now, &timeinfo);
        }
        //the unit of the duration is second
        esp_ble_gap_start_scanning(duration);
        break;
    case ESP_GAP_BLE_SCAN_START_COMPLETE_EVT:
        //scan start complete event to indicate scan start successfully or failed
        if (param->scan_start_cmpl.status != ESP_BT_STATUS_SUCCESS) {
            ESP_LOGE(BLE_TAG, "scan start failed, error status = %x", param->scan_start_cmpl.status);
            break;
        }
        dp_counter = 0;
        time(&now);
        localtime_r(&now, &timeinfo);
        start = (uint64_t) now;
        start *= 1000;
        ESP_LOGI(BLE_TAG, "scan start success at time: %d:%d:%d", timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
        if (timeinfo.tm_sec == 0) {  // send alive confirmation every minute
          char alive[64] = "/v1.6/devices/";
          strcat(alive, MQTT_CLIENT_ID);
          strcat(alive, "/alive");
          esp_mqtt_publish(alive, (uint8_t *) "{\"value\":true}", 14, 1, false);
        }


        break;
    case ESP_GAP_BLE_SCAN_RESULT_EVT: {
        esp_ble_gap_cb_param_t *scan_result = (esp_ble_gap_cb_param_t *)param;
        switch (scan_result->scan_rst.search_evt) {
        case ESP_GAP_SEARCH_INQ_RES_EVT:{
            bit16_UUID = esp_ble_resolve_adv_data(scan_result->scan_rst.ble_adv,
                                            ESP_BLE_AD_TYPE_16SRV_CMPL, &bit16_UUID_len);
            if (bit16_UUID_len == 0 || memcmp(bit16_UUID, bit16_UUID_Eddystone, bit16_UUID_len) != 0) {
              // ESP_LOGI(BLE_TAG, "Not EddyStone!");
              break;  // Ignore not eddystone beacons
            }
            service_data = esp_ble_resolve_adv_data(scan_result->scan_rst.ble_adv,
                                             ESP_BLE_AD_TYPE_SERVICE_DATA, &service_data_len);
            if (service_data_len == 0 || memcmp(service_data, service_data_EddystoneUID, 3) != 0) {
              // ESP_LOGI(BLE_TAG, "Not EddyStoneUID!");
              break;  // ignore eddystone beacons other than UID
            }
            name_space = service_data + 4;
            if (memcmp(name_space, name_space_VIDA, 10) != 0) {
              // ESP_LOGI(BLE_TAG, "Not VIDA! EddyStoneUID!");
              break;  // Ignore EddyStoneUID beacons with different name_space
            }
            instance = name_space + 10;

            bool found = false;
            size_t i;
            for (i = 0; i < beacons_counter; i++) {
              if (memcmp(scan_result->scan_rst.bda, beacons[i].bda, 6) == 0) {
                found = true;
                break;
              }
            }
            if (found == true) {
              ESP_LOGI(BLE_TAG, "Found beacon updated.");
              beacons[i].sumRSSI += scan_result->scan_rst.rssi;
              beacons[i].times_found++;
            } else {
              ESP_LOGI(BLE_TAG,"New Beacon discovered.");
              memcpy(beacons[beacons_counter].bda, scan_result->scan_rst.bda, 6);
              sprintf(beacons[beacons_counter].address, "%x%x%x%x%x%x", beacons[beacons_counter].bda[0],
                           beacons[beacons_counter].bda[1], beacons[beacons_counter].bda[2],
                          beacons[beacons_counter].bda[3], beacons[beacons_counter].bda[4],
                         beacons[beacons_counter].bda[5]);
              adv_name = esp_ble_resolve_adv_data(scan_result->scan_rst.ble_adv,
                                                  ESP_BLE_AD_TYPE_NAME_CMPL, &adv_name_len);
              strncpy(beacons[beacons_counter].name, (char *)adv_name, adv_name_len);
              beacons[beacons_counter].sumRSSI = scan_result->scan_rst.rssi;
              beacons[beacons_counter].times_found = 1;
              beacons[beacons_counter].groupID = ((uint16_t) *instance << 8) | *(instance+1); // first two bytes of instance
              beacons[beacons_counter].individualID = *(instance+2);  // third byte of instance
              beacons[beacons_counter].demographic = *(instance+3);  // fourth byte of instance
              beacons[beacons_counter].reserved = ((uint16_t) *(instance+4) << 8) | *(instance+5); // last two bytes of instance
              beacons[beacons_counter].male = beacons[beacons_counter].demographic & 0x01; // first bit of demographic is 1; demographic && 0000 0001
              beacons[beacons_counter].school_group = (beacons[beacons_counter].demographic & 0x02) >> 1;  // second bit of demographic is 1
              beacons[beacons_counter].VIDArd = (beacons[beacons_counter].demographic & 0x04) >> 2;  // third bit is 1
              beacons[beacons_counter].age_group = (beacons[beacons_counter].demographic & 0x30) >> 4;  // fifth and sixth bits shifted to right 0011 0000 -> 0000 0011

              beacons_counter++;
            }
            break;
          }
        case ESP_GAP_SEARCH_INQ_CMPL_EVT: {
            cJSON *root = cJSON_CreateObject();

            for (size_t i = 0; i < beacons_counter; i++) {
              if (beacons[i].times_found > 0) {
                beacons[i].averageRSSI = beacons[i].sumRSSI / beacons[i].times_found;
                // ESP_LOGI(BLE_TAG, "Found %s %d times with %d average RSSI and with ID %d belonging to group %d and address %s",
                //               beacons[i].name, beacons[i].times_found, beacons[i].averageRSSI, beacons[i].individualID, beacons[i].groupID, beacons[i].address);
                // ESP_LOGI(BLE_TAG, "%s is %s, %s to a school group, %s VIDArd and belongs to age group number %d\n",
                //               beacons[i].name, beacons[i].male ? "male" : "female",
                //               beacons[i].school_group ? "belongs" : "doesn`t belong",
                //               beacons[i].VIDArd ? "solves" : "doesn`t solve",
                //               beacons[i].age_group);
                cJSON *value = NULL;
                value = cJSON_CreateObject();
                cJSON_AddNumberToObject(value, "value", beacons[i].averageRSSI);
                cJSON_AddNumberToObject(value, "timestamp", start);
                cJSON_AddItemToObject(root, beacons[i].address, value);

                beacons[i].times_found = 0;
                beacons[i].sumRSSI = 0;

                dp_counter++;
              }
            }

            ESP_LOGI(BLE_TAG, "Found %d VIDA! beacons", dp_counter);
            if (dp_counter > 0) {
              char *out = NULL;
              out = cJSON_Print(root);
              ESP_LOGI(MQTT_TAG, "Json length: %d\nJson: %s", strlen(out), out);
              char topic[64] = "/v1.6/devices/";
              strcat(topic, MQTT_CLIENT_ID);
              // esp_mqtt_client_publish(mqttclient, "/v1.6/devices/ESP32:office", out, strlen(out), 2, false);  // ubidots style topic
              esp_mqtt_publish(topic, (uint8_t *)out, strlen(out), 1, false);
            }

            time(&now);
            localtime_r(&now, &timeinfo);
            ESP_LOGI(BLE_TAG, "Search inquire complete at time: %d:%d:%d.", timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
            while (timeinfo.tm_sec % 5 != 0) {
              vTaskDelay(100 / portTICK_PERIOD_MS);
              time(&now);
              localtime_r(&now, &timeinfo);
            }
            esp_ble_gap_start_scanning(duration);
            break;
          }
        default:
            break;
        }
    }
    case ESP_GAP_BLE_SCAN_STOP_COMPLETE_EVT:
        // if (param->scan_stop_cmpl.status != ESP_BT_STATUS_SUCCESS){
        //     ESP_LOGE(BLE_TAG, "scan stop failed, error status = %x", param->scan_stop_cmpl.status);
        //     break;
        // }
        // ESP_LOGI(BLE_TAG, "stop scan successfully.");
        break;
    case ESP_GAP_BLE_UPDATE_CONN_PARAMS_EVT:
         ESP_LOGI(BLE_TAG, "update connection params status = %d, min_int = %d, max_int = %d,conn_int = %d,latency = %d, timeout = %d",
                  param->update_conn_params.status,
                  param->update_conn_params.min_int,
                  param->update_conn_params.max_int,
                  param->update_conn_params.conn_int,
                  param->update_conn_params.latency,
                  param->update_conn_params.timeout);
        break;
    default:
        break;
    }
}

static void initialize_wifi(void)
{
    tcpip_adapter_init();
    wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK( esp_event_loop_init(event_handler, NULL) );
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK( esp_wifi_init(&cfg) );
    ESP_ERROR_CHECK( esp_wifi_set_storage(WIFI_STORAGE_RAM) );
    wifi_config_t wifi_config = {
        .sta = {
            .ssid = EXAMPLE_WIFI_SSID,
            .password = EXAMPLE_WIFI_PASS,
        },
    };
    ESP_LOGI(WIFI_TAG, "Setting WiFi configuration SSID %s...", wifi_config.sta.ssid);
    ESP_ERROR_CHECK( esp_wifi_set_mode(WIFI_MODE_STA) );
    ESP_ERROR_CHECK( esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config) );
}

static void initialize_sntp(void)
{
    ESP_LOGI(WIFI_TAG, "Initializing SNTP");
    sntp_setoperatingmode(SNTP_OPMODE_POLL);
    sntp_setservername(0, "time.fi.muni.cz");
    sntp_init();
}

static void initialize_ble(void) {
  ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT));

  esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
  esp_err_t ret = esp_bt_controller_init(&bt_cfg);
  if (ret) {
      ESP_LOGE(BLE_TAG, "%s initialize controller failed: %x\n", __func__, ret);
      return;
  }

  ret = esp_bt_controller_enable(ESP_BT_MODE_BLE);
  if (ret) {
      ESP_LOGE(BLE_TAG, "%s enable controller failed: %x\n", __func__, ret);
      return;
  }

  ret = esp_bluedroid_init();
  if (ret) {
      ESP_LOGE(BLE_TAG, "%s init bluetooth failed: %x\n", __func__, ret);
      return;
  }

  ret = esp_bluedroid_enable();
  if (ret) {
      ESP_LOGE(BLE_TAG, "%s enable bluetooth failed: %x\n", __func__, ret);
      return;
  }
  //register the  callback function to the gap module
  ret = esp_ble_gap_register_callback(esp_gap_cb);
  if (ret){
      ESP_LOGE(BLE_TAG, "%s gap register failed, error code = %x\n", __func__, ret);
      return;
  }
}

static esp_err_t event_handler(void *ctx, system_event_t *event)
{
    switch(event->event_id) {
    case SYSTEM_EVENT_STA_START:
        esp_wifi_connect();
        break;
    case SYSTEM_EVENT_STA_GOT_IP:
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
        esp_mqtt_start(MQTT_HOST, MQTT_PORT_CHAR, MQTT_CLIENT_ID, MQTT_USER, MQTT_PASS);
        break;
    case SYSTEM_EVENT_STA_DISCONNECTED:
        /* This is a workaround as ESP32 WiFi libs don't currently
           auto-reassociate. */
        esp_mqtt_stop();
        esp_wifi_connect();
        xEventGroupClearBits(wifi_event_group, WIFI_CONNECTED_BIT);
        xEventGroupClearBits(wifi_event_group, MQTT_CONNECTED_BIT);
        break;
    default:
        break;
    }
    return ESP_OK;
}

static void obtain_time(void)
{
    initialize_sntp();

    // wait for time to be set
    time_t now = 0;
    struct tm timeinfo = { 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    int retry = 0;
    const int retry_count = 10;
    while(timeinfo.tm_year < (2016 - 1900) && ++retry < retry_count) {
        ESP_LOGI(WIFI_TAG, "Waiting for system time to be set... (%d/%d)", retry, retry_count);
        vTaskDelay(2000 / portTICK_PERIOD_MS);
        time(&now);
        localtime_r(&now, &timeinfo);
    }
}

// static esp_mqtt_client_handle_t mqtt_app_start(void)
// {
//     const esp_mqtt_client_config_t mqtt_cfg = {
//         .uri = MQTT_HOST,
//         .event_handle = mqtt_event_handler,
//         .port = MQTT_PORT,
//         .username = MQTT_USER,
//         .password = MQTT_PASS,
//         .client_id = MQTT_CLIENT_ID,
//         // .user_context = (void *)your_context
//     };
//
//     esp_mqtt_client_handle_t client = esp_mqtt_client_init(&mqtt_cfg);
//     esp_mqtt_client_start(client);
//     return client;
// }

void app_main()
{
    // Initialize NVS.
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK( ret );


    esp_mqtt_init(status_callback, message_callback, 256, 2000);
    initialize_wifi();
    ESP_ERROR_CHECK( esp_wifi_start() );
    xEventGroupWaitBits(wifi_event_group, WIFI_CONNECTED_BIT,
                        false, true, portMAX_DELAY);

    // mqttclient = mqtt_app_start();
    // esp_mqtt_start(MQTT_HOST, MQTT_PORT_CHAR, MQTT_CLIENT_ID, MQTT_USER, MQTT_PASS);
    xEventGroupWaitBits(wifi_event_group, MQTT_CONNECTED_BIT,
                        false, true, portMAX_DELAY);


    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);
    // Is time set? If not, tm_year will be (1970 - 1900).
    if (timeinfo.tm_year < (2018 - 1900)) {
      ESP_LOGI(WIFI_TAG, "Time is not set yet. Connecting to WiFi and getting time over NTP.");
      obtain_time();
      // update 'now' variable with current time
      time(&now);
    }
    char strftime_buf[64];
    // Set timezone to Central European Time
    setenv("TZ", "CET-1CEST,M3.5.0/2,M10.5.0/3", 1);
    tzset();
    localtime_r(&now, &timeinfo);
    strftime(strftime_buf, sizeof(strftime_buf), "%c", &timeinfo);
    ESP_LOGI(WIFI_TAG, "The current date/time in Brno is: %s.", strftime_buf);

    initialize_ble();

    // set ble scan parameters and start scanning afterwards
    esp_err_t scan_ret = esp_ble_gap_set_scan_params(&ble_scan_params);
    if (scan_ret){
        ESP_LOGE(BLE_TAG, "set scan params error, error code = %x", scan_ret);
    }  // scanning starts after parameters are set (see callback function)

    ESP_LOGI(BLE_TAG, "Main finished!");

    // const int deep_sleep_sec = 10;
    // ESP_LOGI(WIFI_TAG, "Entering deep sleep for %d seconds", deep_sleep_sec);
    // esp_deep_sleep(1000000LL * deep_sleep_sec);


}
