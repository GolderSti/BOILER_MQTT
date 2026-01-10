#include <string.h>
#include "esp_system.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "wifi_common.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"
#include "freertos/timers.h"
#include "nvs_flash.h"

#include "lwip/err.h"
#include "lwip/sys.h"

#define QUEUE_LENGTH 1
#define MAX_CONNECT_CB 4
#define MAX_DISCONNECT_CB 4
#define WIFI_TASK_STACK_SIZE (configMINIMAL_STACK_SIZE * 4)

static const char *TAG = "WiFiCMN";

// Глобальные переменные
EventGroupHandle_t s_net_state_event_group = NULL;
static SemaphoreHandle_t s_wifi_mutex = NULL;

// Callback массивы
static uint16_t uConnectCallbackCount = 0;
static uint16_t uDisConnectCallbackCount = 0;
static wifi_callback_t Connect_CB[MAX_CONNECT_CB];
static wifi_callback_t Disconnect_CB[MAX_DISCONNECT_CB];

// Управление Wi-Fi
static uint16_t uWiFiNeddedThreadsCount = 0;
QueueHandle_t xWiFi_Queue = NULL;

// // Повторные попытки подключения
// typedef struct {
//     uint8_t retry_count;
//     uint32_t last_retry_delay;
//     TickType_t last_retry_time;
//     bool reconnect_in_progress;
// } wifi_reconnect_state_t;

// static wifi_reconnect_state_t s_reconnect_state = {0};
// static TimerHandle_t s_reconnect_timer = NULL;

// Новые переменные для сканирования соседних AP
static void (*s_scan_callback)(wifi_neighbor_scan_result_t *) = NULL;
static bool s_scan_in_progress = false;
// static bool s_scan_for_roaming = false;  
static esp_netif_ip_info_t current_IP;

// ==================== ВСПОМОГАТЕЛЬНЫЕ ФУНКЦИИ ====================

/*!
 *  @brief Мьютекс для защиты разделяемых данных
 */
static esp_err_t wifi_lock(uint32_t timeout_ms) {
    if (s_wifi_mutex == NULL) {
        s_wifi_mutex = xSemaphoreCreateMutex();
        if (s_wifi_mutex == NULL) return ESP_FAIL;
    }
    return xSemaphoreTake(s_wifi_mutex, pdMS_TO_TICKS(timeout_ms)) ? ESP_OK : ESP_ERR_TIMEOUT;
}

static void wifi_unlock(void) {
    xSemaphoreGive(s_wifi_mutex);
}

/*!
 *  @brief Расчет экспоненциальной задержки
 */
// static uint32_t calculate_backoff_delay(uint8_t retry_count) {
//     uint32_t delay = WIFI_RECONNECT_BASE_DELAY_MS;
    
//     for (uint8_t i = 0; i < retry_count && delay < WIFI_RECONNECT_MAX_DELAY_MS; i++) {
//         delay *= WIFI_RECONNECT_MULTIPLIER;
//     }
    
//     if (delay > WIFI_RECONNECT_MAX_DELAY_MS) {
//         delay = WIFI_RECONNECT_MAX_DELAY_MS;
//     }
    
//     // Добавляем случайный джиттер (±20%)
//     uint32_t jitter = (esp_random() % (delay / 5)) - (delay / 10);
//     delay += jitter;
    
//     return delay;
// }

/*!
 *  @brief Сброс состояния повторных попыток
 */
// static void reset_reconnect_state(void) {
//     s_reconnect_state.retry_count = 0;
//     s_reconnect_state.last_retry_delay = 0;
//     s_reconnect_state.last_retry_time = 0;
//     s_reconnect_state.reconnect_in_progress = false;
    
//     if (s_reconnect_timer != NULL) {
//         xTimerStop(s_reconnect_timer, portMAX_DELAY);
//     }
    
//     ESP_LOGD(TAG, "Reconnect state reset");
// }

/*!
 *  @brief Планирование следующей попытки подключения
 */
// static void schedule_reconnect_attempt(void) {
//     if (s_reconnect_state.retry_count >= WIFI_RECONNECT_MAX_RETRIES) {
//         ESP_LOGW(TAG, "Maximum reconnect attempts (%d) reached", WIFI_RECONNECT_MAX_RETRIES);
//         reset_reconnect_state();
        
//         for (uint32_t ulTmp = 0; ulTmp < uDisConnectCallbackCount; ulTmp++) {
//             if (Disconnect_CB[ulTmp] != NULL) {
//                 Disconnect_CB[ulTmp]();
//             }
//         }
//         return;
//     }
    
//     uint32_t delay_ms = calculate_backoff_delay(s_reconnect_state.retry_count);
//     s_reconnect_state.last_retry_delay = delay_ms;
//     s_reconnect_state.retry_count++;
    
//     ESP_LOGI(TAG, "Scheduling reconnect attempt %d in %u ms", 
//              s_reconnect_state.retry_count, delay_ms);
    
//     if (s_reconnect_timer == NULL) {
//         s_reconnect_timer = xTimerCreate(
//             "wifi_reconnect_timer",
//             pdMS_TO_TICKS(delay_ms),
//             pdFALSE,
//             NULL,
//             reconnect_timer_callback
//         );
//     }
    
//     if (s_reconnect_timer != NULL) {
//         xTimerChangePeriod(s_reconnect_timer, pdMS_TO_TICKS(delay_ms), portMAX_DELAY);
//         xTimerStart(s_reconnect_timer, portMAX_DELAY);
//     } else {
//         ESP_LOGE(TAG, "Failed to create reconnect timer");
//         reset_reconnect_state();
//     }
// }

/*!
 *  @brief Callback таймера для повторного подключения
 */
// static void reconnect_timer_callback(TimerHandle_t xTimer) {
//     ESP_LOGI(TAG, "Attempting WiFi reconnect (attempt %d/%d)", 
//              s_reconnect_state.retry_count + 1, WIFI_RECONNECT_MAX_RETRIES);
    
//     esp_err_t err = esp_wifi_connect();
//     if (err != ESP_OK) {
//         ESP_LOGE(TAG, "Failed to initiate WiFi connect: %s", esp_err_to_name(err));
//         schedule_reconnect_attempt();
//     }
// }

/*!
 *  @brief Запуск процесса повторного подключения
 */
// static void start_reconnect_process(void) {
//     if (s_reconnect_state.reconnect_in_progress) {
//         ESP_LOGW(TAG, "Reconnect already in progress");
//         return;
//     }
    
//     s_reconnect_state.reconnect_in_progress = true;
//     s_reconnect_state.retry_count = 0;
    
//     ESP_LOGI(TAG, "Starting WiFi reconnect process");
//     schedule_reconnect_attempt();
// }

// ==================== НОВЫЕ ФУНКЦИИ ДЛЯ ПОЛУЧЕНИЯ СТАТУСА ====================

/*!
 *  @brief Получение текущего статуса WiFi подключения
 */
esp_err_t WFc_GetCurrentStatus(wifi_status_info_t *status)
{
    if (!status) return ESP_ERR_INVALID_ARG;
    memset(status, 0, sizeof(*status));

    /* ---- MAC устройства ---- */
    esp_wifi_get_mac(WIFI_IF_STA, status->sta_mac);

    /* ---- Проверка подключения ---- */
    wifi_ap_record_t ap;
    esp_err_t err = esp_wifi_sta_get_ap_info(&ap);

    if (err == ESP_OK) {
        status->is_connected = true;

        /* AP info */
        memcpy(status->bssid, ap.bssid, 6);
        strncpy(status->ssid, (char *)ap.ssid, MAX_SSID_LENGTH);
        status->rssi = ap.rssi;
        status->channel = ap.primary;
        status->second_channel = ap.second;
        status->auth_mode = ap.authmode;

        /* IP info */
        esp_netif_t *netif =
            esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        if (netif) {
            esp_netif_ip_info_t ip;
            if (esp_netif_get_ip_info(netif, &ip) == ESP_OK) {
                status->ip_addr = ip.ip;
                status->gateway = ip.gw;
                status->netmask = ip.netmask;
            }
        }
    } else {
        status->is_connected = false;
        status->rssi = WIFI_ROAMING_MIN_RSSI;
    }

    return ESP_OK;
}

/*!
 *  @brief Сканирование соседних точек доступа
 */
esp_err_t WFc_ScanNeighborAPs(wifi_neighbor_scan_result_t *scan_result, bool async) {
    if (scan_result == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    if (s_scan_in_progress ) {
        ESP_LOGW(TAG, "Scan already in progress");
        return ESP_ERR_INVALID_STATE;
    }
    
    memset(scan_result, 0, sizeof(wifi_neighbor_scan_result_t));
    
    if (async) {
        // Асинхронное сканирование
        s_scan_in_progress = true;
        
        wifi_scan_config_t scan_config = {
            .ssid = NULL,
            .bssid = NULL,
            .channel = 0,
            .show_hidden = true,
            .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        };
        
        esp_err_t err = esp_wifi_scan_start(&scan_config, false);
        if (err != ESP_OK) {
            s_scan_in_progress = false;
            ESP_LOGE(TAG, "Failed to start async scan: %s", esp_err_to_name(err));
            return err;
        }
        
        ESP_LOGD(TAG, "Async neighbor scan started");
        return ESP_OK;
    } else {
        // Синхронное сканирование
        wifi_scan_config_t scan_config = {
            .ssid = NULL,
            .bssid = NULL,
            .channel = 0,
            .show_hidden = true,
            .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        };
        
        esp_err_t err = esp_wifi_scan_start(&scan_config, true);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to start sync scan: %s", esp_err_to_name(err));
            return err;
        }
        
        uint16_t ap_count = 0;
        err = esp_wifi_scan_get_ap_num(&ap_count);
        
        if (err == ESP_OK && ap_count > 0) {
            uint16_t ap_record_count = (ap_count > MAX_NEIGHBOR_APS) ? MAX_NEIGHBOR_APS : ap_count;
            wifi_ap_record_t *ap_records = malloc(ap_record_count * sizeof(wifi_ap_record_t));
            
            if (ap_records == NULL) {
                return ESP_ERR_NO_MEM;
            }
            
            err = esp_wifi_scan_get_ap_records(&ap_record_count, ap_records);
            
            if (err == ESP_OK) {
                scan_result->ap_count = ap_record_count;
                scan_result->scan_status = ESP_OK;
                
                for (int i = 0; i < ap_record_count; i++) {
                    wifi_neighbor_ap_t *neighbor = &scan_result->aps[i];
                    
                    strncpy(neighbor->ssid, (char*)ap_records[i].ssid, MAX_SSID_LENGTH);
                    memcpy(neighbor->bssid, ap_records[i].bssid, 6);
                    neighbor->rssi = ap_records[i].rssi;
                    neighbor->channel = ap_records[i].primary;
                    neighbor->auth_mode = ap_records[i].authmode;
                    neighbor->is_hidden = (strlen(neighbor->ssid) == 0);
                    neighbor->pairwise_cipher = ap_records[i].pairwise_cipher;
                    neighbor->group_cipher = ap_records[i].group_cipher;
                }
                
                ESP_LOGD(TAG, "Sync neighbor scan completed: %d APs found", ap_record_count);
            }
            
            free(ap_records);
        } else {
            scan_result->scan_status = (err == ESP_OK) ? ESP_OK : err;
            ESP_LOGD(TAG, "Sync neighbor scan completed: %d APs found", ap_count);
        }
        
        return scan_result->scan_status;
    }
}

/*!
 *  @brief Регистрация callback для результатов сканирования
 */
void WFc_RegisterScanCallback(void (*callback)(wifi_neighbor_scan_result_t *)) {
    s_scan_callback = callback;
}

// ==================== ФУНКЦИИ РОУМИНГА ====================

/*!
 *  @brief Обновление информации о текущем RSSI
 */
// static void update_current_rssi(void) {
//     wifi_ap_record_t ap_info;
//     esp_err_t err = esp_wifi_sta_get_ap_info(&ap_info);
    
//     if (err == ESP_OK) {
//         s_current_best_rssi = ap_info.rssi;
//         memcpy(s_current_bssid, ap_info.bssid, 6);
        
//         ESP_LOGD(TAG, "Current AP: " MACSTR ", RSSI: %d", 
//                 MAC2STR(ap_info.bssid), ap_info.rssi);
//     }
// }


/*!
 *  @brief Периодическое Обновление информации о текущем RSSI
 */
// static void rssi_update_timer_callback(TimerHandle_t xTimer) {
//     wifi_ap_record_t ap_info;
//     esp_err_t err = esp_wifi_sta_get_ap_info(&ap_info);
    
//     if (err == ESP_OK) {        
//         ESP_LOGD(TAG, "Current AP: " MACSTR ", RSSI: %d", 
//                 MAC2STR(ap_info.bssid), ap_info.rssi);
//     }

//     // WFc_TriggerRoamingScan();
// }

/*!
 *  @brief Переключение на лучшую точку доступа
 */
// static void switch_to_best_ap(wifi_ap_record_t *best_ap) {
//     s_current_best_rssi = best_ap->rssi;
//     memcpy(s_current_bssid, best_ap->bssid, 6);
    
//     ESP_LOGI(TAG, "Switching to AP: " MACSTR " with RSSI: %d", 
//             MAC2STR(best_ap->bssid), best_ap->rssi);
//     esp_err_t err;
    
//     wifi_config_t wifi_config = {
//         .sta = {
//             .ssid = EXAMPLE_WIFI_SSID,
//             .password = EXAMPLE_WIFI_PASSWORD,
//             .bssid_set = 1,
//             .rm_enabled = 1,
//             .btm_enabled = 1,
//         },
//     };
    
//     memcpy(wifi_config.sta.bssid, best_ap->bssid, 6);
    
//     err = esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config);
//     if (err != ESP_OK) {
//         ESP_LOGE(TAG, "Failed to set WiFi config: %s", esp_err_to_name(err));
//         return;
//     }
    
//     err = esp_wifi_connect();
//     if (err != ESP_OK) {
//         ESP_LOGE(TAG, "Failed to connect to new AP: %s", esp_err_to_name(err));
//     }
// }

/*!
 *  @brief Callback завершения сканирования (общий для всех типов)
 */
static void on_scan_done_common(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data) {
    
    // Если это сканирование для соседних AP (внешний запрос)
    if (s_scan_in_progress && s_scan_callback != NULL) {
        wifi_neighbor_scan_result_t scan_result = {0};
        
        uint16_t ap_count = 0;
        esp_err_t err = esp_wifi_scan_get_ap_num(&ap_count);
        
        if (err == ESP_OK && ap_count > 0) {
            uint16_t ap_record_count = (ap_count > MAX_NEIGHBOR_APS) ? MAX_NEIGHBOR_APS : ap_count;
            wifi_ap_record_t *ap_records = malloc(ap_record_count * sizeof(wifi_ap_record_t));
            
            if (ap_records != NULL) {
                err = esp_wifi_scan_get_ap_records(&ap_record_count, ap_records);
                
                if (err == ESP_OK) {
                    scan_result.ap_count = ap_record_count;
                    scan_result.scan_status = ESP_OK;
                    
                    for (int i = 0; i < ap_record_count; i++) {
                        wifi_neighbor_ap_t *neighbor = &scan_result.aps[i];
                        
                        strncpy(neighbor->ssid, (char*)ap_records[i].ssid, MAX_SSID_LENGTH);
                        memcpy(neighbor->bssid, ap_records[i].bssid, 6);
                        neighbor->rssi = ap_records[i].rssi;
                        neighbor->channel = ap_records[i].primary;
                        neighbor->auth_mode = ap_records[i].authmode;
                        neighbor->is_hidden = (strlen(neighbor->ssid) == 0);
                        neighbor->pairwise_cipher = ap_records[i].pairwise_cipher;
                        neighbor->group_cipher = ap_records[i].group_cipher;
                    }
                    
                    ESP_LOGD(TAG, "Neighbor scan completed: %d APs found", ap_record_count);
                }
                
                free(ap_records);
            }
        } else {
            scan_result.scan_status = (err == ESP_OK) ? ESP_OK : err;
            ESP_LOGD(TAG, "Neighbor scan completed: %d APs found", ap_count);
        }
        
        // Вызываем callback с результатами
        s_scan_callback(&scan_result);
        s_scan_in_progress = false;
    }
}

// ==================== CALLBACK'И СОБЫТИЙ ====================


/*!
 *  @brief Callback отключения от Wi-Fi
 */
static void on_wifi_connect(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data) {                            

//
}

/*!
 *  @brief Callback отключения от Wi-Fi
 */
static void on_wifi_disconnect(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data) {
    EventBits_t uxBits;
    wifi_event_sta_disconnected_t *event = (wifi_event_sta_disconnected_t *)event_data;
    
    xEventGroupClearBits(s_net_state_event_group, GOT_IPV4_BIT);
    
    
    uxBits = xEventGroupGetBits(s_net_state_event_group);
    if ((uxBits & WIFI_NEEDED) == WIFI_NEEDED) {
        // ESP_LOGW(TAG, "Wi-Fi disconnected (reason: %d), starting reconnect...", event->reason);
        
        switch (event->reason) {
            case WIFI_REASON_NO_AP_FOUND:
            case WIFI_REASON_AUTH_FAIL:
                ESP_LOGE(TAG, "Critical error: %d. Manual intervention required.", event->reason);
                // reset_reconnect_state();
                return;
            case WIFI_REASON_ROAMING:
                ESP_LOGI(TAG, "station disconnected during roaming");
                break;    
            // case WIFI_REASON_BASIC_RATE_NOT_SUPPORT:
            //     ESP_LOGW(TAG, "Switching to 802.11 bgn mode");
            //     esp_wifi_set_protocol(ESP_IF_WIFI_STA, WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N);
            //     break;
                
            default:
                ESP_LOGI(TAG, "station disconnected with reason %d", event->reason);
                for (uint32_t ulTmp = 0; ulTmp < uDisConnectCallbackCount; ulTmp++) {
                    if (Disconnect_CB[ulTmp] != NULL) {
                        Disconnect_CB[ulTmp]();
                    }
                }
                break;
        }
        
    } else {
        ESP_LOGD(TAG, "WiFi disconnected (OK - not needed)");
        for (uint32_t ulTmp = 0; ulTmp < uDisConnectCallbackCount; ulTmp++) {
            if (Disconnect_CB[ulTmp] != NULL) {
                Disconnect_CB[ulTmp]();
            }
        }
    }
}

/*!
 *  @brief Callback получения IP адреса
 */
static void on_got_ip(void *arg, esp_event_base_t event_base,
                      int32_t event_id, void *event_data) {
    ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        
    ESP_LOGI(TAG, "Got IP: " IPSTR "", 
            IP2STR(&event->ip_info.ip));

    current_IP.gw = event->ip_info.gw;
    current_IP.ip = event->ip_info.ip;
    current_IP.netmask = event->ip_info.netmask;
    
    // if (s_reconnect_state.reconnect_in_progress) {
    //     ESP_LOGI(TAG, "WiFi reconnected successfully after %d attempts", 
    //              s_reconnect_state.retry_count);
    // }
    // reset_reconnect_state();
    
    xEventGroupSetBits(s_net_state_event_group, GOT_IPV4_BIT);
    
    for (uint32_t ulTmp = 0; ulTmp < uConnectCallbackCount; ulTmp++) {
        if (Connect_CB[ulTmp] != NULL) {
            Connect_CB[ulTmp]();
        }
    }    
}

// ==================== ОСНОВНЫЕ ФУНКЦИИ ====================

/*!
 *  @brief Основная задача управления Wi-Fi
 */
void WFc_MainTask(void *pvParameters) {
    uint32_t uiTmp = 1;

    while (1) {       
        if (xQueueReceive(xWiFi_Queue, &uiTmp, portMAX_DELAY) == pdPASS) {       
            if (uiTmp == 1) {
                EventBits_t uxBits;
                
                uxBits = xEventGroupGetBits(s_net_state_event_group);
                if ((uxBits & GOT_IPV4_BIT) != GOT_IPV4_BIT) {
                    xEventGroupSetBits(s_net_state_event_group, WIFI_NEEDED);
                    
                    esp_err_t err = esp_wifi_start();
                    if (err != ESP_OK) {
                        ESP_LOGE(TAG, "Failed to start WiFi: %s", esp_err_to_name(err));
                        continue;
                    }
                    // WFc_TriggerRoamingScan();
                    err = esp_wifi_connect();
                    if (err != ESP_OK) {
                        ESP_LOGE(TAG, "Failed to connect WiFi: %s", esp_err_to_name(err));
                    }
                    
                    ESP_LOGD(TAG, "WiFi STARTED");
                } else {
                    ESP_LOGD(TAG, "Already connected");
                }

#ifdef WIFI_CONTINIOUS_WORK
                break;
#endif
            } else {
                vTaskDelay(pdMS_TO_TICKS(WIFI_MINWORKTIME));
                
                UBaseType_t uxNumberOfItems = uxQueueMessagesWaiting(xWiFi_Queue);
                if (uxNumberOfItems > 0) {
                    continue;
                } else {
                    xEventGroupClearBits(s_net_state_event_group, WIFI_NEEDED);
                    // reset_reconnect_state();
                    
                    esp_err_t err = esp_wifi_disconnect();
                    if (err != ESP_OK) {
                        ESP_LOGE(TAG, "Failed to disconnect WiFi: %s", esp_err_to_name(err));
                    }
                    err = esp_wifi_stop();
                    if (err != ESP_OK) {
                        ESP_LOGE(TAG, "Failed to stop WiFi: %s", esp_err_to_name(err));
                    }
                    
                    ESP_LOGD(TAG, "WiFi STOPPED");
                }
            }
        }      
    }

    vTaskDelete(NULL);    
}

/*!
 *  @brief Инициализация Wi-Fi
 */
void WFc_Init() {
    if (s_net_state_event_group == NULL) {
        s_wifi_mutex = xSemaphoreCreateMutex();
        if (s_wifi_mutex == NULL) ESP_LOGE(TAG,"\tWiFI data mutex init error!");

        uConnectCallbackCount = 0;
        uDisConnectCallbackCount = 0;
        uWiFiNeddedThreadsCount = 0;

        s_scan_in_progress = false;      
        // s_scan_for_roaming = false;

        s_net_state_event_group = xEventGroupCreate();

        ESP_ERROR_CHECK(esp_netif_init());

        ESP_ERROR_CHECK(esp_event_loop_create_default());
        esp_netif_create_default_wifi_sta();

        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        ESP_ERROR_CHECK(esp_wifi_init(&cfg));

        ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_CONNECTED, &on_wifi_connect, NULL));
        ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &on_wifi_disconnect, NULL));
        ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_SCAN_DONE, &on_scan_done_common, NULL)); 
        ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &on_got_ip, NULL));

        ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));


    wifi_config_t wifi_config = {
        .sta = {
            .ssid = EXAMPLE_WIFI_SSID,
            .password = EXAMPLE_WIFI_PASSWORD,
            .scan_method = WIFI_ALL_CHANNEL_SCAN,
            .sort_method = WIFI_CONNECT_AP_BY_SIGNAL,
                // .rm_enabled = 1,
                // .mbo_enabled = 1,
                // .ft_enabled = 1,
            // .btm_enabled = 1,
            .rm_enabled  = 0,
            .btm_enabled = 0,
            .mbo_enabled = 0,
            .ft_enabled  = 0,
            }
        };
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));

        ESP_LOGI(TAG, "Connecting to %s with roaming enabled...", wifi_config.sta.ssid);

        xWiFi_Queue = xQueueCreate(QUEUE_LENGTH, sizeof(uint32_t));
        xTaskCreate(WFc_MainTask, "WFc_MainTask", WIFI_TASK_STACK_SIZE, NULL, 5, NULL);
    } else {
        ESP_LOGD(TAG, "Wi-Fi already initialized");
    }
}

/*!
 *  @brief Запуск Wi-Fi
 */
void WFc_Start() {
    if (wifi_lock(1000) != ESP_OK) return;
    
    uint32_t uiTmp = 1;
    
    if (s_net_state_event_group == NULL) {
        // WFc_Init();
        // xQueueOverwrite(xWiFi_Queue, &uiTmp);
        // ESP_LOGD(TAG, "WiFi Initialized and Start sent.");
        ESP_LOGE(TAG,"\tWiFi didn't initialized. Initialize first!");
    } else {
        EventBits_t uxBits = xEventGroupGetBits(s_net_state_event_group);
        if ((uxBits & GOT_IPV4_BIT) != GOT_IPV4_BIT) {
            xQueueOverwrite(xWiFi_Queue, &uiTmp);
            ESP_LOGD(TAG, "WiFi Start sent.");
        } else {
            ESP_LOGI(TAG, "WiFi already connected");
        }
    }
    
    uWiFiNeddedThreadsCount++;
    ESP_LOGD(TAG, "[uWiFiNeddedThreadsCount] = %u", uWiFiNeddedThreadsCount);
    
    wifi_unlock();
}

/*!
 *  @brief Остановка Wi-Fi
 */
void WFc_Stop() {
    if (wifi_lock(1000) != ESP_OK) return;
    
    if (uWiFiNeddedThreadsCount > 0) {
        uWiFiNeddedThreadsCount--;
        
        if (uWiFiNeddedThreadsCount == 0) {
            uint32_t uiTmp = 0;
            if (s_net_state_event_group != NULL) {
                // reset_reconnect_state();
                xQueueOverwrite(xWiFi_Queue, &uiTmp);
                ESP_LOGD(TAG, "WiFi stop signal sent");
            } else {
                ESP_LOGW(TAG, "Wi-Fi not initialized");
            }
        }
    } else {
        ESP_LOGI(TAG, "Excessive WiFi stop command");
    }
    
    ESP_LOGD(TAG, "[uWiFiNeddedThreadsCount] = %u", uWiFiNeddedThreadsCount);
    wifi_unlock();
}

// ==================== ПУБЛИЧНЫЕ ФУНКЦИИ ====================

/*!
 *  @brief Проверка подключения к Wi-Fi
 */
int WFc_IsConnected() {
    EventBits_t uxBits;
    
    uxBits = xEventGroupGetBits(s_net_state_event_group);
    return ((uxBits & GOT_IPV4_BIT) == GOT_IPV4_BIT);
}

/*!
 *  @brief Ожидание подключения к Wi-Fi
 */
esp_err_t WFc_ConnectionWait(TickType_t xTicksToWait) {
    EventBits_t uxBits;
    if (s_net_state_event_group != NULL) {
        uxBits = xEventGroupWaitBits(
            s_net_state_event_group,
            GOT_IPV4_BIT,
            pdFALSE,
            pdFALSE,
            xTicksToWait);
            
        if ((uxBits & GOT_IPV4_BIT) == GOT_IPV4_BIT) {
            return ESP_OK;
        } else {
            return ESP_ERR_TIMEOUT;
        }    
    } else {
        ESP_LOGE(TAG, "Connection wait: WiFi NOT INITIALIZED");
        return ESP_ERR_INVALID_STATE;
    }
}

/*!
 *  @brief Регистрация callback'а подключения
 */
esp_err_t WFc_ConnectCB_Register(wifi_callback_t callback) {
    if (callback == NULL) return ESP_ERR_INVALID_ARG;
    
    if (wifi_lock(1000) != ESP_OK) return ESP_ERR_TIMEOUT;
    
    esp_err_t ret = ESP_ERR_NO_MEM;
    if (uConnectCallbackCount < MAX_CONNECT_CB) {
        Connect_CB[uConnectCallbackCount++] = callback;
        ESP_LOGD(TAG, "Connect CB registered, total: %u", uConnectCallbackCount);
        ret = ESP_OK;
    } else {
        ESP_LOGW(TAG, "Could not register connect callback - array is full");
    }
    
    wifi_unlock();
    return ret;
}

/*!
 *  @brief Регистрация callback'а отключения
 */
esp_err_t WFc_DisconnectCB_Register(wifi_callback_t callback) {
    if (callback == NULL) return ESP_ERR_INVALID_ARG;
    
    if (wifi_lock(1000) != ESP_OK) return ESP_ERR_TIMEOUT;
    
    esp_err_t ret = ESP_ERR_NO_MEM;
    if (uDisConnectCallbackCount < MAX_DISCONNECT_CB) {
        Disconnect_CB[uDisConnectCallbackCount++] = callback;
        ESP_LOGD(TAG, "Disconnect CB registered, total: %u", uDisConnectCallbackCount);
        ret = ESP_OK;
    } else {
        ESP_LOGW(TAG, "Could not register disconnect callback - array is full");
    }
    
    wifi_unlock();
    return ret;
}

/*!
 *  @brief Принудительный сброс состояния повторных попыток
 */
// void WFc_ResetReconnectState(void) {
//     if (wifi_lock(1000) != ESP_OK) return;
    
//     ESP_LOGI(TAG, "Forcing reconnect state reset");
//     reset_reconnect_state();
    
//     wifi_unlock();
// }