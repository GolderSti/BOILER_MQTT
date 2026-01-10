#ifndef OTA_H
#define OTA_H

#include <stdbool.h>
#include "esp_err.h"

// Конфигурация OTA
typedef struct {
    char server_url[256];           // URL сервера (http://esp-update.lan:8080)
    char firmware_path[128];        // Команда запроса прошивки
    char version_path[128];         // Команда запроса версии прошивки
    char sha256_path[128];          // Команда запроса sha256 прошивки
    char app_name[128];             // имя приложения для запроса прошивки
    uint32_t check_interval_ms;     // Интервал проверки (мс)
    bool check_on_boot;             // Проверять при старте
    uint8_t max_retries;           // Максимальное число попыток
} ota_config_t;

// Статус OTA
typedef enum {
    OTA_STATUS_IDLE,            // Ожидание
    OTA_STATUS_CHECKING,        // Проверка обновления
    OTA_STATUS_UPDATING,        // Обновление в процессе
    OTA_STATUS_SUCCESS,         // Обновление успешно
    OTA_STATUS_FAILED,          // Ошибка обновления
    OTA_STATUS_NO_UPDATE,       // Обновление не требуется
    OTA_STATUS_UPDATE_AVAILABLE,// Обновление доступно
    OTA_STATUS_VERSION_ERROR,   // Ошибка проверки версии
} ota_status_t;

// Инициализация OTA
esp_err_t ota_init(const ota_config_t* config);

// Запуск проверки обновления
esp_err_t ota_check_for_update(void);

// Принудительное обновление до указанной версии
esp_err_t ota_update_to_version(const char* version);

// Получение статуса OTA
ota_status_t ota_get_status(void);

// Получение прогресса обновления (0-100%)
uint8_t ota_get_progress(void);

// Получение последней ошибки
const char* ota_get_last_error(void);

// Получение текущей версии на сервере
const char* ota_get_server_version(void);

// Запуск фоновой задачи проверки обновлений
void ota_start_background_check(void);

// Остановка фоновой задачи
void ota_stop_background_check(void);

#endif