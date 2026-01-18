#include "esp_system.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_sntp.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"

#include "wifi_common.h"
#include "sntp_common.h"

#define TIME_STARTED BIT(0)
#define TIME_SINCHRONIZED BIT(1)
#define MAX_CALLBACKS 5  // Максимальное количество callback-функций

static const char *TAG = "SNTPCMN";
EventGroupHandle_t xTimeEventGroup;
static sntp_callback_type lcallbacks[MAX_CALLBACKS] = {NULL}; // Буфер callback-функций
static bool bTimeSynced = false; // Флаг синхронизации времени
static struct timeval lastTimeval; // Последние полученные данные времени

/*!
 *  @brief Вызов всех зарегистрированных callback-функций
 *
 *  @param[in] tv   текущее время
 *
 */
static void invoke_callbacks(struct timeval *tv) {
    for (int i = 0; i < MAX_CALLBACKS; i++) {
        if (lcallbacks[i] != NULL) {
            lcallbacks[i](tv);
        }
    }
}

/*!
 *  @brief Добавление callback-функции в буфер
 *
 *  @param[in] callback  функция обратного вызова
 *  @return true если успешно, false если буфер полон
 *
 */
static bool add_callback(sntp_callback_type callback) {
    // Если callback NULL, просто возвращаем успех
    if (callback == NULL) {
        return true;
    }
    
    for (int i = 0; i < MAX_CALLBACKS; i++) {
        if (lcallbacks[i] == NULL) {
            lcallbacks[i] = callback;
            return true;
        }
    }
    return false;
}

void sntp_notification(struct timeval *tv)
{
    struct tm timeinfo;
    char strftime_buf[20];

    localtime_r(&tv->tv_sec, &timeinfo);
    if (timeinfo.tm_year < (1970 - 1900)) {
        ESP_LOGE(TAG, "\t[SNTP SYNC]\tFAIL");        
    } else {
        // Сохраняем последние данные времени
        lastTimeval = *tv;
        bTimeSynced = true;

        // Log
        strftime(strftime_buf, sizeof(strftime_buf), "%d.%m.%Y %H:%M:%S", &timeinfo);
        ESP_LOGI(TAG, "\t[SNTP SYNC]\tOK");
        ESP_LOGI(TAG, "\t[CUR. TIME]\t%s", strftime_buf);

        // Вызываем все callback-функции
        invoke_callbacks(tv);

        xEventGroupSetBits(xTimeEventGroup, TIME_SINCHRONIZED);
        WFc_Stop();
        //stop wifi if it was started by this module!
    };
}

esp_err_t esp_netif_sntp_sync_wait(TickType_t tout)
{
    EventBits_t uxBits;
    if ((xTimeEventGroup != NULL))
    {
        uxBits = xEventGroupWaitBits(xTimeEventGroup, TIME_SINCHRONIZED, pdFALSE, pdFALSE, tout);
        if ((uxBits & TIME_SINCHRONIZED) == TIME_SINCHRONIZED)
        {
            return ESP_OK;
        } else {
            return ESP_ERR_TIMEOUT;
        }    
    } else {
        ESP_LOGE(TAG,"\t[SNTP]\tNOT INITILIZED");
        return ESP_ERR_INVALID_STATE;
    }
}

void cmn_sntp_init(sntp_callback_type callback)
{
    // Инициализация event group при первом вызове
    if (xTimeEventGroup == NULL)
    {
        xTimeEventGroup = xEventGroupCreate();
        if (xTimeEventGroup == NULL) {
            ESP_LOGE(TAG, "\t[SNTP INIT]\tFailed to create event group");
            return;
        }
    }

    // Добавляем callback в буфер
    if (!add_callback(callback)) {
        ESP_LOGE(TAG, "\t[SNTP INIT]\tCallback buffer full");
    } else {
        if (callback != NULL) {
            ESP_LOGI(TAG, "\t[SNTP INIT]\tCallback added successfully");
            
            // Если время уже синхронизировано, немедленно вызываем callback
            if (bTimeSynced) {
                ESP_LOGI(TAG, "\t[SNTP INIT]\tTime already synced, invoking callback immediately");
                callback(&lastTimeval);
            }
        } else {
            ESP_LOGI(TAG, "\t[SNTP INIT]\tInitialized without callback");
        }
    }

    // Инициализируем SNTP только при первом вызове
    static bool sntp_initialized = false;
    if (!sntp_initialized)
    {
        WFc_Start();
        if (WFc_ConnectionWait(portMAX_DELAY) == ESP_OK)
        {          
            ESP_LOGV(TAG,"\t[SNTP INIT]\tSTART");
            
            // Установка часового пояска
            setenv("TZ", "MSK-3", 1);
            tzset();

            // Запускаем синхронизацию времени с SNTP
            esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
            esp_sntp_set_time_sync_notification_cb(sntp_notification);
            esp_sntp_setservername(0, "192.168.2.56");
            esp_sntp_setservername(1, "time.nist.gov");
            esp_sntp_init();
            
            if (!esp_sntp_enabled()) {
                ESP_LOGE(TAG, "\t[SNTP INIT]\tFAILED to start client");
                return;
            }            
            
            ESP_LOGD(TAG,"\t[SNTP INIT]\tOK");
            esp_sntp_set_sync_interval(24 * 60 * 60 * 1000); // 24 часа
            xEventGroupSetBits(xTimeEventGroup, TIME_STARTED);
            sntp_initialized = true;
        } else {
            ESP_LOGE(TAG, "\tWiFi CONNETION ERROR - NO TIME SYNC AVALIABLE");
        }
    } else {
        ESP_LOGI(TAG,"\t[SNTP INIT]\tALREADY RUNNING");
    }
}

// Опциональная функция для удаления callback
void sntp_remove_callback(sntp_callback_type callback) {
    if (callback == NULL) {
        return;
    }
    
    for (int i = 0; i < MAX_CALLBACKS; i++) {
        if (lcallbacks[i] == callback) {
            lcallbacks[i] = NULL;
            ESP_LOGI(TAG, "\t[SNTP]\tCallback removed");
            break;
        }
    }
}