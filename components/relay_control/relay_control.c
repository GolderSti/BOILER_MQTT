#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <float.h>
#include <sys/stat.h>
#include "esp_system.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "relay_control.h"
#include "mqtt_common.h"
#include "esp_log.h"
#include "esp_sntp.h"
#include "sun_time_common.h"

#define RELAY_OUTPUT_IO_1   4
#define RELAY_OUTPUT_IO_2   5
#define RELAY_OUTPUT_IO_3   3  // Добавлен пин для третьего реле
#define GPIO_OUTPUT_PIN_SEL  ((1ULL<<RELAY_OUTPUT_IO_1) | (1ULL<<RELAY_OUTPUT_IO_2) | (1ULL<<RELAY_OUTPUT_IO_3)) 
#define RELAY3_ON_TIME 15*60*1000 //15 минут в миллисикундах...
static const char *TAG = "RLYCNTR";
#define MAX_MESSAGE_LENGTH 50
#define TPC_RLY1 0
#define TPC_RLY1_AUTO 1
#define TPC_RLY2 2
#define TPC_RLY2_AUTO 3
#define TPC_RLY3 4           // Добавлено для третьего реле
#define TPC_RLY3_AUTO 5      // Добавлено для третьего реле
static const gpio_num_t rly_pins[] = {
                                    RELAY_OUTPUT_IO_1,
                                    RELAY_OUTPUT_IO_2,
                                    RELAY_OUTPUT_IO_3,  // Добавлено
                                    };

static const char *pcTopics[] = {                    
                                [TPC_RLY1]="/Relay1",
                                [TPC_RLY1_AUTO]="/Relay1/Auto",
                                [TPC_RLY2]="/Relay2",
                                [TPC_RLY2_AUTO]="/Relay2/Auto",
                                [TPC_RLY3]="/Relay3",           // Добавлено
                                [TPC_RLY3_AUTO]="/Relay3/Auto", // Добавлено
                                };

#define CONTROL_TOPICS_COUNT 6  // Обновлено с 4 до 6
static const char *pcControlTopics[] = {
                                        "/Relay1/Set",       //0
                                        "/Relay1/Auto/Set",  //1
                                        "/Relay2/Set",       //2
                                        "/Relay2/Auto/Set",  //3
                                        "/Relay3/Set",       //4 - Добавлено
                                        "/Relay3/Auto/Set",  //5 - Добавлено
                                        };

typedef struct 
{
    uint32_t uState;
    uint32_t uAutoState;
} RelayState_t;

static QueueHandle_t xRelay_Msg_Queue;
static int32_t uiOnTime=480, uiOffTime=1200;
static SemaphoreHandle_t xAutoPowerMutex;
static int8_t iAutoPower; 
static SemaphoreHandle_t rc_TimeMutex;
static const char *CFG_STATE="state";
static const char *CFG_AUTO="auto";
static const char *CFG_STATE2="state2";
static const char *CFG_AUTO2="auto2";
static const char *CFG_STATE3="state3";  // Добавлено для третьего реле
static const char *CFG_AUTO3="auto3";    // Добавлено для третьего реле
    // Таймеры
TimerHandle_t on_delay_timer;



/*!
 * Expand Queue rslt to string
 */

static void QUEUE_check_rslt(const char api_name[], BaseType_t rslt)
{
    switch (rslt)
    {
        // pdPASS = 0,                                    /*!< Function execution successful */
        case pdPASS:

            ESP_LOGV(TAG,"\t[%s]\tOK", api_name);
            break;
        default:
            ESP_LOGE(TAG,"\t[%s]\tError [%d] : Bufer is full\r\n", api_name, rslt);
            break;
    }
}

/*!
 *  @brief Send relay state to MQTT Msg quueue
 *
 *  @param[in] uTopicNmb    : one of TPC_XXX msg
 *  @param[in] uMsg         : msg to send
 *
 */
static void rly_MQTT_MSG_send(uint32_t uTopicNmb, uint32_t uMsg)
{

    char *pTopic = malloc(strlen(pcTopics[uTopicNmb]) + 1);  // Mem alloc for Topic string
    if (pTopic != NULL) {
        char *pMessage = malloc(MAX_MESSAGE_LENGTH);  //Mem alloc for message string
        if (pMessage!= NULL)
        {
            strcpy(pTopic, pcTopics[uTopicNmb]);  
            sprintf(pMessage,"%lu",uMsg);
            mqtt_Message_Publish(pTopic, pMessage);
            free(pTopic);
            free(pMessage);
        }else //Can't allocate memory for Message string - free topic string memory here
        {
            free(pTopic);
        }
        
    }
}

/**
 * @brief Инициализирует раздел NVS с указанным именем.
 *
 * @param partition_name Имя раздела из таблицы разделов (например, "storage").
 * @return ESP_OK — успех, иначе код ошибки.
 */
esp_err_t nvs_storage_init(const char *partition_name) {
    // esp_err_t err = nvs_flash_init_partition(partition_name);
    // if (err != ESP_OK) {
    //     ESP_LOGE(TAG, "Failed to init NVS partition '%s': %s", partition_name, esp_err_to_name(err));
    // } else {
    //     ESP_LOGI(TAG, "NVS partition '%s' initialized", partition_name);
    // }
    // return err;

    ESP_LOGI(TAG, "Initializing custom storage partition...");
    
    // Находим наш раздел
    const esp_partition_t* partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA,
        ESP_PARTITION_SUBTYPE_ANY,
        "storage");
    
    if (partition == NULL) {
        ESP_LOGE(TAG, "Storage partition not found!");
        return ESP_FAIL;
    }
    
    ESP_LOGI(TAG, "Found storage partition: size=%dKB", partition->size / 1024);
    
    // Инициализируем NVS в этом разделе
    // nvs_sec_cfg_t cfg;
    // nvs_flash_secure_init_partition(partition_name, &cfg);
    
    // Альтернативный способ - ручная инициализация
    esp_err_t ret = nvs_flash_init_partition("storage");
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "Erasing %s partition...", partition_name);
        ESP_ERROR_CHECK(nvs_flash_erase_partition("storage"));
        ret = nvs_flash_init_partition("storage");
    }
    
    ESP_ERROR_CHECK(ret);
    ESP_LOGI(TAG, "%s partition initialized", partition_name);
    return ESP_OK;
}

/**
 * @brief Сохраняет uint32_t‑значение в NVS.
 *
 * @param key Ключ для значения (до 15 символов).
 * @param value Сохраняемое значение.
 * @return ESP_OK — успех, иначе код ошибки.
 */
esp_err_t nvs_storage_set_u32(
    const char *key,
    uint32_t value
) {
    const char *partition_name="storage";
    const char *namespace_name="config";
    nvs_handle_t handle;
    esp_err_t err;

    // Открываем namespace
    err = nvs_open_from_partition("storage", "config", NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open namespace '%s' in partition '%s': %s",
                 namespace_name, partition_name, esp_err_to_name(err));
        return err;
    }

    // Сохраняем значение
    err = nvs_set_u32(handle, key, value);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set u32 key '%s': %s", key, esp_err_to_name(err));
        nvs_close(handle);
        return err;
    }

    // Фиксируем изменения
    err = nvs_commit(handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to commit changes for key '%s': %s", key, esp_err_to_name(err));
        nvs_close(handle);
        return err;
    }

    ESP_LOGD(TAG, "Saved u32 key '%s' = %u in namespace '%s'", key, value, namespace_name);
    nvs_close(handle);
    return ESP_OK;
}

/**
 * @brief Читает uint32_t‑значение из NVS.
 *
 * @param key Ключ для значения.
 * @param out_value Указатель на переменную для сохранения результата.
 * @return ESP_OK — значение найдено и прочитано;
 *         ESP_ERR_NVS_NOT_FOUND — ключ не существует;
 *         иначе — код ошибки.
 */
esp_err_t nvs_storage_get_u32(
    const char *key,
    uint32_t *out_value
) {
    const char *partition_name="storage";
    const char *namespace_name="config";
    nvs_handle_t handle;
    esp_err_t err;

    // Открываем namespace
    err = nvs_open_from_partition(partition_name, namespace_name, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open namespace '%s' in partition '%s': %s",
                 namespace_name, partition_name, esp_err_to_name(err));
        return err;
    }

    // Читаем значение
    err = nvs_get_u32(handle, key, out_value);
    if (err != ESP_OK) {
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGW(TAG, "Key '%s' not found in namespace '%s'", key, namespace_name);
        } else {
            ESP_LOGE(TAG, "Failed to get u32 key '%s': %s", key, esp_err_to_name(err));
        }
        nvs_close(handle);
        return err;
    }

    ESP_LOGD(TAG, "Read u32 key '%s' = %u from namespace '%s'", key, *out_value, namespace_name);
    nvs_close(handle);
    return ESP_OK;
}

//show current partitions to LOG
void check_partitions(void) {
    esp_partition_iterator_t it = esp_partition_find(
        ESP_PARTITION_TYPE_DATA, 
        ESP_PARTITION_SUBTYPE_ANY, 
        NULL);
    
    while (it != NULL) {
        const esp_partition_t* p = esp_partition_get(it);
        ESP_LOGI("PART", "Found: %-12s @ 0x%06x size: %6d (%.1fKB) type:%d subtype:%d",
                p->label, p->address, p->size, p->size/1024.0,
                p->type, p->subtype);
        it = esp_partition_next(it);
    }
    esp_partition_iterator_release(it);
}

/**
 * @brief Деинициализирует раздел NVS (опционально).
 *
 * @param partition_name Имя раздела.
 * @return ESP_OK — успех, иначе код ошибки.
 */
esp_err_t nvs_storage_deinit(const char *partition_name) {
    esp_err_t err = nvs_flash_deinit_partition(partition_name);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to deinit NVS partition '%s': %s", partition_name, esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "NVS partition '%s' deinitialized", partition_name);
    }
    return err;
}

/*!
 *  @brief Set GPIO output and send MQTT msg of new state
 *
 *  @param[in] uRelayNumber     : relay number (0, 1 или 2)
 *  @param[in] uRelayNewState   : new Relay state
 *
 */
static void rly_set_output_num(uint32_t uRelayNumber, uint32_t uRelayNewState)
{
    if (uRelayNumber >= sizeof(rly_pins)/sizeof(rly_pins[0])) {
        ESP_LOGE(TAG, "\t[RELAY_SET]\tInvalid relay number: %lu", uRelayNumber);
        return;
    }
    
    if (uRelayNewState)
    {
        //Relay on
        ESP_LOGI(TAG, "\t[RELAY%lu_SET]\tON\t,%i", uRelayNumber+1, RELAY_ON_STATE);
        gpio_set_level(rly_pins[uRelayNumber], RELAY_ON_STATE);
    }
    else
    {
        //relay off
        ESP_LOGI(TAG, "\t[RELAY%lu_SET]\tOFF\t,%i", uRelayNumber+1, RELAY_OFF_STATE);
        gpio_set_level(rly_pins[uRelayNumber], RELAY_OFF_STATE);
    }
}

/*!
 *  @brief Callback fires when system time updated via SNTP.
 *
 *  @param[out] iSunRise   : SunRise time in minutes
 *  @param[out] iSunSet    : SunSet time in minutes
 *
 */
static void stc_SunTimeCB(int iSunRise, int iSunSet)
{
    if (xSemaphoreTake( rc_TimeMutex, pdMS_TO_TICKS(1900) )){
        uiOnTime=iSunSet;
        uiOffTime=iSunRise;
        xSemaphoreGive( rc_TimeMutex );
    }
    else
    {
        uiOnTime=480;
        uiOffTime=1200;
        ESP_LOGE(TAG, "\t[stc_SunTimeCB rc_TimeMutex take]\tFAIL");
    }        
}

// Callback таймера задержки работы реле 3
static void on_delay_timer_callback(TimerHandle_t xTimer) {
    ESP_LOGI(TAG,"relay 3 off by timer");
    Relay_Change_State(3, RELAY_OFF_STATE);
}

/*!
 *  @brief Relay auto control task. Check On and Off time every 30 seconds.
 *
 *  @param[in] pvParameters   : doesn't use
 *
 */
void Relay_Auto_Control_Task(void *pvParameters)
{
    static const char *TAG = "RLYAUTO";
    int32_t iCurTime;
    static int32_t iFiredOff = 0;    //for off run once
    static int32_t iFiredOn = 0;    //for on run once

    ESP_LOGI(TAG,"\tRelay_Auto_Control_task \t START");
    ESP_LOGD(TAG, "\t[Free memory]:\t%d bytes", esp_get_free_heap_size());
    //SNTP sync time
    rc_TimeMutex = xSemaphoreCreateMutex();
    sun_time_init(&stc_SunTimeCB);
    //wait for sync    
    if (stc_sync_wait(portMAX_DELAY) != ESP_OK) {
        ESP_LOGE(TAG,"\t[SNTP]    \tTIMEOUT ERROR");
        vTaskDelete(NULL);
    }    
    for(;;)
    {//check on and off every 30 seconds
        int32_t iloc_OnTime, iloc_OffTime;
        if (xSemaphoreTake( rc_TimeMutex, pdMS_TO_TICKS(1900) )){
            iloc_OnTime=uiOnTime;
            iloc_OffTime=uiOffTime;
            xSemaphoreGive( rc_TimeMutex );
        }
        else
        {
            iloc_OnTime=480;
            iloc_OffTime=1200;
            ESP_LOGE(TAG, "\t[Relay_Auto_Control_Task rc_TimeMutex take]\tFAIL");
        }        

        iCurTime = GetLocalTime();
        ESP_LOGD(TAG,"\t[Auto_CNTRL] \tTIMEOUT\ttm:%i;off:%u;on:%u",iCurTime,iloc_OffTime,iloc_OnTime);
        //check off time
        if ((iCurTime==iloc_OffTime)&&(iFiredOff==0))
        {
                ESP_LOGI(TAG,"\t[Auto_turn]\tOFF");
                iFiredOff=1;
                int8_t ilocAutoPower;
                if (xAutoPowerMutex == NULL) {
                    xAutoPowerMutex = xSemaphoreCreateMutex();
                    if (xAutoPowerMutex == NULL) {
                        ESP_LOGE(TAG, "\tFailed to create mutex for subscribed topics");
                        return;
                    }
                }
                
                ilocAutoPower = iAutoPower;
                xSemaphoreGive(xAutoPowerMutex);

                if ((ilocAutoPower&0x01)==0x01){
                    Relay_Change_State(0,RELAY_OFF_STATE);
                }
                if ((ilocAutoPower&0x02)==0x02){
                    Relay_Change_State(1,RELAY_OFF_STATE);
                }
                if ((ilocAutoPower&0x04)==0x04){  // Добавлено для третьего реле
                    Relay_Change_State(2,RELAY_OFF_STATE);
                }
        }//check on time 
        else if ((iCurTime==iloc_OnTime)&&(iFiredOn==0))
        {
                ESP_LOGI(TAG,"\t[Auto_turn]\tON");
                iFiredOn=1;
                int8_t ilocAutoPower;
                if (xAutoPowerMutex == NULL) {
                    xAutoPowerMutex = xSemaphoreCreateMutex();
                    if (xAutoPowerMutex == NULL) {
                        ESP_LOGE(TAG, "\tFailed to create mutex for subscribed topics");
                        return;
                    }
                }
                
                ilocAutoPower = iAutoPower;
                xSemaphoreGive(xAutoPowerMutex);
                if ((ilocAutoPower&0x01)==0x01){
                    Relay_Change_State(0,RELAY_ON_STATE);
                }
                if ((ilocAutoPower&0x02)==0x02){
                    Relay_Change_State(1,RELAY_ON_STATE);
                }
                if ((ilocAutoPower&0x04)==0x04){  // Добавлено для третьего реле
                    Relay_Change_State(2,RELAY_ON_STATE);
                }
        } 
        //reset one fire flag
        if ((iFiredOff!=0)&&(iCurTime!=iloc_OffTime))
        {
            iFiredOff=0;
        }
        //reset one fire flag
        if ((iFiredOn!=0)&&(iCurTime!=iloc_OnTime))
        {
            iFiredOn=0;
        }
        ESP_LOGD(TAG,"\t[iFired OFF/ON]\t%i / %i", iFiredOff, iFiredOn);
        vTaskDelay(pdMS_TO_TICKS(30000));
    }
}

/*!
 *  @brief Set auto ON/OFF state
 *
 *  @param[in] uRelayNewState   : new Relay state
 *
 */
void rly_Change_AutoPower(uint32_t uNewState)
{
    static TaskHandle_t xAutoTaskHndl=NULL;

    ESP_LOGI(TAG,"\t[Set Auto State init]\t%u", uNewState);
    if (uNewState)
    {
        if (xAutoTaskHndl==NULL)
        {        
            ESP_LOGD(TAG,"\t[Auto Task Create]");
            if (xTaskCreate(Relay_Auto_Control_Task, "Relay Auto Control Task", configMINIMAL_STACK_SIZE * 3, NULL, 5, &xAutoTaskHndl)==pdFAIL)
            {
                ESP_LOGE(TAG,"\t[xTaskCreate]\tFAIL - Relay Auto Control Task");
                xAutoTaskHndl=NULL;
            }else{
                ESP_LOGD(TAG,"\t[Auto Task Create]\tOK");
            }
        }else{
            ESP_LOGD(TAG,"\t[Task already exist]");
        }
    }
    else
    {
        if (xAutoTaskHndl!=NULL)
        {
            ESP_LOGI(TAG,"\t[Auto Task Delete]");
            vTaskDelete(xAutoTaskHndl);
            xAutoTaskHndl=NULL;
        }else{
            ESP_LOGD(TAG,"\t[Task doesn't exist]");
        }
    }

    if (xAutoTaskHndl==NULL)
    {
        rly_MQTT_MSG_send(TPC_RLY1_AUTO, 0);
        nvs_storage_set_u32(CFG_AUTO,0);
        nvs_storage_set_u32(CFG_AUTO2,0);
        nvs_storage_set_u32(CFG_AUTO3,0);
    }else
    {
        rly_MQTT_MSG_send(TPC_RLY1_AUTO, 1);
        nvs_storage_set_u32(CFG_AUTO,1);
        nvs_storage_set_u32(CFG_AUTO2,1);
        nvs_storage_set_u32(CFG_AUTO3,1);
    }
    
    
}

/*!
 *  @brief Relay control task. Wait message and change relay state
 *
 *  @param[in] pvParameters   : doesn't use
 *
 */
void Relay_Control_Task(void *pvParameters)
{
    static const char *TAG = "RLY_TSK";
    static RelayState_t xRelay1State,xRelay2State,xRelay3State;  // Добавлено для третьего реле
    Relay_Msg_t xRelay_Msg;
    esp_err_t err;

    ESP_LOGI(TAG,"\tRElay_Control_task \t START");
    ESP_LOGD(TAG, "\t[Free memory]:\t%d bytes", esp_get_free_heap_size());
    
    // Инициализация состояний реле
    xRelay1State.uState=RELAY_OFF_STATE;
    xRelay1State.uAutoState=0;
    xRelay2State.uState=RELAY_OFF_STATE;
    xRelay2State.uAutoState=0;
    xRelay3State.uState=RELAY_OFF_STATE;  // Добавлено
    xRelay3State.uAutoState=0;           // Добавлено
    
    // Загрузка состояния первого реле
    err = nvs_storage_get_u32(CFG_STATE, &xRelay1State.uState);
    if (err == ESP_OK) {
        ESP_LOGI(TAG,"Load relay 1 state value: %u", xRelay1State.uState);
    } else 
    {
        xRelay1State.uState=RLY_STATE_DEFAULT_VALUE;
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGW(TAG,"Key not found for relay 1 state");
        } else {
            ESP_LOGW(TAG,"Read failed for relay 1 state");
        }
    }

    err = nvs_storage_get_u32(CFG_AUTO, &xRelay1State.uAutoState);
    if (err == ESP_OK) {
        ESP_LOGI(TAG,"Load relay 1 auto value: %u\n", xRelay1State.uAutoState);
    } else 
    {
        xRelay1State.uAutoState=RLY_STATE_DEFAULT_VALUE;
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGW(TAG,"Key not found for relay 1 auto");
        } else {
            ESP_LOGW(TAG,"Read failed for relay 1 auto");
        }
    }

    // Загрузка состояния второго реле
    err = nvs_storage_get_u32(CFG_STATE2, &xRelay2State.uState);
    if (err == ESP_OK) {
        ESP_LOGI(TAG,"Load relay 2 state value: %u", xRelay2State.uState);
    } else 
    {
        xRelay2State.uState=RLY_STATE_DEFAULT_VALUE;
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGW(TAG,"Key not found for relay 2 state");
        } else {
            ESP_LOGW(TAG,"Read failed for relay 2 state");
        }
    }

    // Загрузка состояния третьего реле (добавлено)
    err = nvs_storage_get_u32(CFG_STATE3, &xRelay3State.uState);
    if (err == ESP_OK) {
        ESP_LOGI(TAG,"Load relay 3 state value: %u", xRelay3State.uState);
    } else 
    {
        xRelay3State.uState=RLY_STATE_DEFAULT_VALUE;
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGW(TAG,"Key not found for relay 3 state");
        } else {
            ESP_LOGW(TAG,"Read failed for relay 3 state");
        }
    }

    //set initial state
    rly_set_output_num(0,xRelay1State.uState);
    rly_set_output_num(1,xRelay2State.uState);
    rly_set_output_num(2,xRelay3State.uState);  // Добавлено
    
    rly_MQTT_MSG_send(TPC_RLY1, xRelay1State.uState);                
    rly_MQTT_MSG_send(TPC_RLY2, xRelay2State.uState);
    rly_MQTT_MSG_send(TPC_RLY3, xRelay3State.uState);  // Добавлено
    
    rly_Change_AutoPower(xRelay1State.uAutoState);
    ESP_LOGI(TAG,"\t[Start Main LOOP]\t...");
    
    while (1)
    {
        //wait for PWM Msg     
        if (xQueueReceive( xRelay_Msg_Queue, &xRelay_Msg, portMAX_DELAY )==pdPASS)
        {
            ESP_LOGD(TAG, "\t[MSG_RECEIVE]\tOK");
            ESP_LOGD(TAG, "\t[Free memory]:\t%d bytes", esp_get_free_heap_size());
            switch (xRelay_Msg.uMsgType)
            {
            case RLY_MSG_CONTROL:
                rly_set_output_num(0,xRelay_Msg.uMsg);
                rly_MQTT_MSG_send(TPC_RLY1, xRelay_Msg.uMsg);  
                nvs_storage_set_u32(CFG_STATE,xRelay_Msg.uMsg);              
                break;
            case RLY2_MSG_CONTROL:
                rly_set_output_num(1,xRelay_Msg.uMsg);
                rly_MQTT_MSG_send(TPC_RLY2, xRelay_Msg.uMsg);  
                nvs_storage_set_u32(CFG_STATE2,xRelay_Msg.uMsg);              
                break;
            case RLY3_MSG_CONTROL:  // Добавлено для третьего реле
                rly_set_output_num(2,xRelay_Msg.uMsg);
                rly_MQTT_MSG_send(TPC_RLY3, xRelay_Msg.uMsg);  
                xTimerStart(on_delay_timer,0);
                // nvs_storage_set_u32(CFG_STATE3,xRelay_Msg.uMsg);              
                break;
            default:
                ESP_LOGW(TAG, "\tUNKNOWN MESSAGE");
                break;
            }
        }
    }            
};

/*!
 *  @brief add msg to Relay_msg_queue
 *
 *  @param[in] uRelayNumber:    Relay Nuber 0 for 1st, 1 for second, 2 for third
 *  @param[in] uNewState   :    Desireable relay new state
 *
 */
void Relay_Change_State(uint32_t uRelayNumber, uint32_t uNewState)
{
    Relay_Msg_t xRelay_Msg;
    BaseType_t xStatus;
    
    switch (uRelayNumber)
    {
    case 0:
        xRelay_Msg.uMsgType=RLY_MSG_CONTROL;
        break;
    case 1:
        xRelay_Msg.uMsgType=RLY2_MSG_CONTROL;
        break;
    case 2:
        xRelay_Msg.uMsgType=RLY3_MSG_CONTROL;  // Добавлено для третьего реле
        break;
    default:
        ESP_LOGE(TAG, "\t[Relay_Change_State]\tInvalid relay number: %lu", uRelayNumber);
        return;
    }
    
    xRelay_Msg.uMsg=uNewState;
    xStatus = xQueueSendToBack( xRelay_Msg_Queue, &xRelay_Msg, 0 );
    QUEUE_check_rslt("Relay queue msg send", xStatus);
};

/*!
 *  @brief Convert String to int32 via strtol() and checks some errors
 *
 *  @param[in] pcMessage         : input string
 *
 *  @retval -1 if  there are error
 *  @retval >=0 in other cases 
 */
static int32_t cmn_String_to_int32(const char *pcMessage) {
    char *endptr;
    uint32_t uState;

    // pcTmp=malloc(sizeof(pcMessage));
    // memcpy(pcTmp, pcMessage, sizeof(pcMessage));

    uState=strtol(pcMessage, &endptr, 10);
    if (endptr==pcMessage)
    {
        ESP_LOGW(TAG,"\t[MQTT MSG ERR]\t%s",pcMessage);
        return -1;
    } else if (*endptr != '\0')
    {
        ESP_LOGW(TAG,"\t[MQTT MSG ERR]\t%c", *endptr);
        return -1;
    } else 
    {
        return uState;
    }
}

/*!
 *  @brief Run when MQTT client receive MSG in /Relay1 topic
 *
 *  @param[in] pcMessage         : contain data that was received
 *
 */
static void rly_MQTT_Callback1(const char *pcMessage) {
    uint32_t uState;

    //Convert to int
    uState=cmn_String_to_int32(pcMessage);
    if (uState==0){
        ESP_LOGI(TAG,"\t[RELAY MSG RCV] \tTURN OFF");
        Relay_Change_State(0, RELAY_OFF_STATE);
    } else if (uState==1){
        ESP_LOGI(TAG,"\t[RELAY MSG RCV] \tTURN ON");
        Relay_Change_State(0, RELAY_ON_STATE);
    }else{
        ESP_LOGW(TAG, "[MQTT RELAY MSG] \tOUT OF BOUNDS: %u",uState);
    }
    // free(pcTmp);
}

/*!
 *  @brief Run when MQTT client receive MSG in /Relay1/Auto topic
 *
 *  @param[in] pcMessage         : contain data that was received
 *
 */
static void rly_MQTT_auto_Callback1(const char *pcMessage) {
    uint32_t uState;

    // pcTmp=malloc(sizeof(pcMessage));
    // memcpy(pcTmp, pcMessage, sizeof(pcMessage));
    

    uState=cmn_String_to_int32(pcMessage);
    if (uState==0){
        ESP_LOGI(TAG,"\t[RELAY MSG RCV] \tAUTO TURN OFF");
    
        if (xAutoPowerMutex == NULL) {
            xAutoPowerMutex = xSemaphoreCreateMutex();
            if (xAutoPowerMutex == NULL) {
                ESP_LOGE(TAG, "\tFailed to create mutex for subscribed topics");
                return;
            }
        }
        iAutoPower=iAutoPower&0xFE;
        xSemaphoreGive(xAutoPowerMutex);
    
        rly_Change_AutoPower(0);
    } else if (uState==1){
        ESP_LOGI(TAG,"\t[RELAY MSG RCV] \tAUTO TURN ON");
        if (xAutoPowerMutex == NULL) {
            xAutoPowerMutex = xSemaphoreCreateMutex();
            if (xAutoPowerMutex == NULL) {
                ESP_LOGE(TAG, "\tFailed to create mutex for subscribed topics");
                return;
            }
        }
        iAutoPower=iAutoPower|0x01;
        xSemaphoreGive(xAutoPowerMutex);
        rly_Change_AutoPower(1);
    }else{
        ESP_LOGW(TAG, "[MQTT RELAY MSG] \tOUT OF BOUNDS: %u",uState);
    }
    // free(pcTmp);
}

/*!
 *  @brief Run when MQTT client receive MSG in /Relay2 topic
 *
 *  @param[in] pcMessage         : contain data that was received
 *
 */
static void rly_MQTT_Callback2(const char *pcMessage) {
    uint32_t uState;

    //Convert to int
    uState=cmn_String_to_int32(pcMessage);
    if (uState==0){
        ESP_LOGI(TAG,"\t[RELAY2 MSG RCV] \tTURN OFF");
        Relay_Change_State(1, RELAY_OFF_STATE);
    } else if (uState==1){
        ESP_LOGI(TAG,"\t[RELAY2 MSG RCV] \tTURN ON");
        Relay_Change_State(1, RELAY_ON_STATE);
    }else{
        ESP_LOGW(TAG, "[MQTT RELAY2 MSG] \tOUT OF BOUNDS: %u",uState);
    }
}

/*!
 *  @brief Run when MQTT client receive MSG in /Relay3 topic
 *
 *  @param[in] pcMessage         : contain data that was received
 *
 */
static void rly_MQTT_Callback3(const char *pcMessage) {  // Добавлена новая функция
    uint32_t uState;

    //Convert to int
    uState=cmn_String_to_int32(pcMessage);
    if (uState==0){
        ESP_LOGI(TAG,"\t[RELAY3 MSG RCV] \tTURN OFF");
        Relay_Change_State(2, RELAY_OFF_STATE);
    } else if (uState==1){
        ESP_LOGI(TAG,"\t[RELAY3 MSG RCV] \tTURN ON");
        Relay_Change_State(2, RELAY_ON_STATE);
    }else{
        ESP_LOGW(TAG, "[MQTT RELAY3 MSG] \tOUT OF BOUNDS: %u",uState);
    }
}

/* Остальные функции остаются без изменений до функции rly_MQTT_auto_Callback2 */

/*!
 *  @brief Run when MQTT client receive MSG in /Relay2/Auto topic
 *
 *  @param[in] pcMessage         : contain data that was received
 *
 */
static void rly_MQTT_auto_Callback2(const char *pcMessage) {
    uint32_t uState;

    uState=cmn_String_to_int32(pcMessage);
    if (uState==0){
        ESP_LOGI(TAG,"\t[RELAY2 MSG RCV] \tAUTO TURN OFF");
        if (xAutoPowerMutex == NULL) {
            xAutoPowerMutex = xSemaphoreCreateMutex();
            if (xAutoPowerMutex == NULL) {
                ESP_LOGE(TAG, "\tFailed to create mutex for subscribed topics");
                return;
            }
        }
        iAutoPower=iAutoPower&0xFD; //0b11111101
        xSemaphoreGive(xAutoPowerMutex);
        rly_Change_AutoPower(0);
    } else if (uState==1){
        ESP_LOGI(TAG,"\t[RELAY2 MSG RCV] \tAUTO TURN ON");
        if (xAutoPowerMutex == NULL) {
            xAutoPowerMutex = xSemaphoreCreateMutex();
            if (xAutoPowerMutex == NULL) {
                ESP_LOGE(TAG, "\tFailed to create mutex for subscribed topics");
                return;
            }
        }
        iAutoPower=iAutoPower|0x02;
        xSemaphoreGive(xAutoPowerMutex);
        rly_Change_AutoPower(1);
    }else{
        ESP_LOGW(TAG, "[MQTT RELAY2 MSG] \tOUT OF BOUNDS: %u",uState);
    }
}

/*!
 *  @brief Run when MQTT client receive MSG in /Relay3/Auto topic
 *
 *  @param[in] pcMessage         : contain data that was received
 *
 */
static void rly_MQTT_auto_Callback3(const char *pcMessage) {  // Добавлена новая функция
    uint32_t uState;

    uState=cmn_String_to_int32(pcMessage);
    if (uState==0){
        ESP_LOGI(TAG,"\t[RELAY3 MSG RCV] \tAUTO TURN OFF");
        if (xAutoPowerMutex == NULL) {
            xAutoPowerMutex = xSemaphoreCreateMutex();
            if (xAutoPowerMutex == NULL) {
                ESP_LOGE(TAG, "\tFailed to create mutex for subscribed topics");
                return;
            }
        }
        iAutoPower=iAutoPower&0xFB; //0b11111011
        xSemaphoreGive(xAutoPowerMutex);
        rly_Change_AutoPower(0);
    } else if (uState==1){
        ESP_LOGI(TAG,"\t[RELAY3 MSG RCV] \tAUTO TURN ON");
        if (xAutoPowerMutex == NULL) {
            xAutoPowerMutex = xSemaphoreCreateMutex();
            if (xAutoPowerMutex == NULL) {
                ESP_LOGE(TAG, "\tFailed to create mutex for subscribed topics");
                return;
            }
        }
        iAutoPower=iAutoPower|0x04;
        xSemaphoreGive(xAutoPowerMutex);
        rly_Change_AutoPower(1);
    }else{
        ESP_LOGW(TAG, "[MQTT RELAY3 MSG] \tOUT OF BOUNDS: %u",uState);
    }
}

/*!
 *  @brief Init relay_control unit and start Relay_Control Task
 *
 *  @param[in] cfg   : unit config structure
 *
 */
void Relay_Control_Init()
{
    ESP_LOGD(TAG,"\tRelay Config started\t");
    xRelay_Msg_Queue = xQueueCreate( 5, sizeof( Relay_Msg_t ) );
    
    //GPIO
    gpio_config_t io_conf;
    //disable interrupt
    io_conf.intr_type = GPIO_INTR_DISABLE;
    //set as output mode
    io_conf.mode = GPIO_MODE_OUTPUT;
    //bit mask of the pins that you want to set
    io_conf.pin_bit_mask = GPIO_OUTPUT_PIN_SEL;
    //disable pull-down mode
    io_conf.pull_down_en = 0;
    //disable pull-up mode
    io_conf.pull_up_en = 0;
    //configure GPIO with the given settings
    gpio_config(&io_conf);

    // 1. Инициализация раздела
    esp_err_t err;
    err = nvs_storage_init("storage");
    if (err != ESP_OK) {
        ESP_LOGW(TAG,"NVS init failed");
    }
    on_delay_timer = xTimerCreate(
        "on_delay",
        pdMS_TO_TICKS(RELAY3_ON_TIME),
        pdFALSE,
        NULL,
        on_delay_timer_callback
    );

    mqtt_init();
    mqtt_Topic_Subsribe(pcControlTopics[0],&rly_MQTT_Callback1);
    mqtt_Topic_Subsribe(pcControlTopics[1],&rly_MQTT_auto_Callback1);
    mqtt_Topic_Subsribe(pcControlTopics[2],&rly_MQTT_Callback2);
    mqtt_Topic_Subsribe(pcControlTopics[3],&rly_MQTT_auto_Callback2);
    mqtt_Topic_Subsribe(pcControlTopics[4],&rly_MQTT_Callback3);      // Добавлено
    mqtt_Topic_Subsribe(pcControlTopics[5],&rly_MQTT_auto_Callback3); // Добавлено
    
    xTaskCreate(Relay_Control_Task, "Relay Control Task", configMINIMAL_STACK_SIZE * 3, NULL, 5, NULL);
};