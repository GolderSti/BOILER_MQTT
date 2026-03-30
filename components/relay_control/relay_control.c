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
#include "mqtt_common.h"
#include "esp_log.h"
#include "esp_sntp.h"
#include "sun_time_common.h"

#define FAN_AUTO_OFF_DELAY_MS (15U * 60U * 1000U) // 15 минут в миллисекундах
#define FAN_AUTO_OFF_TIMER_NAME "fan_auto_off"
static const char *TAG = "RLYCNTR";
#define MAX_MESSAGE_LENGTH 50
#define TPC_LIGHT 0
#define TPC_MAIN_LIGHT 1
#define TPC_MIRROR_LIGHT 2
#define TPC_FAN 3
#define TPC_LIGHTMODE 4
static const char *pcTopics[] = {                    
                                [TPC_LIGHT]="/Light",
                                [TPC_MAIN_LIGHT]="/MainLight",
                                [TPC_MIRROR_LIGHT]="/MirrorLight",
                                [TPC_FAN]="/Fan",
                                [TPC_LIGHTMODE]="/LightMode"
                                };

#define CONTROL_TOPICS_COUNT 5
static const char *pcControlTopics[] = {
                                        "/Light/Set",       //0
                                        "/MainLight/Set",   //1
                                        "/MirrorLight/Set", //2
                                        "/Fan/Set",         //3
                                        "/LightMode/Set"
                                        };

// static const char *LightStatesStrings[] = {                    
//                                 [ALL_OFF]="ALL_OFF",
//                                 [MIRROR_ON]="MIRROR_ON",
//                                 [MAIN_ON]="MAIN_ON",
//                                 [ALL_ON]="ALL_ON"
// };



typedef struct 
{
    uint32_t uState;
} RelayState_t;

typedef struct
{
    uint32_t LightMode;    //what to turn on on Light_ON: 01 - mirror, 10 - main, 11 - all
    enum Light_States curLightState;//current light state
} LightState_t;

static QueueHandle_t xRelay_Msg_Queue;
static TimerHandle_t s_fan_auto_off_timer = NULL;
static LightState_t state;

void Relay_Change_State(uint32_t uRelayNumber, uint32_t uNewState);
/*!
 * Expand Queue rslt to string
 */

static void QUEUE_check_rslt(const char api_name[], BaseType_t rslt)
{
    switch (rslt)
    {
        // pdPASS = 0,                                    /*!< Function execution successful */
        case pdPASS:

            // ESP_LOGV(TAG,"\t[%s]\tOK", api_name);
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
            if (uTopicNmb == TPC_LIGHTMODE)
            {
                // sprintf(pMessage, "%s", LightStatesStrings[uMsg]);
                sprintf(pMessage,"%lu",uMsg);
            }else
            {
                sprintf(pMessage,"%lu",uMsg);
            }
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

// Callback таймера задержки работы реле вентилятора
static void on_fan_auto_off_timer_callback(TimerHandle_t xTimer)
{
    Relay_Change_State(2, RELAY_OFF_STATE);
}

//Вспомагательная функция инициализации таймера задержки работы вентилятора
static esp_err_t fan_auto_off_timer_init(void)
{
    if (s_fan_auto_off_timer != NULL) {
        return ESP_OK;
    }

    s_fan_auto_off_timer = xTimerCreate(
        FAN_AUTO_OFF_TIMER_NAME,
        pdMS_TO_TICKS(FAN_AUTO_OFF_DELAY_MS),
        pdFALSE,
        NULL,
        on_fan_auto_off_timer_callback
    );

    if (s_fan_auto_off_timer == NULL) {
        ESP_LOGE(TAG, "Failed to create fan auto-off timer");
        return ESP_FAIL;
    }

    return ESP_OK;
}


/*!
 *  @brief convert uLightState to LightState
 *
 *  @param[in] uiLState:    uiLightState
 *
 */
void LightState_Change(uint32_t uiLState)
{
    switch (uiLState)
    {
    case 00:
        if (state.curLightState != ALL_OFF)
        {
            rly_MQTT_MSG_send(TPC_LIGHT,0);
        }
        state.curLightState = ALL_OFF;
        ESP_LOGD(TAG, "\tLightState:\tALL_OFF");
        break;
    case 1:
        if (state.curLightState == ALL_OFF)
        {
            rly_MQTT_MSG_send(TPC_LIGHT,1);
        }
        state.curLightState = MAIN_ON;
        ESP_LOGD(TAG, "\tLightState:\tMAIN_ON");
        break;
    case 2:
        if (state.curLightState == ALL_OFF)
        {
            rly_MQTT_MSG_send(TPC_LIGHT,1);
        }
        state.curLightState = MIRROR_ON;
        ESP_LOGD(TAG, "\tLightState:\tMIRROR_ON");
        break;
    case 3:
        if (state.curLightState == ALL_OFF)
        {
            rly_MQTT_MSG_send(TPC_LIGHT,1);
        }
        state.curLightState = ALL_ON;
        ESP_LOGD(TAG, "\tLightState:\tALL_ON");
        break;

    default:
        ESP_LOGE(TAG, "\tLight State out of bounds:%lu", uiLState);
        break;
    }
};

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
    static uint32_t uLightState = 0; // 00 - all off, 01 - mirror, 10 - main, 11 - all on

    ESP_LOGI(TAG,"\tRElay_Control_task \t START");
    ESP_LOGD(TAG, "\t[Free memory]:\t%d bytes", esp_get_free_heap_size());
    
    // Инициализация состояний реле
    xRelay1State.uState=RLY_STATE_DEFAULT_VALUE;
    xRelay2State.uState=RLY_STATE_DEFAULT_VALUE;
    uLightState = 0;
    xRelay3State.uState=RELAY_OFF_STATE;

    //set initial state
    rly_set_output_num(0,xRelay1State.uState);
    rly_MQTT_MSG_send(TPC_MAIN_LIGHT, xRelay1State.uState);
    rly_set_output_num(1,xRelay2State.uState);
    rly_MQTT_MSG_send(TPC_MIRROR_LIGHT, xRelay2State.uState);
    rly_set_output_num(2,xRelay3State.uState);  
    rly_MQTT_MSG_send(TPC_FAN, xRelay3State.uState);
    rly_MQTT_MSG_send(TPC_LIGHT,0);
    rly_MQTT_MSG_send(TPC_LIGHTMODE, state.LightMode);

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
            case RLY_MAIN_CONTROL:
                rly_set_output_num(0,xRelay_Msg.uMsg);
                if (xRelay_Msg.uMsg)
                {
                    uLightState |= 2; //0b10
                }else
                {
                    uLightState &= 1; //0b01
                }
                LightState_Change(uLightState);
                rly_MQTT_MSG_send(TPC_MAIN_LIGHT, xRelay_Msg.uMsg);  
                break;
            case RLY_MIRROR_CONTROL:
                rly_set_output_num(1,xRelay_Msg.uMsg);
                if (xRelay_Msg.uMsg)
                {
                    uLightState |= 1; //0b01
                }else
                {
                    uLightState &= 2; //0b10
                }
                LightState_Change(uLightState);
                rly_MQTT_MSG_send(TPC_MIRROR_LIGHT, xRelay_Msg.uMsg);  
                break;
            case RLY_FAN_CONTROL:  // Добавлено для третьего реле
                rly_set_output_num(2,xRelay_Msg.uMsg);
                rly_MQTT_MSG_send(TPC_FAN, xRelay_Msg.uMsg);  
                if (s_fan_auto_off_timer != NULL) {
                    if (xRelay_Msg.uMsg == RELAY_ON_STATE) {
                        xTimerStart(s_fan_auto_off_timer, 0);
                    } else {
                        xTimerStop(s_fan_auto_off_timer, 0);
                    }
                }
                break;
            case RLY_LIGHTMODE_CONTROL:
                state.LightMode = xRelay_Msg.uMsg;
                rly_MQTT_MSG_send(TPC_LIGHTMODE, xRelay_Msg.uMsg);  
                break;
            default:
                ESP_LOGW(TAG, "\tUNKNOWN MESSAGE");
                break;
            }
        }
    }
}

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
        xRelay_Msg.uMsgType=RLY_MAIN_CONTROL;
        break;
    case 1:
        xRelay_Msg.uMsgType=RLY_MIRROR_CONTROL;
        break;
    case 2:
        xRelay_Msg.uMsgType=RLY_FAN_CONTROL;  // Добавлено для третьего реле
        break;
    case 3:
        xRelay_Msg.uMsgType=RLY_LIGHTMODE_CONTROL;  
        break;
    default:
        ESP_LOGE(TAG, "\t[Relay_Change_State]\tInvalid relay number: %lu", uRelayNumber);
        return;
    }
    
    xRelay_Msg.uMsg=uNewState;
    xStatus = xQueueSendToBack( xRelay_Msg_Queue, &xRelay_Msg, 0 );
    QUEUE_check_rslt("Relay queue msg send", xStatus);
};


void Relay_Fan_Off(void)
{
    Relay_Change_State(2, RELAY_OFF_STATE);
}

void Relay_Fan_On(void)
{
    Relay_Change_State(2, RELAY_ON_STATE);
}

void Relay_Light_Off(void)
{
    if (Relay_IO_GetState(0) == RELAY_ON_STATE)
    {
        Relay_Change_State(0, RELAY_OFF_STATE);    
    }
    if (Relay_IO_GetState(1) == RELAY_ON_STATE)
    {
        Relay_Change_State(1, RELAY_OFF_STATE);    
    }    
    rly_MQTT_MSG_send(TPC_LIGHT, 0);
}

void Relay_Light_On(void)
{
    if ((state.LightMode & 2) != 0)
    {
        Relay_Change_State(0, RELAY_ON_STATE);    
    }
    if ((state.LightMode & 1) != 0)
    {
        Relay_Change_State(1, RELAY_ON_STATE);    
    }    
    rly_MQTT_MSG_send(TPC_LIGHT, 1);
}

void Relay_MainLight_Off(void)
{
    Relay_Change_State(0, RELAY_OFF_STATE);
}

void Relay_MainLight_On(void)
{
    Relay_Change_State(0, RELAY_ON_STATE);
}

void Relay_MirrorLight_Off(void)
{
    Relay_Change_State(1, RELAY_OFF_STATE);
}

void Relay_MirrorLight_On(void)
{
    Relay_Change_State(1, RELAY_ON_STATE);
}

void Relay_LoopLightMode(void)
{
    uint32_t uLightMode;
    uLightMode = Relay_GetLightMode();
    uLightMode += 1;
    if (uLightMode>3)
    {
        uLightMode = 1;
    }
    Relay_SetLightMode(uLightMode);
}

void Relay_SetLightMode(uint32_t new_mode)
{
    if ((new_mode > 3))
    {
        ESP_LOGW(TAG, "\t[Relay_SetLightMode]\tLight Mode Out of Bounds: %lu", new_mode);
        return;
    }
    Relay_Change_State(3,new_mode);
}

uint32_t Relay_GetLightMode(void)
{
    return state.LightMode;
}

enum Light_States Relay_GetLightState(void)
{
    return state.curLightState;
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
 *  @brief Run when MQTT client receive MSG in /Light topic
 *
 *  @param[in] pcMessage         : contain data that was received
 *
 */
static void rly_MQTT_Callback0(const char *pcMessage) {
    uint32_t uState;

    //Convert to int
    uState=cmn_String_to_int32(pcMessage);
    if (uState==0){
        ESP_LOGI(TAG,"\t[LIGHT MSG RCV] \tTURN OFF");
        Relay_Light_Off();
    } else if (uState==1){
        ESP_LOGI(TAG,"\t[LIGHT MSG RCV] \tTURN ON");
        Relay_Light_On();
    }else{
        ESP_LOGW(TAG, "[LIGHT RELAY MSG] \tOUT OF BOUNDS: %u",uState);
    }
    // free(pcTmp);
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
        Relay_Fan_Off();
    } else if (uState==1){
        ESP_LOGI(TAG,"\t[FAN MSG RCV] \tTURN ON");
        Relay_Fan_On();
    }else{
        ESP_LOGW(TAG, "[MQTT FAN MSG] \tOUT OF BOUNDS: %u",uState);
    }
}

/*!
 *  @brief Run when MQTT client receive MSG in /Fan topic
 *
 *  @param[in] pcMessage         : contain data that was received
 *
 */
static void rly_MQTT_Callback4(const char *pcMessage) {  // Добавлена новая функция
    uint32_t uState;

    //Convert to int
    uState=cmn_String_to_int32(pcMessage);
    if (uState<=3){
        ESP_LOGI(TAG,"\t[LightMode MSG RCV]\tSet mode: %lu", uState);
        Relay_SetLightMode(uState);
    }else{
        ESP_LOGW(TAG, "[LightMode MSG]\tOUT OF BOUNDS: %lu",uState);
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

    if (fan_auto_off_timer_init() != ESP_OK) {
        return;
    }

    state.curLightState=ALL_OFF;
    state.LightMode=3;
    mqtt_init();
    mqtt_Topic_Subsribe(pcControlTopics[0],&rly_MQTT_Callback0);
    mqtt_Topic_Subsribe(pcControlTopics[1],&rly_MQTT_Callback1);
    mqtt_Topic_Subsribe(pcControlTopics[2],&rly_MQTT_Callback2);
    mqtt_Topic_Subsribe(pcControlTopics[3],&rly_MQTT_Callback3);
    mqtt_Topic_Subsribe(pcControlTopics[4],&rly_MQTT_Callback4);
    
    xTaskCreate(Relay_Control_Task, "Relay Control Task", configMINIMAL_STACK_SIZE * 3, NULL, 5, NULL);
};
