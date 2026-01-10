#pragma once

#include "hlk_ld2410c_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Инициализация модуля HLK-LD2410C
 * 
 * @param config Конфигурация модуля
 * @return esp_err_t Код ошибки ESP_OK при успехе
 */
esp_err_t hlk_ld2410c_init(const hlk_config_t *config);

/**
 * @brief Запуск поиска и подключения к датчику
 * 
 * @return esp_err_t Код ошибки ESP_OK при успехе
 */
esp_err_t hlk_ld2410c_start(void);

/**
 * @brief Остановка работы модуля
 */
void hlk_ld2410c_stop(void);

/**
 * @brief Получение текущего статуса подключения
 * 
 * @return true Подключен
 * @return false Не подключен
 */
bool hlk_ld2410c_is_connected(void);

/**
 * @brief Получение последних данных с датчика
 * 
 * @param data Указатель для сохранения данных
 * @return true Данные получены успешно
 * @return false Нет данных или ошибка
 */
bool hlk_ld2410c_get_latest_data(hlk_target_data_t *data);

/**
 * @brief Установка пароля Bluetooth
 * 
 * @param password Пароль (6 байт)
 * @return esp_err_t Код ошибки
 */
esp_err_t hlk_ld2410c_set_password(const uint8_t password[6]);

#ifdef __cplusplus
}
#endif