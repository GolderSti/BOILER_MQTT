#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <float.h>
#include <sys/stat.h>
#include "esp_system.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "relay_control.h"
#include "relay_io.h"
#include "relay_storage.h"
#include "mqtt_common.h"
#include "esp_log.h"
#include "esp_sntp.h"
#include "sun_time_common.h"

#define RELAY3_ON_TIME 15*60*1000 //15 минут в миллисикундах...
static const char *TAG = "RLYCNTR";
#define MAX_MESSAGE_LENGTH 50
#define TPC_RLY1 0
#define TPC_RLY1_AUTO 1
#define TPC_RLY2 2
#define TPC_RLY2_AUTO 3
#define TPC_FAN 4
static const char *pcTopics[] = {                    
                                [TPC_RLY1]="/Relay1",
                                [TPC_RLY1_AUTO]="/Relay1/Auto",
                                [TPC_RLY2]="/Relay2",
                                [TPC_RLY2_AUTO]="/Relay2/Auto",
                                [TPC_FAN]="/Fan",
                                };

#define CONTROL_TOPICS_COUNT 5
static const char *pcControlTopics[] = {
                                        "/Relay1/Set",       //0
                                        "/Relay1/Auto/Set",  //1
                                        "/Relay2/Set",       //2
                                        "/Relay2/Auto/Set",  //3
                                        "/Fan/Set",          //4
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

/*!
 *  @brief Set GPIO output and send MQTT msg of new state
 *
 *  @param[in] uRelayNumber     : relay number (0, 1 или 2)
 *  @param[in] uRelayNewState   : new Relay state
 *
 */
static void rly_set_output_num(uint32_t uRelayNumber, uint32_t uRelayNewState)
{
    esp_err_t err = Relay_IO_Set(uRelayNumber, uRelayNewState);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "\t[RELAY%lu_SET]\tFailed", uRelayNumber + 1);
        return;
    }

    ESP_LOGI(TAG,
             "\t[RELAY%lu_SET]\t%s\t,%lu",
             uRelayNumber + 1,
             Relay_IO_GetState(uRelayNumber) == RELAY_ON_STATE ? "ON" : "OFF",
             Relay_IO_GetState(uRelayNumber));
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
    // ESP_LOGI(TAG,"relay 3 off by timer");
    Relay_Change_State(2, RELAY_OFF_STATE);
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
        relay_storage_set_u32(CFG_AUTO,0);
        relay_storage_set_u32(CFG_AUTO2,0);
    }else
    {
        rly_MQTT_MSG_send(TPC_RLY1_AUTO, 1);
        relay_storage_set_u32(CFG_AUTO,1);
        relay_storage_set_u32(CFG_AUTO2,1);
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
    xRelay3State.uState=RELAY_OFF_STATE;
    xRelay3State.uAutoState=0;
    
    // Загрузка состояния первого реле
    err = relay_storage_get_u32(CFG_STATE, &xRelay1State.uState);
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

    err = relay_storage_get_u32(CFG_AUTO, &xRelay1State.uAutoState);
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
    err = relay_storage_get_u32(CFG_STATE2, &xRelay2State.uState);
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

    // Состояние вентилятора всегда стартует выключенным.
    xRelay3State.uState = RELAY_OFF_STATE;

    //set initial state
    rly_set_output_num(0,xRelay1State.uState);
    rly_set_output_num(1,xRelay2State.uState);
    rly_set_output_num(2,xRelay3State.uState);  // Добавлено
    
    rly_MQTT_MSG_send(TPC_RLY1, xRelay1State.uState);                
    rly_MQTT_MSG_send(TPC_RLY2, xRelay2State.uState);
    rly_MQTT_MSG_send(TPC_FAN, xRelay3State.uState);
    
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
                relay_storage_set_u32(CFG_STATE,xRelay_Msg.uMsg);              
                break;
            case RLY2_MSG_CONTROL:
                rly_set_output_num(1,xRelay_Msg.uMsg);
                rly_MQTT_MSG_send(TPC_RLY2, xRelay_Msg.uMsg);  
                relay_storage_set_u32(CFG_STATE2,xRelay_Msg.uMsg);              
                break;
            case RLY3_MSG_CONTROL:  // Добавлено для третьего реле
                rly_set_output_num(2,xRelay_Msg.uMsg);
                rly_MQTT_MSG_send(TPC_FAN, xRelay_Msg.uMsg);  
                if (xRelay_Msg.uMsg == RELAY_ON_STATE) {
                    xTimerStart(on_delay_timer,0);
                } else {
                    xTimerStop(on_delay_timer, 0);
                }
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
    // QUEUE_check_rslt("Relay queue msg send", xStatus);
};


void Relay_Fan_Off(void)
{
    Relay_Change_State(2, RELAY_OFF_STATE);
}

void Relay_Fan_On(void)
{
    Relay_Change_State(2, RELAY_ON_STATE);
}

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
 *  @brief Run when MQTT client receive MSG in /Fan topic
 *
 *  @param[in] pcMessage         : contain data that was received
 *
 */
static void rly_MQTT_Callback3(const char *pcMessage) {  // Добавлена новая функция
    uint32_t uState;

    //Convert to int
    uState=cmn_String_to_int32(pcMessage);
    if (uState==0){
        ESP_LOGI(TAG,"\t[FAN MSG RCV] \tTURN OFF");
        Relay_Change_State(2, RELAY_OFF_STATE);
    } else if (uState==1){
        ESP_LOGI(TAG,"\t[FAN MSG RCV] \tTURN ON");
        Relay_Change_State(2, RELAY_ON_STATE);
    }else{
        ESP_LOGW(TAG, "[MQTT FAN MSG] \tOUT OF BOUNDS: %u",uState);
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
 *  @brief Init relay_control unit and start Relay_Control Task
 *
 *  @param[in] cfg   : unit config structure
 *
 */
void Relay_Control_Init()
{
    ESP_LOGD(TAG,"\tRelay Config started\t");
    xRelay_Msg_Queue = xQueueCreate( 5, sizeof( Relay_Msg_t ) );
    
    if (Relay_IO_Init() != ESP_OK) {
        ESP_LOGE(TAG, "Relay GPIO init failed");
        return;
    }

    // 1. Инициализация раздела
    esp_err_t err;
    err = relay_storage_init("storage");
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
    mqtt_Topic_Subsribe(pcControlTopics[4],&rly_MQTT_Callback3);
    
    xTaskCreate(Relay_Control_Task, "Relay Control Task", configMINIMAL_STACK_SIZE * 3, NULL, 5, NULL);
};
