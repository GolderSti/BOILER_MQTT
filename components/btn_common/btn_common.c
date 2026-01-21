#include "btn_common.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "freertos/queue.h"
#include "driver/gpio.h"
#include <string.h>

#define MAX_BUTTONS 16
#define DEBOUNCE_TICKS_MS 5 // Точность опроса в режиме polling

static const char *TAG = "BTN";

// Конечный автомат для обработки кнопки
typedef enum {
    BTN_STATE_IDLE,
    BTN_STATE_PRESS_DEBOUNCE,
    BTN_STATE_PRESSED,
    BTN_STATE_RELEASE_DEBOUNCE,
    BTN_STATE_WAIT_DOUBLE
} btn_fsm_state_t;

typedef struct {
    uint8_t gpio_num;
    button_config_t config;
    button_event_callback_t callback;
    
    // Состояние конечного автомата
    btn_fsm_state_t state;
    uint32_t press_time;
    uint32_t release_time;
    uint32_t last_event_time;
    bool stable_state;
    bool last_stable_state;
    uint8_t click_count;
    
    // Для режима прерываний
    TimerHandle_t debounce_timer;
    bool timer_running;
} button_info_t;

static button_info_t buttons[MAX_BUTTONS];
static uint8_t button_count = 0;
static QueueHandle_t gpio_event_queue = NULL;
static TaskHandle_t button_task_handle = NULL;
static bool use_interrupts = true;

// Прототипы функций
static void button_process_event(button_info_t* btn);
static void debounce_timer_callback(TimerHandle_t xTimer);

static void IRAM_ATTR gpio_isr_handler(void* arg) {
    uint32_t gpio_num = (uint32_t)arg;
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    xQueueSendFromISR(gpio_event_queue, &gpio_num, &xHigherPriorityTaskWoken);
    if (xHigherPriorityTaskWoken) {
        portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
    }
}

static void button_task(void* arg) {
    uint32_t gpio_num;
    
    while (1) {
        if (xQueueReceive(gpio_event_queue, &gpio_num, portMAX_DELAY)) {
            for (int i = 0; i < button_count; i++) {
                if (buttons[i].gpio_num == gpio_num) {
                    button_process_event(&buttons[i]);
                    break;
                }
            }
        }
    }
}

static void button_process_event(button_info_t* btn) {
    bool current = gpio_get_level(btn->gpio_num);
    if (btn->config.active_low) {
        current = !current; // Инвертируем если active_low
    }
    
    uint32_t now = esp_timer_get_time() / 1000;
    
    switch (btn->state) {
        case BTN_STATE_IDLE:
            if (current) {
                btn->state = BTN_STATE_PRESS_DEBOUNCE;
                btn->press_time = now;
            }
            break;
            
        case BTN_STATE_PRESS_DEBOUNCE:
            if (now - btn->press_time >= btn->config.debounce_ms) {
                if (current) {
                    btn->state = BTN_STATE_PRESSED;
                    btn->last_event_time = now;
                    if (btn->callback) {
                        btn->callback(btn->gpio_num, BTN_EVENT_PRESSED);
                    }
                } else {
                    btn->state = BTN_STATE_IDLE;
                }
            }
            break;
            
        case BTN_STATE_PRESSED:
            if (!current) {
                btn->state = BTN_STATE_RELEASE_DEBOUNCE;
                btn->release_time = now;
            } else if (now - btn->last_event_time >= btn->config.long_press_ms) {
                if (btn->callback) {
                    btn->callback(btn->gpio_num, BTN_EVENT_LONG_PRESS);
                }
                btn->last_event_time = now; // Сброс для повторных long press
            }
            break;
            
        case BTN_STATE_RELEASE_DEBOUNCE:
            if (now - btn->release_time >= btn->config.debounce_ms) {
                if (!current) {
                    btn->click_count++;
                    
                    if (btn->click_count == 1) {
                        btn->state = BTN_STATE_WAIT_DOUBLE;
                        btn->last_event_time = now;
                    } else if (btn->click_count == 2) {
                        if (btn->callback) {
                            btn->callback(btn->gpio_num, BTN_EVENT_DOUBLE_CLICK);
                        }
                        btn->click_count = 0;
                        btn->state = BTN_STATE_IDLE;
                    }
                    
                    if (btn->callback) {
                        btn->callback(btn->gpio_num, BTN_EVENT_RELEASED);
                    }
                } else {
                    btn->state = BTN_STATE_PRESSED;
                }
            }
            break;
            
        case BTN_STATE_WAIT_DOUBLE:
            if (now - btn->last_event_time >= btn->config.double_click_ms) {
                // Одиночный клик
                if (btn->callback && btn->click_count == 1) {
                    btn->callback(btn->gpio_num, BTN_EVENT_CLICK);
                }
                btn->click_count = 0;
                btn->state = BTN_STATE_IDLE;
            } else if (current) {
                // Второе нажатие в пределах времени двойного клика
                btn->state = BTN_STATE_PRESS_DEBOUNCE;
                btn->press_time = now;
            }
            break;
    }
}

static void debounce_timer_callback(TimerHandle_t xTimer) {
    button_info_t* btn = (button_info_t*)pvTimerGetTimerID(xTimer);
    
    // Обработка в контексте таймера
    bool current = gpio_get_level(btn->gpio_num);
    if (btn->config.active_low) {
        current = !current;
    }
    
    // Простая проверка состояния
    if (current != btn->stable_state) {
        btn->stable_state = current;
        if (btn->callback) {
            if (current) {
                btn->callback(btn->gpio_num, BTN_EVENT_PRESSED);
            } else {
                btn->callback(btn->gpio_num, BTN_EVENT_RELEASED);
            }
        }
    }
    
    btn->timer_running = false;
}

void button_init(void) {
    memset(buttons, 0, sizeof(buttons));
    
    // Создаем очередь для событий от прерываний
    gpio_event_queue = xQueueCreate(10, sizeof(uint32_t));
    
    if (!gpio_event_queue) {
        ESP_LOGE(TAG, "Failed to create event queue");
        return;
    }
    
    // Устанавливаем сервис прерываний
    esp_err_t ret = gpio_install_isr_service(0);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "Failed to install ISR service: %d", ret);
    }
    
    // Создаем задачу для обработки событий
    BaseType_t task_ret = xTaskCreate(button_task, "button_task", 4096, NULL, 
                                     tskIDLE_PRIORITY + 1, &button_task_handle);
    if (task_ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create button task");
    }
    
    ESP_LOGI(TAG, "Button module initialized");
}

bool button_register(uint8_t gpio_num, const button_config_t *config, 
                     button_event_callback_t callback) {
    if (button_count >= MAX_BUTTONS) {
        ESP_LOGE(TAG, "Max buttons reached");
        return false;
    }
    
    if (gpio_num >= GPIO_NUM_MAX) {
        ESP_LOGE(TAG, "Invalid GPIO number: %d", gpio_num);
        return false;
    }
    
    // Проверяем, не зарегистрирован ли уже этот GPIO
    for (int i = 0; i < button_count; i++) {
        if (buttons[i].gpio_num == gpio_num) {
            ESP_LOGW(TAG, "GPIO %d already registered", gpio_num);
            return false;
        }
    }
    
    button_info_t* btn = &buttons[button_count];
    memset(btn, 0, sizeof(button_info_t));
    btn->gpio_num = gpio_num;
    btn->callback = callback;
    
    // Копируем конфигурацию или используем дефолтную
    if (config) {
        memcpy(&btn->config, config, sizeof(button_config_t));
    } else {
        button_config_t default_cfg = BUTTON_CONFIG_DEFAULT();
        memcpy(&btn->config, &default_cfg, sizeof(button_config_t));
    }
    
    // Настройка GPIO
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << gpio_num),
        .mode = GPIO_MODE_INPUT,
        .intr_type = GPIO_INTR_ANYEDGE,
        .pull_up_en = (btn->config.pull == GPIO_PULLUP_ONLY || 
                       btn->config.pull == GPIO_PULLUP_PULLDOWN) ? 1 : 0,
        .pull_down_en = (btn->config.pull == GPIO_PULLDOWN_ONLY || 
                         btn->config.pull == GPIO_PULLUP_PULLDOWN) ? 1 : 0,
    };
    
    esp_err_t ret = gpio_config(&io_conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure GPIO %d: %d", gpio_num, ret);
        return false;
    }
    
    // Регистрируем обработчик прерывания
    ret = gpio_isr_handler_add(gpio_num, gpio_isr_handler, (void*)(uint32_t)gpio_num);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add ISR handler for GPIO %d: %d", gpio_num, ret);
        return false;
    }
    
    // Создаем таймер для антидребезга
    char timer_name[16];
    snprintf(timer_name, sizeof(timer_name), "btn_tmr_%d", gpio_num);
    btn->debounce_timer = xTimerCreate(
        timer_name,
        pdMS_TO_TICKS(btn->config.debounce_ms),
        pdFALSE,
        (void*)btn,
        debounce_timer_callback
    );
    
    if (!btn->debounce_timer) {
        ESP_LOGE(TAG, "Failed to create timer for GPIO %d", gpio_num);
        gpio_isr_handler_remove(gpio_num);
        return false;
    }
    
    // Инициализируем состояние
    bool init_state = gpio_get_level(gpio_num);
    if (btn->config.active_low) {
        init_state = !init_state;
    }
    btn->stable_state = init_state;
    btn->last_stable_state = init_state;
    btn->state = BTN_STATE_IDLE;
    btn->timer_running = false;
    
    button_count++;
    ESP_LOGI(TAG, "Button registered on GPIO %d", gpio_num);
    return true;
}

bool button_unregister(uint8_t gpio_num) {
    for (int i = 0; i < button_count; i++) {
        if (buttons[i].gpio_num == gpio_num) {
            // Удаляем таймер
            if (buttons[i].debounce_timer) {
                xTimerDelete(buttons[i].debounce_timer, portMAX_DELAY);
            }
            
            // Удаляем обработчик прерывания
            gpio_isr_handler_remove(gpio_num);
            
            // Сдвигаем массив
            for (int j = i; j < button_count - 1; j++) {
                buttons[j] = buttons[j + 1];
            }
            
            button_count--;
            memset(&buttons[button_count], 0, sizeof(button_info_t));
            
            ESP_LOGI(TAG, "Button unregistered from GPIO %d", gpio_num);
            return true;
        }
    }
    return false;
}

bool button_get_state(uint8_t gpio_num) {
    for (int i = 0; i < button_count; i++) {
        if (buttons[i].gpio_num == gpio_num) {
            return buttons[i].stable_state;
        }
    }
    return false;
}

void button_poll(void) {
    static uint32_t last_poll_time = 0;
    uint32_t now = esp_timer_get_time() / 1000;
    
    if (now - last_poll_time < DEBOUNCE_TICKS_MS) {
        return;
    }
    
    last_poll_time = now;
    
    for (int i = 0; i < button_count; i++) {
        button_process_event(&buttons[i]);
    }
}