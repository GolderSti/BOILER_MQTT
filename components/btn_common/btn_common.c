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
#define BUTTON_EVENT_QUEUE_SIZE 10

static const char *TAG = "BTN";

// События для обработки в задаче
typedef enum {
    BTN_EVT_GPIO_CHANGE,
    BTN_EVT_TIMER_DEBOUNCE,
    BTN_EVT_TIMER_LONGPRESS,
} button_event_type_t;

typedef struct {
    button_event_type_t type;
    uint8_t gpio_num;
    bool state; // Для GPIO_CHANGE
} button_event_t;

// Состояния конечного автомата
typedef enum {
    BTN_STATE_IDLE,
    BTN_STATE_FIRST_PRESS,
    BTN_STATE_FIRST_RELEASE,
    BTN_STATE_SECOND_PRESS,
    BTN_STATE_PRESSED // Для долгого нажатия
} btn_fsm_state_t;

// Структура для хранения данных кнопки
typedef struct {
    uint8_t gpio_num;
    button_config_t config;
    button_event_callback_t callback;
    
    // Состояние FSM
    btn_fsm_state_t state;
    bool current_state;        // Текущее физическое состояние
    bool debounced_state;      // Состояние после антидребезга
    
    // Временные метки
    uint32_t first_press_time;
    uint32_t press_time;
    
    // Таймеры
    TimerHandle_t debounce_timer;
    TimerHandle_t longpress_timer;
    
} button_info_t;

static button_info_t buttons[MAX_BUTTONS];
static uint8_t button_count = 0;
static QueueHandle_t button_event_queue = NULL;
static TaskHandle_t button_task_handle = NULL;

// Прототипы
static button_info_t* find_button_by_gpio(uint8_t gpio_num);
static void debounce_timer_callback(TimerHandle_t xTimer);
static void longpress_timer_callback(TimerHandle_t xTimer);
static void button_task(void* arg);
static void handle_debounced_event(button_info_t* btn, bool state);
static void reset_button_state(button_info_t* btn);

// Сброс состояния кнопки
static void reset_button_state(button_info_t* btn) {
    btn->state = BTN_STATE_IDLE;
	ESP_LOGD(TAG, "BTN_STATE_IDLE");
    btn->first_press_time = 0;
    btn->press_time = 0;
    
    // Останавливаем все таймеры
    if (btn->longpress_timer) {
        xTimerStop(btn->longpress_timer, 0);
    }
}

// Обработчик прерывания GPIO
static void IRAM_ATTR gpio_isr_handler(void* arg) {
    uint32_t gpio_num = (uint32_t)arg;
    
    // Отправляем событие в очередь
    button_event_t evt = {
        .type = BTN_EVT_GPIO_CHANGE,
        .gpio_num = (uint8_t)gpio_num,
        .state = gpio_get_level(gpio_num)
    };
    
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    xQueueSendFromISR(button_event_queue, &evt, &xHigherPriorityTaskWoken);
    
    if (xHigherPriorityTaskWoken) {
        portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
    }
}

// Callback таймера антидребезга
static void debounce_timer_callback(TimerHandle_t xTimer) {
    button_info_t* btn = (button_info_t*)pvTimerGetTimerID(xTimer);
    
    if (!btn) return;
    
    // Отправляем событие в очередь для обработки в задаче
    button_event_t evt = {
        .type = BTN_EVT_TIMER_DEBOUNCE,
        .gpio_num = btn->gpio_num,
        .state = gpio_get_level(btn->gpio_num)
    };
    
    xQueueSend(button_event_queue, &evt, 0);
}

// Callback таймера долгого нажатия
static void longpress_timer_callback(TimerHandle_t xTimer) {
    button_info_t* btn = (button_info_t*)pvTimerGetTimerID(xTimer);
    
    if (!btn) return;
    
    button_event_t evt = {
        .type = BTN_EVT_TIMER_LONGPRESS,
        .gpio_num = btn->gpio_num,
        .state = true
    };
    
    xQueueSend(button_event_queue, &evt, 0);
}

// Задача для обработки событий кнопок
static void button_task(void* arg) {
    button_event_t evt;
    
    while (1) {
        if (xQueueReceive(button_event_queue, &evt, portMAX_DELAY)) {
            button_info_t* btn = find_button_by_gpio(evt.gpio_num);
            if (!btn) continue;
            
            bool processed_state = evt.state;
            if (btn->config.active_low) {
                processed_state = !processed_state;
            }
            
            switch (evt.type) {
                case BTN_EVT_GPIO_CHANGE:
                    // При изменении GPIO запускаем таймер антидребезга
                    if (processed_state != btn->debounced_state) {
						xTimerStart(btn->debounce_timer, 0);
					}
                    break;
                    
                case BTN_EVT_TIMER_DEBOUNCE:
                    // Таймер антидребезга сработал
					ESP_LOGD(TAG,"BTN_EVT_TIMER_DEBOUNCE");
                    btn->debounced_state = processed_state;
					handle_debounced_event(btn, processed_state);
                    break;
                    
                case BTN_EVT_TIMER_LONGPRESS:
                    // Таймер долгого нажатия сработал
					ESP_LOGD(TAG,"BTN_EVT_TIMER_LONGPRESS");
                    if ((btn->state == BTN_STATE_FIRST_PRESS || btn->state == BTN_STATE_SECOND_PRESS) && btn->debounced_state) {
                        
						if (btn->callback) {
                            btn->callback(btn->gpio_num, BTN_EVENT_LONG_PRESS);
                        }
                        // После долгого нажатия сбрасываем состояние двойного клика
                        reset_button_state(btn);
                    }
                    break;
            }
        }
    }
}

// Обработка события после антидребезга
static void handle_debounced_event(button_info_t* btn, bool state) {
    uint32_t now = esp_timer_get_time() / 1000;
    
    if (state) { // Нажатие
        btn->press_time = now;
        
        switch (btn->state) {
            case BTN_STATE_IDLE:
                // Первое нажатие
                btn->state = BTN_STATE_FIRST_PRESS;
				ESP_LOGD(TAG, "BTN_STATE_FIRST_PRESS");
                btn->first_press_time = now;
                
                // Запускаем таймер долгого нажатия
                if (btn->config.long_press_ms > 0 && btn->longpress_timer) {
                    xTimerChangePeriod(btn->longpress_timer, 
                                     pdMS_TO_TICKS(btn->config.long_press_ms), 0);
                    xTimerStart(btn->longpress_timer, 0);
                }
                
                if (btn->callback) {
                    btn->callback(btn->gpio_num, BTN_EVENT_PRESSED);
                }
                if (btn->callback) {
                    btn->callback(btn->gpio_num, BTN_EVENT_CLICK);
                }
                break;
                
            case BTN_STATE_FIRST_RELEASE:
                // Второе нажатие 
                // Проверяем настроено ли двойное нажатие и попали ли мы в окно двойного нажатия
                if (btn->config.double_click_ms > 0 && (now-btn->first_press_time) < btn->config.double_click_ms ) {
					btn->state = BTN_STATE_SECOND_PRESS;
					ESP_LOGD(TAG, "BTN_STATE_SECOND_PRESS");
					
					if (btn->callback) {
						btn->callback(btn->gpio_num, BTN_EVENT_PRESSED);
					}
					if (btn->callback) {
						btn->callback(btn->gpio_num, BTN_EVENT_CLICK);
					}
					if (btn->callback) {
						btn->callback(btn->gpio_num, BTN_EVENT_DOUBLE_CLICK);
					}
					// Запускаем таймер долгого нажатия
					if (btn->config.long_press_ms > 0 && btn->longpress_timer) {
						xTimerChangePeriod(btn->longpress_timer, 
										 pdMS_TO_TICKS(btn->config.long_press_ms), 0);
						xTimerStart(btn->longpress_timer, 0);
					}
                }else{ //если нет, то повторяем одинарное нажатие
					// Первое нажатие
					btn->state = BTN_STATE_FIRST_PRESS;
					ESP_LOGD(TAG, "BTN_STATE_FIRST_PRESS");
					btn->first_press_time = now;
					
					// Запускаем таймер долгого нажатия
					if (btn->config.long_press_ms > 0 && btn->longpress_timer) {
						xTimerChangePeriod(btn->longpress_timer, 
										 pdMS_TO_TICKS(btn->config.long_press_ms), 0);
						xTimerStart(btn->longpress_timer, 0);
					}
					
					if (btn->callback) {
						btn->callback(btn->gpio_num, BTN_EVENT_PRESSED);
					}
					if (btn->callback) {
						btn->callback(btn->gpio_num, BTN_EVENT_CLICK);
					}
				}
                
                break;
                
            default:
                // Другие состояния - игнорируем
				ESP_LOGD(TAG,"btn.state %i", btn->state);
                break;
        }
    } else { // Отпускание
        switch (btn->state) {
            case BTN_STATE_FIRST_PRESS:
                // Первое отпускание
                btn->state = BTN_STATE_FIRST_RELEASE;
				ESP_LOGD(TAG, "BTN_STATE_FIRST_RELEASE");
                
                // Останавливаем таймер долгого нажатия
                if (btn->longpress_timer) {
                    xTimerStop(btn->longpress_timer, 0);
                }
                                
                if (btn->callback) {
                    btn->callback(btn->gpio_num, BTN_EVENT_RELEASED);
                }
                break;
                
            case BTN_STATE_SECOND_PRESS:
                                
                if (btn->callback) {
                    btn->callback(btn->gpio_num, BTN_EVENT_RELEASED);
                }
                // Второе отпускание - двойной клик завершен
                reset_button_state(btn);
                break;
                               
            default:
                // Другие состояния
                if (btn->callback) {
                    btn->callback(btn->gpio_num, BTN_EVENT_RELEASED);
                }
				ESP_LOGD(TAG,"btn.state %i", btn->state);
                reset_button_state(btn);
                break;
        }
    }
}

// Находим кнопку по GPIO
static button_info_t* find_button_by_gpio(uint8_t gpio_num) {
    for (int i = 0; i < button_count; i++) {
        if (buttons[i].gpio_num == gpio_num) {
            return &buttons[i];
        }
    }
    return NULL;
}

// Инициализация модуля
void button_init(void) {
    memset(buttons, 0, sizeof(buttons));
    button_count = 0;
    
    // Создаем очередь для событий
    button_event_queue = xQueueCreate(BUTTON_EVENT_QUEUE_SIZE, sizeof(button_event_t));
    if (!button_event_queue) {
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
                                     configMAX_PRIORITIES - 2, &button_task_handle);
    if (task_ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create button task");
        vQueueDelete(button_event_queue);
        button_event_queue = NULL;
        return;
    }
    
    ESP_LOGI(TAG, "Button module initialized with task and queue");
}

// Регистрация кнопки
bool button_register(uint8_t gpio_num, const button_config_t *config, 
                     button_event_callback_t callback) {
    if (button_count >= MAX_BUTTONS) {
        ESP_LOGE(TAG, "Max buttons reached");
        return false;
    }
    
    // Проверяем, не зарегистрирован ли уже
    if (find_button_by_gpio(gpio_num) != NULL) {
        ESP_LOGW(TAG, "GPIO %d already registered", gpio_num);
        return false;
    }
    
    button_info_t* btn = &buttons[button_count];
    memset(btn, 0, sizeof(button_info_t));
    btn->gpio_num = gpio_num;
    btn->callback = callback;
    
    // Копируем конфигурацию
    if (config) {
        memcpy(&btn->config, config, sizeof(button_config_t));
    } else {
        button_config_t default_cfg = BUTTON_CONFIG_DEFAULT();
        memcpy(&btn->config, &default_cfg, sizeof(button_config_t));
    }
    
    // Конвертируем pull mode
    gpio_pull_mode_t esp_pull;
    switch (btn->config.pull) {
        case BTN_PULL_UP:
            esp_pull = GPIO_PULLUP_ONLY;
            break;
        case BTN_PULL_DOWN:
            esp_pull = GPIO_PULLDOWN_ONLY;
            break;
        case BTN_PULL_UP_DOWN:
            esp_pull = GPIO_PULLUP_PULLDOWN;
            break;
        case BTN_PULL_DISABLE:
        default:
            esp_pull = GPIO_FLOATING;
            break;
    }
    
    // Настройка GPIO
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << gpio_num),
        .mode = GPIO_MODE_INPUT,
        .intr_type = GPIO_INTR_ANYEDGE,
        .pull_up_en = (esp_pull == GPIO_PULLUP_ONLY || esp_pull == GPIO_PULLUP_PULLDOWN),
        .pull_down_en = (esp_pull == GPIO_PULLDOWN_ONLY || esp_pull == GPIO_PULLUP_PULLDOWN),
    };
    
    esp_err_t ret = gpio_config(&io_conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure GPIO %d: %d", gpio_num, ret);
        return false;
    }
    
    // Создаем таймеры
    char timer_name[20];
    
    // Таймер антидребезга
    snprintf(timer_name, sizeof(timer_name), "debounce_%d", gpio_num);
    btn->debounce_timer = xTimerCreate(
        timer_name,
        pdMS_TO_TICKS(btn->config.debounce_ms),
        pdFALSE,
        (void*)btn,
        debounce_timer_callback
    );
    
    // Таймер долгого нажатия
    snprintf(timer_name, sizeof(timer_name), "longpress_%d", gpio_num);
    btn->longpress_timer = xTimerCreate(
        timer_name,
        pdMS_TO_TICKS(btn->config.long_press_ms),
        pdFALSE,
        (void*)btn,
        longpress_timer_callback
    );
    
    // Проверяем создание таймеров
    if (!btn->debounce_timer || !btn->longpress_timer) {
        ESP_LOGE(TAG, "Failed to create timers for GPIO %d", gpio_num);
        if (btn->debounce_timer) xTimerDelete(btn->debounce_timer, 0);
        if (btn->longpress_timer) xTimerDelete(btn->longpress_timer, 0);
        gpio_isr_handler_remove(gpio_num);
        return false;
    }
    
    // Регистрируем обработчик прерывания
    ret = gpio_isr_handler_add(gpio_num, gpio_isr_handler, (void*)(uint32_t)gpio_num);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add ISR handler for GPIO %d: %d", gpio_num, ret);
        xTimerDelete(btn->debounce_timer, 0);
        xTimerDelete(btn->longpress_timer, 0);
        return false;
    }
    
    // Инициализируем состояние
    bool init_state = gpio_get_level(gpio_num);
    if (btn->config.active_low) {
        init_state = !init_state;
    }
    btn->current_state = init_state;
    btn->debounced_state = init_state;
    btn->state = BTN_STATE_IDLE;
    
    button_count++;
    ESP_LOGI(TAG, "Button registered on GPIO %d", gpio_num);
    return true;
}

// Удаление регистрации
bool button_unregister(uint8_t gpio_num) {
    for (int i = 0; i < button_count; i++) {
        if (buttons[i].gpio_num == gpio_num) {
            // Удаляем обработчик прерывания
            gpio_isr_handler_remove(gpio_num);
            
            // Удаляем таймеры
            if (buttons[i].debounce_timer) {
                xTimerDelete(buttons[i].debounce_timer, 0);
            }
            if (buttons[i].longpress_timer) {
                xTimerDelete(buttons[i].longpress_timer, 0);
            }
            
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

// Получение состояния
bool button_get_state(uint8_t gpio_num) {
    button_info_t* btn = find_button_by_gpio(gpio_num);
    return btn ? btn->debounced_state : false;
}