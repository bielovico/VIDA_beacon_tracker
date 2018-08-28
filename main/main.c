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
#include "esp_timer.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"

#include "lwip/err.h"
#include "lwip/ip4_addr.h"
#include "apps/sntp/sntp.h"

#include <esp_mqtt.h>

#include "cJSON.h"

#define APP_VERSION "0.9.2" // Safe publish definition

#define MAIN_TAG "MAIN"
#define BLE_TAG "BLE"
#define WIFI_TAG "WiFi"
#define MQTT_TAG "MQTT"
#define TIMER_TAG "TIMER"

// #define WIFI_SSID CONFIG_WIFI_SSID
// #define WIFI_PASS CONFIG_WIFI_PASSWORD
#define WIFI_SSID "OlVeHotspot"
#define WIFI_PASS "1ViBDzr1ZK"
#define WIFI_CHANNEL 4
#define DISCONNECTIONS_LIMIT 30

// #define TIME_SYNC_INTERVAL 30000  // in milliseconds
// #define TIME_SYNC_WINDOW 5000  // in milliseconds

// IBM MQTT credentials: token rgpCKDpsJA76OgV7zI
// #define MQTT_HOST "mqtt://p28mg8.messaging.internetofthings.ibmcloud.com"
// #define MQTT_USER "use-token-auth"
// #define MQTT_PASS "rgpCKDpsJA76OgV7zI"
// #define MQTT_PORT 1883
// #define MQTT_PORT_CHAR "1883"
// #define MQTT_CLIENT_ID "d:p28mg8:ESP32:office"
// #define MQTT_TOPIC_IBM "iot-2/evt/reading/fmt/json"

// ubidots MQTT credentials:
// #define MQTT_HOST "mqtt://things.ubidots.com"
// #define MQTT_USER "A1E-Cg5BcJyCyq9AImrvvG08MlYmxydLD2"
// #define MQTT_PASS " "
// #define MQTT_PORT 1883
// #define MQTT_PORT_CHAR "1883"
// #define MQTT_CLIENT_ID "ESP32:Office:Testing"

// VIDA mosquitto MQTT credentials
#define MQTT_HOST "10.0.14.150"
#define MQTT_USER " "
#define MQTT_PASS " "
#define MQTT_PORT 1883
#define MQTT_PORT_CHAR "1883"
// used IDs:
// #define MQTT_CLIENT_ID "office"
#define MQTT_CLIENT_ID "testing"
// #define MQTT_CLIENT_ID "civi:ostrov"
// #define MQTT_CLIENT_ID "civi:lego"
// #define MQTT_CLIENT_ID "civi:motory"
// #define MQTT_CLIENT_ID "civi:para"
// #define MQTT_CLIENT_ID "civi:hluk"
// #define MQTT_CLIENT_ID "civi:ideal"
// #define MQTT_CLIENT_ID "civi:stul"
// #define MQTT_CLIENT_ID "civi:agora"

#define MQTT_COMMAND_TIMEOUT 12000
#define MQTT_PUBLISH_DELAY_MS 200

#define RESTART_WINDOW_SEC 1200  // 20 minutes windows
#define RESTART_MIN_SEC    3000  // after 50 minutes

/* FreeRTOS event group to signal when we are connected & ready to make a request */
static EventGroupHandle_t wifi_event_group;
/* The event group allows multiple bits for each event,
   but we only care about one event - are we connected
   to the AP with an IP? */
const int WIFI_CONNECTED_BIT = BIT0;
const int MQTT_CONNECTED_BIT = BIT1;

static const uint8_t bit16_UUID_Eddystone[2] = {0xaa, 0xfe};
static const uint8_t service_data_EddystoneUID[3] = {0xaa, 0xfe, 0x00};
static const uint8_t name_space_VIDA[10] = {0xec, 0x0c, 0xb4, 0xca, 0x9f, 0xa6, 0x84, 0xd6, 0xab, 0xbf};
static const uint8_t duration = 2;

// queue of JSON results
QueueHandle_t results_queue;
// queue of beacon IDs
QueueHandle_t beacons_queue;

/* declare static functions */
static void esp_gap_cb(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param);
static void initialize_ble(void);
static void obtain_time(void);
static void initialize_sntp(void);
static void initialize_wifi(void);
static esp_err_t event_handler(void *ctx, system_event_t *event);
static void restart_timer_callback(void* arg);


static esp_ble_scan_params_t ble_scan_params = {
    .scan_type              = BLE_SCAN_TYPE_ACTIVE,
    .own_addr_type          = BLE_ADDR_TYPE_PUBLIC,
    .scan_filter_policy     = BLE_SCAN_FILTER_ALLOW_ALL,
    .scan_interval          = 0x0050, // 50 ms; N*0.625ms
    .scan_window            = 0x0050  // 50 ms
};

typedef struct VIDABeacon {
  int sumRSSI;
  uint8_t times_found;
  uint8_t bda[6];
  char address[12];
  uint8_t instance[6];
  char visitor[12];
  // char name[29];
  // uint16_t groupID;
  // uint8_t individualID;
  // uint8_t demographic;
  // uint16_t reserved;
  // bool male;
  // bool school_group;
  // bool VIDArd;
  // uint8_t age_group;
  // int averageRSSI;
} VIDABeacon;


// MQTT callback functions
static void status_callback(esp_mqtt_status_t status) {
  switch (status) {
    case ESP_MQTT_STATUS_CONNECTED:
      ESP_LOGI(MQTT_TAG, "Connected to broker.");
      xEventGroupSetBits(wifi_event_group, MQTT_CONNECTED_BIT);
      break;
    case ESP_MQTT_STATUS_DISCONNECTED:
      xEventGroupClearBits(wifi_event_group, MQTT_CONNECTED_BIT);
      // reconnect
      ESP_LOGI(MQTT_TAG, "Disconnected from broker. Reconnecting when on wifi.");
      xEventGroupWaitBits(wifi_event_group, WIFI_CONNECTED_BIT,
                          false, true, portMAX_DELAY);
      esp_mqtt_start(MQTT_HOST, MQTT_PORT_CHAR, MQTT_CLIENT_ID, MQTT_USER, MQTT_PASS);
      break;
  }
}

static void message_callback(const char *topic, uint8_t *payload, size_t len) {
  ESP_LOGI(MQTT_TAG, "incoming: %s => %s (%d)", topic, payload, (int)len);
}

static bool safe_publish(const char *topic, uint8_t *payload, size_t len, uint8_t QoS, bool retained) {
  bool published = esp_mqtt_publish(topic, payload, len, QoS, retained);
  while (!published) {
    ESP_LOGI(MQTT_TAG, "Couldn`t publish, try again after reconnecting!");
    xEventGroupWaitBits(wifi_event_group, MQTT_CONNECTED_BIT,
                        false, true, portMAX_DELAY);
    published = esp_mqtt_publish(topic, payload, len, QoS, retained);
    // ESP_LOGI(MQTT_TAG, "Tried publishing again.");
  }
  return true;
}

static void publish_task(void * pvParameters) {
  char topic[64] = "observers/";
  strcat(topic, MQTT_CLIENT_ID);
  strcat(topic, "/observations");
  char beacon_topic[64];
  cJSON *beacon_address;
  cJSON *result;
  char *out;
  BaseType_t xStatus;
  const TickType_t xTicksToWait = pdMS_TO_TICKS( 5000 );
  for ( ; ; ) {
    xStatus = xQueueReceive( results_queue, &result, xTicksToWait );
    if (xStatus == pdPASS) {
      out = cJSON_Print(result);
      safe_publish(topic, (uint8_t *)out, strlen(out), 1, true);
      ESP_LOGI(MQTT_TAG, "Published! %s", out);
      cJSON_Delete(result);
    }
    xStatus = xQueueReceive( beacons_queue, &result, ( TickType_t ) 10);
    if (xStatus == pdPASS) {
      // esp_mqtt_client_publish(mqttclient, "/v1.6/devices/ESP32:office", out, strlen(out), 2, false);
      strcpy(beacon_topic, "beacons/");
      beacon_address = cJSON_DetachItemFromObjectCaseSensitive(result, "address");
      strcat(beacon_topic, beacon_address->valuestring);
      strcat(beacon_topic, "/visitors");
      out = cJSON_Print(result);
      safe_publish(beacon_topic, (uint8_t *)out, strlen(out), 1, true);
      ESP_LOGI(MQTT_TAG, "Published to topic: %s! %s", beacon_topic, out);
      cJSON_Delete(result);
      vTaskDelay(MQTT_PUBLISH_DELAY_MS);  // when bulk-loading (e.g. after disconnection recovery), BLE would trigger wdt, giving some time for BLE to work with antenna
    }
  }
}

// static void sync_task(void * pvParameters) {
//   for ( ; ; ) {
//     sntp_init();
//     ESP_LOGI(MAIN_TAG, "Time sync");
//     vTaskDelay(TIME_SYNC_WINDOW);
//     sntp_stop();
//     vTaskDelay(TIME_SYNC_INTERVAL - TIME_SYNC_WINDOW);
//   }
// }

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
    // uint8_t *adv_name = NULL;
    // uint8_t adv_name_len = 0;
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
        while (timeinfo.tm_sec % duration != 0) {
          vTaskDelay(500 / portTICK_PERIOD_MS);
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
        // start *= 1000; // to get milliseconds
        ESP_LOGI(BLE_TAG, "scan start success at time: %d:%d:%d", timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
        // if (timeinfo.tm_sec == 0) {  // send alive confirmation every minute
        //   char alive[64] = "/v1.6/devices/";
        //   strcat(alive, MQTT_CLIENT_ID);
        //   strcat(alive, "/alive");
        //   esp_mqtt_publish(alive, (uint8_t *) "{\"value\":true}", 14, 1, false);
        // }
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
              if (memcmp(instance, beacons[i].instance, 6) != 0) {
                memcpy(beacons[i].instance, instance, 6);
                sprintf(beacons[i].visitor, "%02x%02x%02x%02x%02x%02x", instance[0],
                              instance[1], instance[2], instance[3], instance[4], instance[5]);
                ESP_LOGI(MAIN_TAG, "Updated visitor ID: %s on beacon address: %s", beacons[i].visitor, beacons[i].address);
                cJSON *root = cJSON_CreateObject();
                cJSON_AddStringToObject(root, "address", beacons[i].address);
                cJSON_AddStringToObject(root, "id", beacons[i].visitor);
                cJSON_AddNumberToObject(root, "timestamp", start);
                if( xQueueSendToBack( beacons_queue, &root, ( TickType_t ) 10 ) != pdPASS ) {
                  ESP_LOGW(MAIN_TAG, "Could not add beacon ID to queue! Restarting!");
                  esp_restart(); // probably could not reconnect to WiFi
                }
              }
              // ESP_LOGI(BLE_TAG, "Found beacon updated.");
              beacons[i].sumRSSI += scan_result->scan_rst.rssi;
              beacons[i].times_found++;
            } else {
              // ESP_LOGI(BLE_TAG,"New Beacon discovered.");
              memcpy(beacons[beacons_counter].bda, scan_result->scan_rst.bda, 6);
              sprintf(beacons[beacons_counter].address, "%02x%02x%02x%02x%02x%02x", beacons[beacons_counter].bda[0],
                           beacons[beacons_counter].bda[1], beacons[beacons_counter].bda[2],
                          beacons[beacons_counter].bda[3], beacons[beacons_counter].bda[4],
                         beacons[beacons_counter].bda[5]);
              memcpy(beacons[beacons_counter].instance, instance, 6);
              sprintf(beacons[beacons_counter].visitor, "%02x%02x%02x%02x%02x%02x", instance[0],
                            instance[1], instance[2], instance[3], instance[4], instance[5]);
              // adv_name = esp_ble_resolve_adv_data(scan_result->scan_rst.ble_adv,
              //                                     ESP_BLE_AD_TYPE_NAME_CMPL, &adv_name_len);
              // strncpy(beacons[beacons_counter].name, (char *)adv_name, adv_name_len);
              ESP_LOGI(MAIN_TAG, "New visitor ID: %s on beacon address: %s", beacons[beacons_counter].visitor, beacons[beacons_counter].address);
              beacons[beacons_counter].sumRSSI = scan_result->scan_rst.rssi;
              beacons[beacons_counter].times_found = 1;
              // beacons[beacons_counter].groupID = ((uint16_t) *instance << 8) | *(instance+1); // first two bytes of instance
              // beacons[beacons_counter].individualID = *(instance+2);  // third byte of instance
              // beacons[beacons_counter].demographic = *(instance+3);  // fourth byte of instance
              // beacons[beacons_counter].reserved = ((uint16_t) *(instance+4) << 8) | *(instance+5); // last two bytes of instance
              // beacons[beacons_counter].male = beacons[beacons_counter].demographic & 0x01; // first bit of demographic is 1; demographic && 0000 0001
              // beacons[beacons_counter].school_group = (beacons[beacons_counter].demographic & 0x02) >> 1;  // second bit of demographic is 1
              // beacons[beacons_counter].VIDArd = (beacons[beacons_counter].demographic & 0x04) >> 2;  // third bit is 1
              // beacons[beacons_counter].age_group = (beacons[beacons_counter].demographic & 0x30) >> 4;  // fifth and sixth bits shifted to right 0011 0000 -> 0000 0011
              cJSON *root = cJSON_CreateObject();
              cJSON_AddStringToObject(root, "address", beacons[beacons_counter].address);
              cJSON_AddStringToObject(root, "id", beacons[beacons_counter].visitor);
              cJSON_AddNumberToObject(root, "timestamp", start);
              if( xQueueSendToBack( beacons_queue, &root, ( TickType_t ) 10 ) != pdPASS ) {
                ESP_LOGE(MAIN_TAG, "Could not add beacon ID to queue! Restarting!");
                esp_restart(); // probably could not reconnect to WiFi
              }

              beacons_counter++;
            }
            break;
          }
        case ESP_GAP_SEARCH_INQ_CMPL_EVT: {
            int averageRSSI;
            cJSON *root = cJSON_CreateObject();
            cJSON_AddNumberToObject(root, "timestamp", start);
            cJSON *rssi = cJSON_CreateObject();
            cJSON *times_observed = cJSON_CreateObject();
            cJSON_AddItemToObject(root, "rssi", rssi);
            cJSON_AddItemToObject(root, "timesObserved", times_observed);

            for (size_t i = 0; i < beacons_counter; i++) {
              if (beacons[i].times_found > 0) {
                averageRSSI = beacons[i].sumRSSI / beacons[i].times_found;
                // ESP_LOGI(BLE_TAG, "Found %s %d times with %d average RSSI and with ID %d belonging to group %d and address %s",
                //               beacons[i].name, beacons[i].times_found, beacons[i].averageRSSI, beacons[i].individualID, beacons[i].groupID, beacons[i].address);
                // ESP_LOGI(BLE_TAG, "%s is %s, %s to a school group, %s VIDArd and belongs to age group number %d\n",
                //               beacons[i].name, beacons[i].male ? "male" : "female",
                //               beacons[i].school_group ? "belongs" : "doesn`t belong",
                //               beacons[i].VIDArd ? "solves" : "doesn`t solve",
                //               beacons[i].age_group);
                cJSON_AddNumberToObject(rssi, beacons[i].visitor, averageRSSI);
                cJSON_AddNumberToObject(times_observed, beacons[i].visitor, beacons[i].times_found);

                beacons[i].times_found = 0;
                beacons[i].sumRSSI = 0;

                dp_counter++;
              }
            }

            ESP_LOGI(BLE_TAG, "Found %d VIDA! beacons", dp_counter);
            if( xQueueSendToBack( results_queue, &root, ( TickType_t ) 10 ) != pdPASS ) {
              ESP_LOGE(MAIN_TAG, "Could not add JSON to queue! Waiting for MQTT connection!");
              ESP_ERROR_CHECK(esp_wifi_disconnect());
              xEventGroupWaitBits(wifi_event_group, MQTT_CONNECTED_BIT,
                                  false, true, portMAX_DELAY);
              // esp_restart(); // probably could not reconnect to WiFi
            }

            time(&now);
            localtime_r(&now, &timeinfo);
            // ESP_LOGI(BLE_TAG, "Search inquire complete at time: %d:%d:%d.", timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
            while (timeinfo.tm_sec % duration != 0) {
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
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
            .channel = WIFI_CHANNEL,
        },
    };
    ESP_LOGI(WIFI_TAG, "Setting WiFi configuration SSID %s...", wifi_config.sta.ssid);
    ESP_ERROR_CHECK( esp_wifi_set_mode(WIFI_MODE_STA) );
    ESP_ERROR_CHECK( esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config) );
}

static void initialize_sntp(void)
{
    ESP_LOGI(MAIN_TAG, "Initializing SNTP");
    sntp_setoperatingmode(SNTP_OPMODE_POLL);
    // sntp_setservername(0, "time.fi.muni.cz");
    // sntp_setservername(1, "pool.ntp.org");
    // sntp_setservername(2, "europe.pool.ntp.org");
    // sntp_setservername(3, "cz.pool.ntp.org");
    ip_addr_t IP;
    IP_ADDR4(&IP, 10, 0, 14, 150);
    sntp_setserver(0, &IP);
    sntp_setservername(1, "time.fi.muni.cz");
    sntp_setservername(2, "cz.pool.ntp.org");
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
    static bool onceConnected;
    static uint8_t initialDisconnections;
    switch(event->event_id) {
    case SYSTEM_EVENT_STA_START:
        // ESP_LOGI(WIFI_TAG, "Started WIFI, trying to connect.");
        ESP_ERROR_CHECK(esp_wifi_connect());
        break;
    case SYSTEM_EVENT_STA_GOT_IP:
        onceConnected = true;
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
        esp_mqtt_start(MQTT_HOST, MQTT_PORT_CHAR, MQTT_CLIENT_ID, MQTT_USER, MQTT_PASS);
        break;
    case SYSTEM_EVENT_STA_DISCONNECTED:
        /* This is a workaround as ESP32 WiFi libs don't currently
           auto-reassociate. */
        ESP_LOGE(WIFI_TAG, "Disconnected from WIFI");
        if (onceConnected) {
          ESP_ERROR_CHECK(esp_wifi_stop());
          vTaskDelay(1000);
          ESP_ERROR_CHECK(esp_wifi_start());
          onceConnected = false;
          initialDisconnections = 0;
          // esp_restart();
        } else {
          initialDisconnections++;
          if (initialDisconnections > DISCONNECTIONS_LIMIT) {
            ESP_ERROR_CHECK(esp_wifi_stop());
            vTaskDelay(1000);
            ESP_ERROR_CHECK(esp_wifi_start());
            onceConnected = false;
            initialDisconnections = 0;
          } else {
            ESP_ERROR_CHECK(esp_wifi_connect());
          }
        }
        xEventGroupClearBits(wifi_event_group, WIFI_CONNECTED_BIT);
        xEventGroupClearBits(wifi_event_group, MQTT_CONNECTED_BIT);
        break;
    case SYSTEM_EVENT_STA_LOST_IP:
        ESP_ERROR_CHECK(esp_wifi_stop());
        vTaskDelay(1000);
        ESP_ERROR_CHECK(esp_wifi_start());
        onceConnected = false;
        initialDisconnections = 0;
        break;
    case SYSTEM_EVENT_STA_STOP:
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
    while(timeinfo.tm_year < (2016 - 1900) && ++retry <= retry_count) {
        if (retry == retry_count) {
          esp_restart();
        }
        ESP_LOGI(MAIN_TAG, "Waiting for system time to be set... (%d/%d)", retry, retry_count);
        vTaskDelay(2000 / portTICK_PERIOD_MS);
        time(&now);
        localtime_r(&now, &timeinfo);
    }
    // sntp_stop();
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

static void restart_timer_callback(void* arg)
{
  ESP_LOGI(TIMER_TAG, "Planned restart!");
  ESP_ERROR_CHECK(esp_wifi_stop());
  ESP_ERROR_CHECK(esp_wifi_deinit());
  esp_restart();
  // esp_wifi_stop();
  // esp_wifi_start();
}

void app_main()
{
    esp_log_level_set("wifi", ESP_LOG_VERBOSE);
    esp_log_level_set("BT_HCI", ESP_LOG_NONE);

    ESP_LOGI(MAIN_TAG, "This is %s observer", MQTT_CLIENT_ID);
    ESP_LOGI(MAIN_TAG, "Application version: %s", APP_VERSION);
    // timer for planned restart
    const esp_timer_create_args_t restart_timer_args = {
      .callback = &restart_timer_callback,
      .name = "restart"
    };

    float random = (float) esp_random();
    random = random / UINT32_MAX;
    random = random * RESTART_WINDOW_SEC;
    int64_t restart_value = (int64_t) random + RESTART_MIN_SEC;
    ESP_LOGI(TIMER_TAG, "Planned restart after %lld minutes and %lld seconds. (Total %lld seconds)", restart_value/60, restart_value%60, restart_value);
    restart_value *= 1000000;
    esp_timer_handle_t restart_timer;
    ESP_ERROR_CHECK(esp_timer_create(&restart_timer_args, &restart_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(restart_timer, restart_value));

    // Initialize NVS.
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK( ret );


    esp_mqtt_init(status_callback, message_callback, 256, MQTT_COMMAND_TIMEOUT);
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
      ESP_LOGI(MAIN_TAG, "Time is not set yet. Connecting to WiFi and getting time over NTP.");
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
    ESP_LOGI(MAIN_TAG, "The current date/time in Brno is: %s.", strftime_buf);

    initialize_ble();

    results_queue = xQueueCreate(120, sizeof(cJSON *));
    if (results_queue == 0) {
      ESP_LOGI(MAIN_TAG, "Could not allocate a results queue!");
    }

    beacons_queue = xQueueCreate(120, sizeof(cJSON *));
    if (beacons_queue == 0) {
      ESP_LOGI(MAIN_TAG, "Could not allocate a beacons queue!");
    }

    // set ble scan parameters and start scanning afterwards
    esp_err_t scan_ret = esp_ble_gap_set_scan_params(&ble_scan_params);
    if (scan_ret){
        ESP_LOGE(BLE_TAG, "set scan params error, error code = %x", scan_ret);
    }  // scanning starts after parameters are set (see callback function)


    xTaskCreatePinnedToCore(publish_task, "Publish", 3000, NULL, tskIDLE_PRIORITY, NULL, 1);
    // xTaskCreatePinnedToCore(sync_task, "Periodic time sync", 3000, NULL, tskIDLE_PRIORITY, NULL, 1);
    // ESP_LOGI(BLE_TAG, "Free heap size: %d", xPortGetFreeHeapSize());

    ESP_LOGI(MAIN_TAG, "Main finished!");

    // const int deep_sleep_sec = 10;
    // ESP_LOGI(WIFI_TAG, "Entering deep sleep for %d seconds", deep_sleep_sec);
    // esp_deep_sleep(1000000LL * deep_sleep_sec);


}
