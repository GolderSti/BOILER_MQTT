/*
 * SPDX-FileCopyrightText: 2023 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */
/* WiFi Roaming App Example

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
   dirty commit version + dirty
   try new v1.3.1-1-g44b9d77-dirty
*/
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_chip_info.h"
#include "nvs_flash.h"

#include "wifi_common.h"
#include "version.h"
#include "esp_ota_ops.h"
#include "ota.h"
#include "mqtt_common.h"
#include "hlk_ld2410c.h"
#include "relay_control.h"
#include "btn_common.h"

const char *TAG = "MAIN";

// Конфигурация Wi-Fi
#define WIFI_SSID "Sti-WiFi"
#define WIFI_PASS "6k8wSbcN"
#define WIFI_WAIT_BEFOR_OTA 30000 //wait for wifi befor OTA to reise an error

// Конфигурация OTA
static const ota_config_t ota_config = {
    .server_url = "http://esp-update.lan:8080",
    .firmware_path = "/api/firmware/download?app=",
    .version_path = "/api/firmware/version?app=",
    .sha256_path = "/api/firmware/sha256?app=",
    .check_interval_ms = 300000, // 5 минут
    .check_on_boot = true,
    .max_retries = 3,
};

//переменные для управления светом
#define MAIN_LIGHT_RELAY 0
#define MIRROR_LIGHT_RELAY 1
#define FAN_RELAY 2
static bool bLightState;
#define MAIN_BTN_PIN GPIO_NUM_1
#define MIRROR_BTN_PIN GPIO_NUM_2

/* =========================================================
 * Общая информация о системе
 * ========================================================= */

// Функция для вывода информации о системе
static void print_system_info(void) {
    ESP_LOGI(TAG, "====================================");
    ESP_LOGI(TAG, "OTA Example Application");
    ESP_LOGI(TAG, "Версия прошивки: %s", version_get());
    ESP_LOGI(TAG, "IDF Version: %s", esp_get_idf_version());
    
    // Информация о flash
    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);
    ESP_LOGI(TAG, "Чип: %s Rev %d", 
             CONFIG_IDF_TARGET, chip_info.revision);
    ESP_LOGI(TAG, "Ядер CPU: %d", chip_info.cores);
    ESP_LOGI(TAG, "Wi-Fi: %s", 
             (chip_info.features & CHIP_FEATURE_WIFI_BGN) ? "Yes" : "No");
    
    // Информация о памяти
    ESP_LOGI(TAG, "Свободная память: %" PRIu32 " bytes", 
             esp_get_free_heap_size());
    ESP_LOGI(TAG, "Минимальная свободная память: %" PRIu32 " bytes", 
             esp_get_minimum_free_heap_size());
    
    // Информация о текущей прошивке
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (running) {
        ESP_LOGI(TAG, "Текущий раздел: %s (0x%x)", 
                 running->label, running->address);
    }
    
    ESP_LOGI(TAG, "====================================");
}

/* =========================================================
 * ОТА дополнительные функции
 * ========================================================= */

 void check_all_versions(void) {
    // 1. Из заголовка прошивки (то, что видит bootloader)
    const esp_app_desc_t *app_desc = esp_app_get_description();
    
    // 2. Из нашего компонента
    const char* our_version = version_get();
    
    // 3. Из раздела (если нужно)
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_app_desc_t partition_desc;
    esp_ota_get_partition_description(running, &partition_desc);
    
    ESP_LOGV(TAG, "=== Version Check ===");
    ESP_LOGV(TAG, "1. App header version: %s", app_desc->version);
    ESP_LOGV(TAG, "2. Component version: %s", our_version);
    ESP_LOGV(TAG, "3. Partition version: %s", partition_desc.version);
    ESP_LOGV(TAG, "4. Project name: %s", app_desc->project_name);
    ESP_LOGV(TAG, "5. Compile time: %s %s", app_desc->date, app_desc->time);
    
    // Проверяем совпадение
    bool all_match = (strcmp(app_desc->version, our_version) == 0) &&
                     (strcmp(app_desc->version, partition_desc.version) == 0);
    
    if (all_match) {
        ESP_LOGV(TAG, "✓ All versions match!");
    } else {
        ESP_LOGW(TAG, "✗ Version mismatch detected!");
    }
}

//esp idf rollback functions
static void check_and_validate_ota(bool bFW_Not_Valid)
{    
    const esp_partition_t *running =
        esp_ota_get_running_partition();

    esp_ota_img_states_t state;
    esp_err_t err =
        esp_ota_get_state_partition(running, &state);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get OTA state");
        // esp_ota_mark_app_invalid_rollback_and_reboot();
        return;
    }
    //show partition OTA state
    switch (state)
    {
    case ESP_OTA_IMG_NEW:
        ESP_LOGI(TAG, "ESP_OTA_IMG_NEW");
        break;
    case ESP_OTA_IMG_VALID:
        ESP_LOGI(TAG, "ESP_OTA_IMG_VALID");
        break;
    case ESP_OTA_IMG_INVALID:
        ESP_LOGI(TAG, "ESP_OTA_IMG_INVALID");
        break;
    case ESP_OTA_IMG_ABORTED:
        ESP_LOGI(TAG, "ESP_OTA_IMG_ABORTED");
        break;
    case ESP_OTA_IMG_UNDEFINED:
        ESP_LOGI(TAG, "ESP_OTA_IMG_UNDEFINED");
        break;
    
    default:
        break;
    }

    if (state == ESP_OTA_IMG_PENDING_VERIFY) {
        ESP_LOGW(TAG, "OTA image pending verification");
        if (!bFW_Not_Valid) {
            ESP_LOGI(TAG, "Marking OTA image as valid");
            esp_ota_mark_app_valid_cancel_rollback();
        }
        else {
            ESP_LOGE(TAG, "OTA validation failed, rolling back");
            vTaskDelay(pdMS_TO_TICKS(3000));
            esp_ota_mark_app_invalid_rollback_and_reboot();
        }
    }
}

/* =========================================================
 * Управление освещением
 * ========================================================= */
void Switch_Light(){
    if (Relay_GetLightState()==ALL_OFF)
    {
        Relay_Light_On();
        ESP_LOGD(TAG,"Switch light -> ON");
    }else
    {
        Relay_Light_Off();
        ESP_LOGD(TAG,"Switch light -> OFF");
    }    
}
/* =========================================================
 * Датчик HLK-LD2410C
 * ========================================================= */

// Callback-функция при обнаружении присутствия
static void on_presence(const hlk_target_data_t *data) {
    ESP_LOGI(TAG, "Presence detected!");
    ESP_LOGI(TAG, "Target state: 0x%02X", data->target_state);
    ESP_LOGI(TAG, "Moving distance: %d cm", data->moving_distance_cm);
    ESP_LOGI(TAG, "Moving energy: %d", data->moving_energy);
    
    Relay_Light_On();
}

// Callback-функция при отсутствии
static void on_absence(void) {
    ESP_LOGI(TAG, "No presence detected");

    Relay_Light_Off();
}

// Callback-функция изменения статуса подключения
static void on_connection(bool connected) {
    if (connected) {
        ESP_LOGI(TAG, "Connected to radar");
    } else {
        ESP_LOGI(TAG, "Disconnected from radar");
    }
}

// Callback-функция ошибок
static void on_error(const char *error) {
    ESP_LOGE(TAG, "Error: %s", error);
}

/* =========================================================
 * Связь реле и кнопок
 * ========================================================= */
void main_switch_change(){
    Switch_Light();
}

void mirror_switch_change(){
    Relay_Fan_On();
}

void button_callback(uint8_t gpio_num, uint8_t event) {
    static const char* event_names[] = {
        "PRESSED", "RELEASED", "CLICK", "LONG_PRESS", "DOUBLE_CLICK"
    };
    
    ESP_LOGI(TAG, "GPIO %d: %s", gpio_num, 
             event < BTN_EVENT_MAX ? event_names[event] : "UNKNOWN");

    if (gpio_num==MAIN_BTN_PIN && (event == BTN_EVENT_PRESSED || event == BTN_EVENT_RELEASED))
    {
        if (event == BTN_EVENT_PRESSED)
        {
            Relay_Light_On();
        }
        if (event == BTN_EVENT_RELEASED)
        {
            Relay_Light_Off();
        }
        
        
    }
    if (gpio_num==MIRROR_BTN_PIN && (event == BTN_EVENT_PRESSED || event == BTN_EVENT_RELEASED))
    {
        mirror_switch_change();
    }    
}

void app_main(void)
{
    ESP_LOGI(TAG, "[APP] Startup..");
    ESP_LOGI(TAG, "[APP] Free memory: %" PRIu32 " bytes", esp_get_free_heap_size());

    esp_log_level_set("*", ESP_LOG_WARN);
    esp_log_level_set("OTA", ESP_LOG_VERBOSE);
    esp_log_level_set("HLK_LD2410C", ESP_LOG_VERBOSE);
    esp_log_level_set("MAIN", ESP_LOG_VERBOSE);
    esp_log_level_set("RLYCNTR", ESP_LOG_VERBOSE);
    esp_log_level_set("RLY_TSK", ESP_LOG_VERBOSE);
    esp_log_level_set("RLYAUTO", ESP_LOG_VERBOSE);
    esp_log_level_set("MQTTCMN", ESP_LOG_INFO);
    esp_log_level_set("BTN", ESP_LOG_VERBOSE);
    
    
    

    bool bOTA_Firmware_not_valid = pdTRUE;
    //Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    if (ret!=ESP_OK)
    {
        bOTA_Firmware_not_valid=pdTRUE;
    }
    
    // Подключение к Wi-Fi
    ESP_LOGI(TAG, "Подключаюсь к Wi-Fi");
    
    // Используем стандартный пример для подключения Wi-Fi

    WFc_Init();
    WFc_Start();
    
    // Выводим информацию о системе
    print_system_info();
    ret = WFc_ConnectionWait(pdMS_TO_TICKS(WIFI_WAIT_BEFOR_OTA));
    if (ret!=ESP_OK)
    {
        bOTA_Firmware_not_valid=pdTRUE;
    }
    
    // Инициализация OTA
    ESP_LOGI(TAG, "Инициализация OTA...");
    // Копируем дефолтный конфиг
    ota_config_t config = ota_config;

    // Заполняем имя приложения
    const esp_app_desc_t *app_desc = esp_app_get_description();
    strncpy(config.app_name, app_desc->project_name, sizeof(config.app_name) - 1);
    config.app_name[sizeof(config.app_name) - 1] = '\0';
    ret = ota_init(&config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Ошибка инициализации OTA: %s", esp_err_to_name(ret));
        bOTA_Firmware_not_valid=pdTRUE;
    }
    check_all_versions();

    // Запускаем фоновую проверку обновлений
    ESP_LOGI(TAG, "Запуск службы проверки обновлений...");
    ota_start_background_check();

    mqtt_init();

    ESP_LOGI(TAG, "Starting HLK-LD2410C BLE Radar");
    
    // Конфигурация модуля
    hlk_config_t hlk_config = {
        .device_name_prefix = "HLK-LD2410_9A95",
        .ble_scan_timeout = 30, // секунды
        .auto_reconnect = true,
        .presence_cb = on_presence,
        .absence_cb = on_absence,
        .connection_cb = on_connection,
        .error_cb = on_error
    };
    
    // Инициализация модуля
    ret = hlk_ld2410c_init(&hlk_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Ошибка инициализации OTA: %s", esp_err_to_name(ret));
        bOTA_Firmware_not_valid=pdTRUE;
    }
    
    // Запуск поиска устройства
    ret = hlk_ld2410c_start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Ошибка инициализации OTA: %s", esp_err_to_name(ret));
        bOTA_Firmware_not_valid=pdTRUE;
    }

    //ждём отработки задачи проверки ОТА
    vTaskDelay(pdMS_TO_TICKS(100));
    ota_status_t status = ota_get_status();

    while ((status==OTA_STATUS_CHECKING)||(status==OTA_STATUS_IDLE))
    {
        vTaskDelay(pdMS_TO_TICKS(100));
        status = ota_get_status();
    }
    if ((status!=OTA_STATUS_FAILED)&&(status!=OTA_STATUS_VERSION_ERROR))
    {
        bOTA_Firmware_not_valid=pdFALSE;
    }
    //
    ESP_LOGI(TAG,"Starting Relay Module");
    Relay_Control_Init();
    bLightState = pdFALSE;
    Relay_Light_Off();

    ESP_LOGI(TAG,"Starting Buttons");
    // Инициализация модуля кнопок
    button_init();
    // Регистрация кнопки с настройками по умолчанию
    button_config_t default_cfg = BUTTON_CONFIG_DEFAULT();
    button_register(MAIN_BTN_PIN, &default_cfg, button_callback);
    button_register(MIRROR_BTN_PIN, &default_cfg, button_callback);
    //Подключились к серверу и проверили наличие прошивки там. Если связи с сервером нет, а мы только что обновились - значит что-то не так с кодом -> откатимся на старую прошивку
    ESP_LOGI(TAG,"Validating OTA");
    check_and_validate_ota(bOTA_Firmware_not_valid); 
    // Основной цикл приложения
    ESP_LOGI(TAG, "Приложение инициализировано.");
}
