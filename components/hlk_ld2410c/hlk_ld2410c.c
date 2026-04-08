#include "hlk_ld2410c.h"

#include <string.h>
#include <stdlib.h>

#include "esp_log.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "nvs_flash.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

static const char *TAG = "HLK_LD2410C";
static const uint16_t PRESENCE_ZONE_DISTANCE_CM = 375;
static const uint16_t INSIDE_ZONE_DISTANCE_CM = 250;
// static const uint16_t ZONE_HISTER_CM = 15;

static const uint8_t hlk_ble_auth_cmd[] = {
    0xFD, 0xFC, 0xFB, 0xFA,
    0x08, 0x00,
    0xA8, 0x00,
    0x48, 0x69, 0x4C, 0x69, 0x6E, 0x6B,
    0x04, 0x03, 0x02, 0x01
};
/* =========================================================
* STATE MACHINE DEFINITIONS
* ========================================================= */
typedef enum {
    STATE_OUTSIDE = 0,
    STATE_LEAVING_2,
    STATE_INTERMIDIATE,
    STATE_LEAVING,
    STATE_GO_INTERMIDIATE,
    STATE_INSIDE
} hlk_state_t;

static const char *state_to_string(hlk_state_t state)
{
    switch (state) {
    case STATE_OUTSIDE:
        return "OUTSIDE";
    case STATE_LEAVING_2:
        return "LEAVING_2";
    case STATE_INTERMIDIATE:
        return "INTERMIDIATE";
    case STATE_LEAVING:
        return "LEAVING";
    case STATE_GO_INTERMIDIATE:
        return "GO_INTERMIDIATE";
    case STATE_INSIDE:
        return "INSIDE";
    default:
        return "UNKNOWN";
    }
}

/* =========================================================
 * CONTEXT
 * ========================================================= */

typedef struct {
    esp_gatt_if_t gattc_if;
    uint16_t conn_id;
    esp_bd_addr_t device_addr;

    uint16_t service_start;
    uint16_t service_end;

    uint16_t tx_char_handle;
    uint16_t rx_char_handle;

    bool is_initialized;
    bool connected;
    hlk_config_t config;

    // State machine fields
    hlk_state_t current_state;
    int64_t state_entry_time_us;

    hlk_target_data_t latest_data;
    bool has_presence;
    bool absence_pending;
    int64_t absence_pending_since_us;
    SemaphoreHandle_t data_mutex;
    QueueHandle_t event_queue;
    TaskHandle_t ble_task_handle;

} hlk_context_t;

static hlk_context_t ctx;

// Очередь событий BLE
typedef struct {
    uint32_t event;
    esp_gatt_if_t gattc_if;
    void *data;
} ble_event_t;

/* =========================================================
 * FORWARD DECL
 * ========================================================= */

static void gap_cb(esp_gap_ble_cb_event_t event,
                   esp_ble_gap_cb_param_t *param);

static void gattc_cb(esp_gattc_cb_event_t event,
                     esp_gatt_if_t gattc_if,
                     esp_ble_gattc_cb_param_t *param);

/* =========================================================
* STATE MACHINE HELPERS
* ========================================================= */
static void transition_to_state(hlk_state_t new_state) {
    hlk_state_t old_state = ctx.current_state;
    const char *state_name = state_to_string(new_state);
    
    // Вызываем callback при переходе из OUTSIDE
    if (old_state == STATE_OUTSIDE && new_state != STATE_OUTSIDE) {
        if (ctx.config.presence_cb) {
            ctx.config.presence_cb(&ctx.latest_data);
        ctx.has_presence = true;
        }
    }
    
    // Вызываем callback при переходе в OUTSIDE
    if (new_state == STATE_OUTSIDE && old_state != STATE_OUTSIDE) {
        if (ctx.config.absence_cb) {
            ctx.config.absence_cb();
        }
        ctx.has_presence = false;
    }
    
    ctx.current_state = new_state;
    ctx.state_entry_time_us = esp_timer_get_time();

    if (ctx.config.state_change_cb != NULL) {
        ctx.config.state_change_cb(state_name);
    }
    
    ESP_LOGD(TAG, "State transition: %d -> %d (%s)", old_state, new_state, state_name);
}

static bool is_in_transition_zone(uint16_t dst) {
    return (dst >= (INSIDE_ZONE_DISTANCE_CM) && dst < (PRESENCE_ZONE_DISTANCE_CM));
}

static void parse_radar_data(const uint8_t *data, size_t length) {
    if (length < 13) return;
    
    // Проверяем заголовок пакета (F4 F3 F2 F1)
    if (data[0] != 0xF4 || data[1] != 0xF3 ||
        data[2] != 0xF2 || data[3] != 0xF1) {
        return;
    }
    
    // Длина данных (little-endian)
    uint16_t data_len = data[4] | (data[5] << 8);
    uint16_t cmd      = data[6];
    
    // Проверяем command word
    if (cmd != 0x0002) {
        ESP_LOGD(TAG,"Engineer mode");
        return;
    } else {
        ESP_LOGD(TAG,"Normal mode");
    }
    
    // Проверка длины
    if (data_len + 6 > length) {
        ESP_LOGW(TAG,"Wrong length");
        return;
    }
    
    // Парсим данные цели
    const uint8_t *p = &data[8];
    hlk_target_data_t new_data = {
        .target_state            = p[0],
        .moving_distance_cm      = p[1] | (p[2] << 8),
        .moving_energy           = p[3],
        .stationary_distance_cm  = p[4] | (p[5] << 8),
        .stationary_energy       = p[6],
        .detection_distance_cm   = p[7] | (p[8] << 8),
    };
    
    // dst = min(moving_distance_cm, stationary_distance_cm)
    uint16_t dst = new_data.moving_distance_cm;
    if (new_data.stationary_distance_cm < dst) {
        dst = new_data.stationary_distance_cm;
    }
    
    // target = 0 если target_state == 0
    bool target_exists = (new_data.target_state != 0);
    
    xSemaphoreTake(ctx.data_mutex, portMAX_DELAY);
    ctx.latest_data = new_data;
    
    // STATE MACHINE LOGIC
    int64_t now_us = esp_timer_get_time();
    int64_t state_duration_us = now_us - ctx.state_entry_time_us;
    
    switch (ctx.current_state) {
        case STATE_OUTSIDE:
            if (is_in_transition_zone(dst)) {
                transition_to_state(STATE_INTERMIDIATE);
            } else if (dst < INSIDE_ZONE_DISTANCE_CM) {
                transition_to_state(STATE_INSIDE);
            }
            break;
            
        case STATE_LEAVING_2:
            // if (!target_exists) {
            if(pdFALSE) {    
                transition_to_state(STATE_OUTSIDE);
            } else if (dst < INSIDE_ZONE_DISTANCE_CM) {
                transition_to_state(STATE_INSIDE);
            } else if (is_in_transition_zone(dst)) {
                transition_to_state(STATE_INTERMIDIATE);
            } else if (state_duration_us >= 3000000) { // 3 seconds
                transition_to_state(STATE_OUTSIDE);
            }
            break;
            
        case STATE_INTERMIDIATE:
            if (dst < INSIDE_ZONE_DISTANCE_CM) {
                transition_to_state(STATE_INSIDE);
            } else if (dst > PRESENCE_ZONE_DISTANCE_CM || !target_exists) {
                transition_to_state(STATE_LEAVING_2);
            }
            break;
            
        case STATE_GO_INTERMIDIATE:
            if (dst < INSIDE_ZONE_DISTANCE_CM) {
                transition_to_state(STATE_INSIDE);
            } else if (state_duration_us >= 1000000) { // 3 seconds
                transition_to_state(STATE_INTERMIDIATE);
            }
            break;
            
        case STATE_INSIDE:
            if (dst > PRESENCE_ZONE_DISTANCE_CM || !target_exists) {
                transition_to_state(STATE_LEAVING);
            } else if (is_in_transition_zone(dst)) {
                transition_to_state(STATE_GO_INTERMIDIATE);
            }
            break;
            
        case STATE_LEAVING:
            if (dst < INSIDE_ZONE_DISTANCE_CM) {
                transition_to_state(STATE_INSIDE);
            } else if (is_in_transition_zone(dst)) {
                transition_to_state(STATE_GO_INTERMIDIATE);
            } else if (state_duration_us >= 150000000) { // 5 minutes = 300 seconds
                transition_to_state(STATE_OUTSIDE);
            }
            break;
    }
    
    xSemaphoreGive(ctx.data_mutex);
    
    ESP_LOGD(TAG,
        "Target state: 0x%02X, dst=%ucm, State=%d",
        new_data.target_state,
        dst,
        ctx.current_state);
        
    ESP_LOGV(TAG,
        "State=%u MD=%ucm ME=%u SD=%ucm SE=%u DD=%ucm",
        new_data.target_state,
        new_data.moving_distance_cm,
        new_data.moving_energy,
        new_data.stationary_distance_cm,
        new_data.stationary_energy,
        new_data.detection_distance_cm
    );
}

/* =========================================================
 * GAP CALLBACK
 * ========================================================= */

static void gap_cb(esp_gap_ble_cb_event_t event,
                   esp_ble_gap_cb_param_t *param)
{
    if (event != ESP_GAP_BLE_SCAN_RESULT_EVT) {
        return;
    }

    if (param->scan_rst.search_evt != ESP_GAP_SEARCH_INQ_RES_EVT) {
        return;
    }

    uint8_t name_len = 0;
    uint8_t *name_ptr = esp_ble_resolve_adv_data(
        param->scan_rst.ble_adv,
        ESP_BLE_AD_TYPE_NAME_CMPL,
        &name_len
    );

    if (!name_ptr || name_len == 0) {
        return;
    }

    char name[32];
    if (name_len >= sizeof(name)) {
        return;
    }

    memcpy(name, name_ptr, name_len);
    name[name_len] = '\0';

    if (strncmp(name,
                ctx.config.device_name_prefix,
                strlen(ctx.config.device_name_prefix)) != 0) {
        return;
    }

    ESP_LOGI(TAG, "Found device: %s", name);

    esp_ble_gap_stop_scanning();
    memcpy(ctx.device_addr,
           param->scan_rst.bda,
           ESP_BD_ADDR_LEN);

    esp_ble_gattc_open(
        ctx.gattc_if,
        ctx.device_addr,
        param->scan_rst.ble_addr_type,
        true
    );
}

/* =========================================================
 * GATTC CALLBACK
 * ========================================================= */

static void gattc_cb(esp_gattc_cb_event_t event,
                     esp_gatt_if_t gattc_if,
                     esp_ble_gattc_cb_param_t *param)
{
    switch (event) {

    case ESP_GATTC_REG_EVT:
        ESP_LOGI(TAG, "GATTC registered");
        ctx.gattc_if = gattc_if;
        break;

    case ESP_GATTC_CONNECT_EVT:
        ESP_LOGI(TAG, "Connected");
        ctx.conn_id = param->connect.conn_id;
        ctx.gattc_if = gattc_if;
        ctx.connected = true;

        memcpy(ctx.device_addr,
            param->connect.remote_bda,
            sizeof(esp_bd_addr_t));

        ESP_LOGI(TAG, "Connected to %02X:%02X:%02X:%02X:%02X:%02X",
                ctx.device_addr[0], ctx.device_addr[1], ctx.device_addr[2],
                ctx.device_addr[3], ctx.device_addr[4], ctx.device_addr[5]);

        if (ctx.config.connection_cb) {
            ctx.config.connection_cb(true);
        }

        esp_ble_gattc_search_service(
            gattc_if,
            ctx.conn_id,
            NULL
        );
        break;

    case ESP_GATTC_SEARCH_RES_EVT:
        if (param->search_res.srvc_id.uuid.len == ESP_UUID_LEN_16 &&
            param->search_res.srvc_id.uuid.uuid.uuid16 == HLK_SERVICE_UUID) {

            ctx.service_start = param->search_res.start_handle;
            ctx.service_end   = param->search_res.end_handle;

            ESP_LOGI(TAG,
                     "HLK service found [0x%04X - 0x%04X]",
                     ctx.service_start,
                     ctx.service_end);
        }
        break;

    case ESP_GATTC_SEARCH_CMPL_EVT: {
        uint16_t count = 0;

        esp_ble_gattc_get_attr_count(
            gattc_if,
            ctx.conn_id,
            ESP_GATT_DB_CHARACTERISTIC,
            ctx.service_start,
            ctx.service_end,
            0,
            &count
        );

        if (count == 0) {
            ESP_LOGE(TAG, "No characteristics found");
            break;
        }

        esp_gattc_char_elem_t *chars =
            malloc(sizeof(esp_gattc_char_elem_t) * count);

        if (!chars) {
            ESP_LOGE(TAG, "No memory for char list");
            break;
        }

        esp_ble_gattc_get_all_char(
            gattc_if,
            ctx.conn_id,
            ctx.service_start,
            ctx.service_end,
            chars,
            &count,
            0
        );

        for (int i = 0; i < count; i++) {
            if (chars[i].uuid.len == ESP_UUID_LEN_16) {
                uint16_t uuid = chars[i].uuid.uuid.uuid16;

                if (uuid == HLK_TX_CHAR_UUID) {
                    ctx.tx_char_handle = chars[i].char_handle;
                } else if (uuid == HLK_RX_CHAR_UUID) {
                    ctx.rx_char_handle = chars[i].char_handle;
                }
            }
        }

        free(chars);

        ESP_LOGI(TAG,
                 "Characteristics ready TX=0x%04X RX=0x%04X",
                 ctx.tx_char_handle,
                 ctx.rx_char_handle);
        /* Регистрируемся на notify TX */
        esp_err_t err = esp_ble_gattc_register_for_notify(
            gattc_if,
            ctx.device_addr,
            ctx.tx_char_handle
        );

        ESP_LOGI(TAG, "register_for_notify: %s", esp_err_to_name(err));
        break;
    }

    case ESP_GATTC_REG_FOR_NOTIFY_EVT: {
        ESP_LOGI(TAG, "REG_FOR_NOTIFY_EVT status=%d handle=0x%04X",
                param->reg_for_notify.status,
                param->reg_for_notify.handle);

        if (param->reg_for_notify.status != ESP_GATT_OK) {
            ESP_LOGE(TAG, "Notify register failed");
            break;
        }

        uint16_t notify_en = 0x0001; // notifications ON

        esp_ble_gattc_write_char_descr(
            gattc_if,
            ctx.conn_id,
            param->reg_for_notify.handle + 1, // CCCD
            sizeof(notify_en),
            (uint8_t *)&notify_en,
            ESP_GATT_WRITE_TYPE_RSP,
            ESP_GATT_AUTH_REQ_NONE
        );
        break;
    }

    case ESP_GATTC_WRITE_DESCR_EVT:
        ESP_LOGI(TAG, "CCCD written, sending password");

        esp_ble_gattc_write_char(
            gattc_if,
            ctx.conn_id,
            ctx.rx_char_handle,   // FFF2
            sizeof(hlk_ble_auth_cmd),
            hlk_ble_auth_cmd,
            ESP_GATT_WRITE_TYPE_NO_RSP,
            ESP_GATT_AUTH_REQ_NONE
        );
        break;

    case ESP_GATTC_NOTIFY_EVT:
        // ESP_LOGI(TAG, "NOTIFY len=%d", param->notify.value_len);
        // ESP_LOG_BUFFER_HEX(TAG,
        //                 param->notify.value,
        //                 param->notify.value_len);
        // Получены данные с радара
        if (param->notify.handle == ctx.tx_char_handle) {
            parse_radar_data(param->notify.value,
                            param->notify.value_len);
        }
        break;

    case ESP_GATTC_DISCONNECT_EVT:
        ESP_LOGW(TAG, "Disconnected");
        ctx.connected = false;

        if (ctx.config.connection_cb) {
            ctx.config.connection_cb(false);
        }

        if (ctx.config.auto_reconnect) {
            esp_ble_gap_start_scanning(ctx.config.ble_scan_timeout);
        }
        break;

    default:
        break;
    }
}

// Задача обработки BLE событий
static void ble_task(void *arg) {
    ble_event_t evt;
    ESP_LOGI(TAG,"\tble_task initialized");
    while (1) {
        if (xQueueReceive(ctx.event_queue, &evt, portMAX_DELAY)) {
            // Обработка GAP событий
            if (evt.event < 1000) {
                esp_gap_ble_cb_event_t event = evt.event;
                esp_ble_gap_cb_param_t *param = evt.data;
                if (event == ESP_GAP_BLE_SCAN_RESULT_EVT) {
                    // ESP_LOGI(TAG,"\tble_task\tESP_GAP_BLE_SCAN_RESULT_EVT");
                    if (param->scan_rst.search_evt == ESP_GAP_SEARCH_INQ_RES_EVT) {
                        uint8_t name_len = 0;
                        uint8_t *name_ptr = esp_ble_resolve_adv_data(
                            param->scan_rst.ble_adv,
                            ESP_BLE_AD_TYPE_NAME_CMPL,
                            &name_len
                        );

                        if (!name_ptr || name_len == 0) {
                            continue;
                        }

                        char name[32];
                        if (name_len >= sizeof(name)) {
                            continue;
                        }

                        memcpy(name, name_ptr, name_len);
                        name[name_len] = '\0';

                        if (strncmp(name,
                                    ctx.config.device_name_prefix,
                                    strlen(ctx.config.device_name_prefix)) != 0) {
                            continue;
                        }

                        ESP_LOGI(TAG, "Found device: %s", name);

                        esp_ble_gap_stop_scanning();
                        memcpy(ctx.device_addr,
                            param->scan_rst.bda,
                            ESP_BD_ADDR_LEN);

                        esp_ble_gattc_open(
                            ctx.gattc_if,
                            ctx.device_addr,
                            param->scan_rst.ble_addr_type,
                            true
                        );
                        
                    }
                }

                
                // switch (event) {
                //     case ESP_GAP_BLE_SCAN_RESULT_EVT:
                //         if (param->scan_rst.search_evt == 
                //             ESP_GAP_SEARCH_INQ_RES_EVT) {
                //             // Проверяем имя устройства
                //             char device_name[32] = {0};
                //             if (param->scan_rst.ble_adv_len > 0) {
                //                 // Ищем имя в AD данных
                //                 uint8_t *adv_data = param->scan_rst.ble_adv;
                //                 uint8_t adv_len = param->scan_rst.ble_adv_len;
                                
                //                 uint8_t *p = adv_data;
                //                 while (p < adv_data + adv_len) {
                //                     uint8_t length = *p++;
                //                     if (length == 0) break;
                                    
                //                     uint8_t type = *p++;
                //                     if (type == ESP_BLE_AD_TYPE_NAME_CMPL || 
                //                         type == ESP_BLE_AD_TYPE_NAME_SHORT) {
                //                         memcpy(device_name, p, length - 1);
                //                         device_name[length - 1] = '\0';
                //                         break;
                //                     }
                //                     p += length - 1;
                //                 }
                //             }
                            
                //             // Проверяем префикс имени
                //             if (strstr(device_name, ctx.config.device_name_prefix)) {
                //                 ESP_LOGI(TAG, "Found device: %s", device_name);
                //                 memcpy(ctx.device_addr, 
                //                        param->scan_rst.bda, 
                //                        sizeof(esp_bd_addr_t));
                                
                //                 // Останавливаем сканирование и подключаемся
                //                 esp_ble_gap_stop_scanning();
                //                 esp_ble_gattc_open(ctx.gattc_if, 
                //                                    ctx.device_addr, 
                //                                    true);
                //             }
                //         }
                //         break;
                        
                //     case ESP_GAP_BLE_SCAN_PARAM_SET_COMPLETE_EVT:
                //         ESP_LOGI(TAG, "Scan parameters set");
                //         break;
                        
                //     default:
                //         break;
                // }
            }
            // Обработка GATT событий
            else {
                esp_gattc_cb_event_t event = evt.event - 1000;
                esp_ble_gattc_cb_param_t *param = evt.data;
                
                switch (event) {
                case ESP_GATTC_REG_EVT:
                    ESP_LOGI(TAG, "GATTC registered");
                    ctx.gattc_if = evt.gattc_if;
                    break;

                case ESP_GATTC_CONNECT_EVT:
                    ESP_LOGI(TAG, "Connected");
                    ctx.conn_id = param->connect.conn_id;
                    ctx.connected = true;

                    if (ctx.config.connection_cb) {
                        ctx.config.connection_cb(true);
                    }

                    esp_ble_gattc_search_service(
                        evt.gattc_if,
                        ctx.conn_id,
                        NULL
                    );
                    break;

                case ESP_GATTC_SEARCH_RES_EVT:
                    ESP_LOGI(TAG,"ESP_GATTC_SEARCH_RES_EVT");
                    if (param->search_res.srvc_id.uuid.len == ESP_UUID_LEN_16 &&
                        param->search_res.srvc_id.uuid.uuid.uuid16 == HLK_SERVICE_UUID) {

                        ctx.service_start = param->search_res.start_handle;
                        ctx.service_end   = param->search_res.end_handle;

                        ESP_LOGI(TAG,
                                "HLK service found [0x%04X - 0x%04X]",
                                ctx.service_start,
                                ctx.service_end);
                    }
                    break;

                case ESP_GATTC_SEARCH_CMPL_EVT: {
                    ESP_LOGI(TAG,"ESP_GATTC_SEARCH_CMPL_EVT");
                    uint16_t count = 0;

                    esp_ble_gattc_get_attr_count(
                        evt.gattc_if,
                        ctx.conn_id,
                        ESP_GATT_DB_CHARACTERISTIC,
                        ctx.service_start,
                        ctx.service_end,
                        0,
                        &count
                    );

                    if (count == 0) {
                        ESP_LOGE(TAG, "No characteristics found");
                        break;
                    }

                    esp_gattc_char_elem_t *chars =
                        malloc(sizeof(esp_gattc_char_elem_t) * count);

                    if (!chars) {
                        ESP_LOGE(TAG, "No memory for char list");
                        break;
                    }

                    esp_ble_gattc_get_all_char(
                        evt.gattc_if,
                        ctx.conn_id,
                        ctx.service_start,
                        ctx.service_end,
                        chars,
                        &count,
                        0
                    );

                    for (int i = 0; i < count; i++) {
                        if (chars[i].uuid.len == ESP_UUID_LEN_16) {
                            uint16_t uuid = chars[i].uuid.uuid.uuid16;

                            if (uuid == HLK_TX_CHAR_UUID) {
                                ctx.tx_char_handle = chars[i].char_handle;
                            } else if (uuid == HLK_RX_CHAR_UUID) {
                                ctx.rx_char_handle = chars[i].char_handle;
                            }
                        }
                    }

                    free(chars);

                    ESP_LOGI(TAG,
                            "Characteristics ready TX=0x%04X RX=0x%04X",
                            ctx.tx_char_handle,
                            ctx.rx_char_handle);
                    break;
                }

                case ESP_GATTC_DISCONNECT_EVT:
                    ESP_LOGW(TAG, "Disconnected");
                    ctx.connected = false;

                    if (ctx.config.connection_cb) {
                        ctx.config.connection_cb(false);
                    }

                    if (ctx.config.auto_reconnect) {
                        esp_ble_gap_start_scanning(ctx.config.ble_scan_timeout);
                    }
                    break;

                default:
                    break;
                }
                
            }
        }
    }
}

/* =========================================================
 * PUBLIC API
 * ========================================================= */

esp_err_t hlk_ld2410c_init(const hlk_config_t *config)
{
    if (ctx.is_initialized) {
        return ESP_FAIL;
    }

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    ctx = (hlk_context_t){0};
    ctx.config = *config;
    ctx.current_state = STATE_OUTSIDE;
    ctx.state_entry_time_us = esp_timer_get_time();
    ESP_ERROR_CHECK(
        esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT));

    esp_bt_controller_config_t bt_cfg =
        BT_CONTROLLER_INIT_CONFIG_DEFAULT();

    ESP_ERROR_CHECK(esp_bt_controller_init(&bt_cfg));
    ESP_ERROR_CHECK(esp_bt_controller_enable(ESP_BT_MODE_BLE));

    ESP_ERROR_CHECK(esp_bluedroid_init());
    ESP_ERROR_CHECK(esp_bluedroid_enable());

    ESP_ERROR_CHECK(esp_ble_gap_register_callback(gap_cb));
    ESP_ERROR_CHECK(esp_ble_gattc_register_callback(gattc_cb));
    ESP_ERROR_CHECK(esp_ble_gattc_app_register(0));

    // Создание очереди и мьютекса
    ctx.event_queue = xQueueCreate(10, sizeof(ble_event_t));
    ctx.data_mutex = xSemaphoreCreateMutex();
    
    if (!ctx.event_queue || !ctx.data_mutex) {
        return ESP_FAIL;
    }
    
    // Запуск задачи обработки BLE
    xTaskCreate(ble_task, "ble_task", 4096, NULL, 5, &ctx.ble_task_handle);
    
    ctx.is_initialized = true;
    ESP_LOGI(TAG, "HLK-LD2410C module initialized");

    return ESP_OK;
}

esp_err_t hlk_ld2410c_start(void)
{
    esp_ble_gap_start_scanning(ctx.config.ble_scan_timeout);
    return ESP_OK;
}

void hlk_ld2410c_stop(void)
{
    if (ctx.connected) {
        esp_ble_gattc_close(ctx.gattc_if, ctx.conn_id);
        ctx.connected = false;
    }
}

bool hlk_ld2410c_is_connected(void)
{
    return ctx.connected;
}

bool hlk_ld2410c_get_latest_data(hlk_target_data_t *data)
{
    if (!ctx.connected || !data) {
        return false;
    }
    
    xSemaphoreTake(ctx.data_mutex, portMAX_DELAY);
    memcpy(data, &ctx.latest_data, sizeof(hlk_target_data_t));
    xSemaphoreGive(ctx.data_mutex);
    // (void) data;
    return true;
}

bool hlk_ld2410c_get_presence_state(hlk_target_data_t *data, bool *has_presence)
{
    if (!ctx.is_initialized || !data || !has_presence) {
        return false;
    }

    xSemaphoreTake(ctx.data_mutex, portMAX_DELAY);
    memcpy(data, &ctx.latest_data, sizeof(hlk_target_data_t));
    *has_presence = ctx.has_presence;
    xSemaphoreGive(ctx.data_mutex);

    return true;
}

esp_err_t hlk_ld2410c_set_password(const uint8_t password[6])
{
    if (!ctx.connected || ctx.rx_char_handle == 0) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t pwd[6];
    memcpy(pwd, password, 6);

    return esp_ble_gattc_write_char(
        ctx.gattc_if,
        ctx.conn_id,
        ctx.rx_char_handle,
        6,
        pwd,
        ESP_GATT_WRITE_TYPE_RSP,
        ESP_GATT_AUTH_REQ_NONE
    );
}
