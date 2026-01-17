#include <time.h>
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "sun_time.h"
#include "sntp_common.h"
#include "sun_time_common.h"

#define LATITUDE 56.349444f
#define LONGITUDE 44.114444f
#define LOCAL_OFFSET 180 //time diff in minutes
#define MAX_CALLBACKS 5  // Максимальное количество callback-функций

static const char *TAG = "SUNTCMN";
static int iSunriseT, iSunsetT;
static stc_callback_type lstc_callbacks[MAX_CALLBACKS] = {NULL}; // Буфер callback-функций
static uint64_t ulStartTime = 0;
static SemaphoreHandle_t stc_DataMutex;
static bool bDataReady = false; // Флаг готовности данных

/*!
 *  @brief Return local time in minutes from 00:00.
 *
 *  @return int32_t
 *
 */
int32_t GetLocalTime(){
    time_t now;
    struct tm *tm;
    now = time(NULL);
    tm = localtime(&now);
    return tm->tm_hour*60+tm->tm_min; 
}

/*!
 *  @brief Вызов всех зарегистрированных callback-функций
 *
 *  @param[in] sunrise  время восхода
 *  @param[in] sunset   время заката
 *
 */
static void invoke_callbacks(int sunrise, int sunset) {
    for (int i = 0; i < MAX_CALLBACKS; i++) {
        if (lstc_callbacks[i] != NULL) {
            lstc_callbacks[i](sunrise, sunset);
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
static bool add_callback(stc_callback_type callback) {
    // Если callback NULL, просто возвращаем успех
    if (callback == NULL) {
        return true;
    }
    
    for (int i = 0; i < MAX_CALLBACKS; i++) {
        if (lstc_callbacks[i] == NULL) {
            lstc_callbacks[i] = callback;
            return true;
        }
    }
    return false;
}

/*!
 *  @brief Callback that runs after SNTP was synced.
 *
 *  @param[out] tv   : current time
 *
 */
void sntp_callback(struct timeval *tv)
{
    // Calc Sunset and Sunrise for PWM Auto Task
    struct tm timeinfo;
    int localOffset;
    float lat;
    float lng;
    int ilSunSet, ilSunRise;
    uint64_t ulCurrTime;

    ulCurrTime = esp_timer_get_time();
    //calc SunRise/ SunSet time in minutes from 00:00 and send it to CallBack function
    localtime_r(&tv->tv_sec, &timeinfo);
    const int dayOfYear = timeinfo.tm_yday + 1; 
    const int dst = timeinfo.tm_isdst;
    lat = LATITUDE;
    lng = LONGITUDE;
    localOffset = LOCAL_OFFSET;

    ilSunRise = calculateSunriseSunset(dayOfYear, FROMFLOAT(lat), FROMFLOAT(lng), localOffset, dst, 1);
    ilSunSet = calculateSunriseSunset(dayOfYear, FROMFLOAT(lat), FROMFLOAT(lng), localOffset, dst, 0);
    ESP_LOGI(TAG, "\t[Sunrise/Sunset]:\t%02d:%02d/ %02d:%02d", ilSunRise / 60, ilSunRise % 60, ilSunSet / 60, ilSunSet % 60);
    
    if (xSemaphoreTake(stc_DataMutex, pdMS_TO_TICKS(1900))) {
        iSunriseT = ilSunRise;
        iSunsetT = ilSunSet;
        // iSunsetT=1097; //for debug purposes
        
        //calc start time 
        if (ulStartTime == 0) {
            //start time in microseconds
            ulStartTime = (uint64_t) tv->tv_sec * 1000000L + (uint64_t) tv->tv_usec - ulCurrTime;   
        }
        
        bDataReady = true; // Устанавливаем флаг готовности данных
        xSemaphoreGive(stc_DataMutex);
        
        // Вызываем все callback-функции после освобождения мьютекса
        invoke_callbacks(ilSunRise, ilSunSet);
    } else {
        ESP_LOGE(TAG, "\t[MQTT stc_DataMutex take]\tFAIL");
    }        
}

/*!
 *  @brief return value that stored in ulStartTime.
 *
 *  @result   : return StartTime
 *
 */
uint64_t stc_GetStartTime()
{
    uint64_t result = 0;
    if (xSemaphoreTake(stc_DataMutex, pdMS_TO_TICKS(1900))) {
        result = ulStartTime;
        xSemaphoreGive(stc_DataMutex);
    } else {
        ESP_LOGE(TAG, "\t[ulStartTime stc_DataMutex take]\tFAIL");
    }
    return result;
}

/*!
 *  @brief return value that stored in iSunriseT.
 *
 *  @result   : return Sunrise time
 *
 */
int stc_GetSunriseTime()
{
    int result = 360;
    if (xSemaphoreTake(stc_DataMutex, pdMS_TO_TICKS(1900))) {
        result = iSunriseT;
        xSemaphoreGive(stc_DataMutex);
    } else {
        ESP_LOGE(TAG, "\t[iSunriseT stc_DataMutex take]\tFAIL");
    }
    return result;
}

/*!
 *  @brief return value that stored in iSunsetT.
 *
 *  @result   : return Sunset time
 *
 */
int stc_GetSunsetTime()
{
    int result = 1200;
    if (xSemaphoreTake(stc_DataMutex, pdMS_TO_TICKS(1900))) {
        result = iSunsetT;
        xSemaphoreGive(stc_DataMutex);
    } else {
        ESP_LOGE(TAG, "\t[iSunsetT stc_DataMutex take]\tFAIL");
    }
    return result;
}

esp_err_t stc_sync_wait(TickType_t tout)
{
    return esp_netif_sntp_sync_wait(tout);
}

void sun_time_init(stc_callback_type stc_callback)
{
    // Инициализация мьютекса при первом вызове
    if (stc_DataMutex == NULL) {
        stc_DataMutex = xSemaphoreCreateMutex();
        if (stc_DataMutex == NULL) {
            ESP_LOGE(TAG, "\t[STC INIT]\tFailed to create mutex");
            return;
        }
        cmn_sntp_init(&sntp_callback);
    }
    
    if (xSemaphoreTake(stc_DataMutex, pdMS_TO_TICKS(1900))) {
        // Добавляем callback в буфер (может быть NULL)
        if (!add_callback(stc_callback)) {
            ESP_LOGE(TAG, "\t[STC INIT]\tCallback buffer full");
        } else {
            if (stc_callback != NULL) {
                ESP_LOGI(TAG, "\t[STC INIT]\tCallback added successfully");
                
                // Если данные уже готовы, немедленно вызываем callback
                if (bDataReady) {
                    ESP_LOGI(TAG, "\t[STC INIT]\tData ready, invoking callback immediately");
                    stc_callback(iSunriseT, iSunsetT);
                }
            } else {
                ESP_LOGI(TAG, "\t[STC INIT]\tInitialized without callback");
            }
        }
              
        xSemaphoreGive(stc_DataMutex);
    } else {
        ESP_LOGE(TAG, "\t[STC INIT]\tMutex take failed");
    }
}

// Опциональная функция для удаления callback
void stc_remove_callback(stc_callback_type stc_callback) {
    if (stc_callback == NULL) {
        return;
    }
    
    if (xSemaphoreTake(stc_DataMutex, pdMS_TO_TICKS(1900))) {
        for (int i = 0; i < MAX_CALLBACKS; i++) {
            if (lstc_callbacks[i] == stc_callback) {
                lstc_callbacks[i] = NULL;
                ESP_LOGI(TAG, "\t[STC]\tCallback removed");
                break;
            }
        }
        xSemaphoreGive(stc_DataMutex);
    }
}