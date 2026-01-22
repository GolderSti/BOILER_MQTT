#ifndef BTN_COMMON_H
#define BTN_COMMON_H

#include <stdint.h>
#include <stdbool.h>
#include "driver/gpio.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BTN_PULL_UP = 0,
    BTN_PULL_DOWN,
    BTN_PULL_UP_DOWN,
    BTN_PULL_DISABLE
} button_pull_mode_t;

/**
 * @brief Тип callback-функции, вызываемой при событии кнопки.
 * @param gpio_num Номер GPIO, на котором произошло событие
 * @param event Тип события
 */
typedef void (*button_event_callback_t)(uint8_t gpio_num, uint8_t event);

/**
 * @brief Типы событий кнопки.
 */
typedef enum {
    BTN_EVENT_PRESSED = 0,     ///< Кнопка нажата
    BTN_EVENT_RELEASED,        ///< Кнопка отпущена
    BTN_EVENT_CLICK,           ///< Одиночный клик
    BTN_EVENT_LONG_PRESS,      ///< Долгое нажатие
    BTN_EVENT_DOUBLE_CLICK,    ///< Двойной клик
    BTN_EVENT_MAX
} button_event_state_t;

/**
 * @brief Конфигурация кнопки.
 */
typedef struct {
    bool active_low;           ///< Активный низкий уровень (true = нажатие = 0)
    uint32_t debounce_ms;      ///< Время подавления дребезга (мс)
    uint32_t long_press_ms;    ///< Время для определения долгого нажатия (мс)
    uint32_t double_click_ms;  ///< Макс. время между кликов для двойного нажатия (мс)
    button_pull_mode_t pull;     ///< Режим подтяжки
} button_config_t;

/**
 * @brief Дефолтная конфигурация кнопки.
 */
#define BUTTON_CONFIG_DEFAULT() { \
    .active_low = true,          \
    .debounce_ms = 50,           \
    .long_press_ms = 1000,       \
    .double_click_ms = 400,      \
    .pull = BTN_PULL_UP          \
}

/**
 * @brief Инициализация модуля обработки кнопок.
 * @note Должна вызываться один раз в начале программы.
 */
void button_init(void);

/**
 * @brief Регистрация кнопки.
 * @param gpio_num Номер GPIO
 * @param config Конфигурация кнопки (NULL для дефолтной)
 * @param callback Функция обратного вызова при событиях
 * @return true в случае успеха, false при ошибке
 */
bool button_register(uint8_t gpio_num, const button_config_t *config, 
                     button_event_callback_t callback);

/**
 * @brief Удаление регистрации кнопки.
 * @param gpio_num Номер GPIO
 * @return true в случае успеха
 */
bool button_unregister(uint8_t gpio_num);

/**
 * @brief Получение текущего состояния кнопки.
 * @param gpio_num Номер GPIO
 * @return true если кнопка нажата, false если отпущена
 */
bool button_get_state(uint8_t gpio_num);

#ifdef __cplusplus
}
#endif

#endif // BTN_COMMON_H