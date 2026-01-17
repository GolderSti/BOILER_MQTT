#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "driver/gpio.h"

#define DEBOUNCE_DELAY_MS 50 ///< Задержка для фильтра дребезга (в миллисекундах)
#define MAX_BUTTONS 4        ///< Максимальное количество кнопок

static bool isr_service_installed = false; ///< Флаг, указывающий, установлен ли сервис прерываний
static const char *TAG = "BTN_CMN";

/**
 * @brief Тип callback-функции, вызываемой при нажатии кнопки.
 */
typedef void (*button_callback_t)(void);

/**
 * @brief Структура для хранения информации о кнопке.
 */
typedef struct {
    uint8_t gpio_num;              ///< Номер GPIO, к которому подключена кнопка
    button_callback_t callback;    ///< Callback-функция, вызываемая при нажатии
    TimerHandle_t debounce_timer;  ///< Таймер для фильтра дребезга
    bool pressed;                  ///< Флаг, указывающий на нажатие кнопки
} button_t;

static button_t buttons[MAX_BUTTONS]; ///< Массив для хранения информации о кнопках
static uint8_t button_count = 0;     ///< Количество зарегистрированных кнопок

/**
 * @brief Обработчик прерываний GPIO.
 * @param arg Указатель на данные (в данном случае номер GPIO).
 */
static void IRAM_ATTR gpio_isr_handler(void* arg) {
    uint8_t gpio_num = (uint8_t)arg;
    for (int i = 0; i < button_count; i++) {
        if (buttons[i].gpio_num == gpio_num) {
            // Перезапуск таймера для фильтра дребезга
            xTimerResetFromISR(buttons[i].debounce_timer, NULL);
            break;
        }
    }
}


/**
 * @brief Инициализация GPIO для кнопки.
 * @param gpio_num Номер GPIO, к которому подключена кнопка.
 */
static void button_gpio_init(uint8_t gpio_num) {
    gpio_config_t io_conf;
    io_conf.intr_type = GPIO_INTR_ANYEDGE; // Прерывание на любое изменение состояния
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pin_bit_mask = (1ULL << gpio_num);
    io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    gpio_config(&io_conf);

 if (!isr_service_installed) {
        gpio_install_isr_service(0); // Установка сервиса прерываний GPIO (только один раз!)
        isr_service_installed = true;
    }
    gpio_isr_handler_add(gpio_num, gpio_isr_handler, (void*)gpio_num); // Добавление обработчика прерываний
}

/**
 * @brief Callback-функция таймера для фильтра дребезга.
 * @param xTimer Таймер, который вызвал callback.
 */
static void debounce_timer_callback(TimerHandle_t xTimer) {
    button_t* button = (button_t*)pvTimerGetTimerID(xTimer);

    bool current_state = gpio_get_level(button->gpio_num);
    if (current_state == 0) { // Кнопка активна низким уровнем
        if (!button->pressed) {
            button->pressed = true;
            ESP_LOGD(TAG, "Button on GPIO %d pressed", button->gpio_num);
            if (button->callback != NULL) {
                button->callback(); // Вызов callback-функции
            }
        }
    } else {
        if (button->pressed) {
            button->pressed = false;
            ESP_LOGD(TAG, "Button on GPIO %d released", button->gpio_num);
        }
    }
    
}

/**
 * @brief Регистрация callback-функции для кнопки.
 * @param gpio_num Номер GPIO, к которому подключена кнопка.
 * @param callback Callback-функция, которая будет вызвана при нажатии кнопки.
 * @note Максимальное количество кнопок ограничено значением MAX_BUTTONS.
 */
void button_register_callback(uint8_t gpio_num, button_callback_t callback) {
    if (button_count >= MAX_BUTTONS) {
        ESP_LOGE(TAG, "Error: Maximum number of buttons reached!");
        return;
    }

    buttons[button_count].gpio_num = gpio_num;
    buttons[button_count].callback = callback;
    buttons[button_count].pressed = false;

    // Создание таймера для фильтра дребезга
    buttons[button_count].debounce_timer = xTimerCreate(
        "debounce_timer",                  // Имя таймера
        pdMS_TO_TICKS(DEBOUNCE_DELAY_MS),  // Период таймера
        pdFALSE,                           // Одноразовый таймер
        (void*)&buttons[button_count],     // ID таймера (указатель на кнопку)
        debounce_timer_callback            // Callback-функция
    );

    button_gpio_init(gpio_num); // Инициализация GPIO

    button_count++;
    ESP_LOGD(TAG, "Button on GPIO %d registered (initial state: %s)", 
         gpio_num, buttons[button_count - 1].pressed ? "pressed" : "released");}

/**
 * @brief Инициализация модуля обработки кнопок.
 */
void button_init() {
}