#ifndef WIFI_COMMON_H
#define WIFI_COMMON_H

#include <stdint.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_err.h"
#include "esp_wifi.h"

// Конфигурация из Kconfig
#define EXAMPLE_WIFI_SSID CONFIG_ESP_WIFI_SSID
#define EXAMPLE_WIFI_PASSWORD CONFIG_ESP_WIFI_PASSWORD
// #define EXAMPLE_WIFI_RSSI_THRESHOLD CONFIG_EXAMPLE_WIFI_RSSI_THRESHOLD

// Битовая маска событий
#define WIFI_NEEDED BIT(1)
#define GOT_IPV4_BIT BIT(2)
#define WIFI_ROAMING_IN_PROGRESS BIT(3)

// Конфигурация повторных попыток
#define WIFI_RECONNECT_MAX_RETRIES 5
#define WIFI_RECONNECT_BASE_DELAY_MS 1000
#define WIFI_RECONNECT_MAX_DELAY_MS 30000
#define WIFI_RECONNECT_MULTIPLIER 2

// Конфигурация роуминга
#define WIFI_ROAMING_RSSI_THRESHOLD -65
#define WIFI_ROAMING_HYSTERESIS 5
#define WIFI_ROAMING_SCAN_TIMEOUT_MS 5000
#define WIFI_ROAMING_MIN_RSSI -95

// Основной рабочий режим
// #define WIFI_CONTINIOUS_WORK
#define WIFI_MINWORKTIME 10000

// Максимальное количество соседних AP для возврата
#define MAX_NEIGHBOR_APS 20
#define MAX_SSID_LENGTH 32

/**
 * @brief Тип callback-функции для событий Wi-Fi
 */
typedef void (*wifi_callback_t)(void);

/**
 * @brief Структура с информацией о текущем состоянии WiFi
 */
typedef struct {
    bool is_connected;          ///< Статус подключения
    uint8_t sta_mac[6];         ///< MAC адрес устройства
    int8_t rssi;                ///< Уровень сигнала в dBm
    uint8_t bssid[6];           ///< MAC адрес точки доступа
    char ssid[MAX_SSID_LENGTH + 1]; ///< Имя точки доступа
    esp_ip4_addr_t ip_addr;     ///< IP адрес устройства
    esp_ip4_addr_t gateway;     ///< IP шлюза
    esp_ip4_addr_t netmask;     ///< Маска подсети
    wifi_auth_mode_t auth_mode; ///< Тип аутентификации
    uint8_t channel;            ///< Канал WiFi
    wifi_second_chan_t second_channel; ///< Вторичный канал (для 40MHz)
} wifi_status_info_t;

/**
 * @brief Структура с информацией о соседней точке доступа
 */
typedef struct {
    char ssid[MAX_SSID_LENGTH + 1]; ///< Имя точки доступа
    uint8_t bssid[6];           ///< MAC адрес точки доступа
    int8_t rssi;                ///< Уровень сигнала в dBm
    uint8_t channel;            ///< Канал WiFi
    wifi_auth_mode_t auth_mode; ///< Тип аутентификации
    bool is_hidden;             ///< Скрытая сеть
    wifi_cipher_type_t pairwise_cipher; ///< Тип шифрования
    wifi_cipher_type_t group_cipher;    ///< Тип группового шифрования
} wifi_neighbor_ap_t;

/**
 * @brief Структура с результатами сканирования соседних AP
 */
typedef struct {
    wifi_neighbor_ap_t aps[MAX_NEIGHBOR_APS]; ///< Массив соседних AP
    uint16_t ap_count;                        ///< Количество найденных AP
    esp_err_t scan_status;                    ///< Статус сканирования
} wifi_neighbor_scan_result_t;

// Основные функции управления Wi-Fi
void WFc_Init(void);
void WFc_Start(void);
void WFc_Stop(void);
int WFc_IsConnected(void);
esp_err_t WFc_ConnectionWait(TickType_t xTicksToWait);

// Функции для callback'ов
esp_err_t WFc_ConnectCB_Register(wifi_callback_t callback);
esp_err_t WFc_DisconnectCB_Register(wifi_callback_t callback);

// Функции для повторных попыток и роуминга
// void WFc_ResetReconnectState(void);
// void WFc_EnableRoaming(bool enable);
// bool WFc_IsRoamingEnabled(void);
// esp_err_t WFc_TriggerRoamingScan(void);

// Новые функции для получения информации о WiFi
esp_err_t WFc_GetCurrentStatus(wifi_status_info_t *status_info);
esp_err_t WFc_ScanNeighborAPs(wifi_neighbor_scan_result_t *scan_result, bool async);
void WFc_RegisterScanCallback(void (*callback)(wifi_neighbor_scan_result_t *));

#endif // WIFI_COMMON_H