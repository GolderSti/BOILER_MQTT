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
*/
#include <string.h>
#include "esp_system.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include "wifi_common.h"
#include "mqtt_common.h"
#include "hlk_ld2410c.h"

const char *TAG = "wifi roaming app";

// Callback-функция при обнаружении присутствия
static void on_presence(const hlk_target_data_t *data) {
    ESP_LOGI(TAG, "Presence detected!");
    ESP_LOGI(TAG, "Target state: 0x%02X", data->target_state);
    ESP_LOGI(TAG, "Moving distance: %d cm", data->moving_distance_cm);
    ESP_LOGI(TAG, "Moving energy: %d", data->moving_energy);
    
    // Здесь можно управлять реле, светом и т.д.
    mqtt_Message_Publish_TAG("/espBath/LIGHT","ON",pdFALSE);
}

// Callback-функция при отсутствии
static void on_absence(void) {
    ESP_LOGI(TAG, "No presence detected");
    
    // Выключение света, реле и т.д.
    mqtt_Message_Publish_TAG("/espBath/LIGHT","OFF",pdFALSE);
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


void app_main(void)
{
    //Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_LOGI(TAG, "ESP_WIFI_MODE_STA");
    WFc_Init();
    WFc_Start();
    mqtt_init();

    ESP_LOGI(TAG, "Starting HLK-LD2410C BLE Radar Demo");
    
    // Конфигурация модуля
    hlk_config_t config = {
        .device_name_prefix = "HLK-LD2410_9A95",
        .ble_scan_timeout = 30, // секунды
        .auto_reconnect = true,
        .presence_cb = on_presence,
        .absence_cb = on_absence,
        .connection_cb = on_connection,
        .error_cb = on_error
    };
    
    // Инициализация модуля
    ret = hlk_ld2410c_init(&config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize HLK module");
        return;
    }
    
    // Запуск поиска устройства
    ret = hlk_ld2410c_start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HLK module");
        return;
    }
}
