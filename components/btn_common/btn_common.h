#include <stdint.h>
/**
 * @brief Тип callback-функции, вызываемой при нажатии кнопки.
 */
typedef void (*button_callback_t)(void);

/**
 * @brief Регистрация callback-функции для кнопки.
 * @param gpio_num Номер GPIO, к которому подключена кнопка.
 * @param callback Callback-функция, которая будет вызвана при нажатии кнопки.
 * @note Максимальное количество кнопок ограничено значением MAX_BUTTONS.
 */
void button_register_callback(uint8_t gpio_num, button_callback_t callback);

/**
 * @brief Инициализация модуля обработки кнопок.
 */
void button_init();
