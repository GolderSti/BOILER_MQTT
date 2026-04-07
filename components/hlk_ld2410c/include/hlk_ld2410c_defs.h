#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_gap_ble_api.h"
#include "esp_gattc_api.h"

// UUID сервисов и характеристик HLK-LD2410C
#define HLK_SERVICE_UUID        0xFFF0
#define HLK_TX_CHAR_UUID        0xFFF1  // Module -> APP
#define HLK_RX_CHAR_UUID        0xFFF2  // APP -> Module

// Структура данных о цели
typedef struct {
    uint8_t target_state;           // 0x00-0x06
    uint16_t moving_distance_cm;    // расстояние движущейся цели
    uint8_t moving_energy;          // энергия движущейся цели
    uint16_t stationary_distance_cm;// расстояние статичной цели
    uint8_t stationary_energy;      // энергия статичной цели
    uint16_t detection_distance_cm; // расстояние обнаружения
} hlk_target_data_t;

// Callback-функции
typedef void (*hlk_presence_callback_t)(const hlk_target_data_t *data);
typedef void (*hlk_absence_callback_t)(void);
typedef void (*hlk_connection_callback_t)(bool connected);
typedef void (*hlk_error_callback_t)(const char *error);
typedef void (*hlk_state_change_callback_t)(const char *state_name);

// Конфигурация модуля
typedef struct {
    char device_name_prefix[20];    // Префикс имени устройства ("HLK-LD2410B_")
    uint8_t ble_scan_timeout;       // Таймаут сканирования в секундах
    bool auto_reconnect;            // Автоподключение при разрыве
    hlk_presence_callback_t presence_cb;
    hlk_absence_callback_t absence_cb;
    hlk_connection_callback_t connection_cb;
    hlk_error_callback_t error_cb;
    hlk_state_change_callback_t state_change_cb;
} hlk_config_t;
