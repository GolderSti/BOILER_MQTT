#pragma once

#include <stdint.h>

// #include "freertos/FreeRTOS.h"
// #include "freertos/task.h"
// #include "freertos/semphr.h"
// #include "freertos/queue.h"

#define RELAY_ON_STATE 1
#define RELAY_OFF_STATE 0
#define RLY_STATE_DEFAULT_VALUE RELAY_OFF_STATE
#define RLY_MAIN_CONTROL 1
#define RLY_MIRROR_CONTROL 2
#define RLY_FAN_CONTROL 3
#define RLY_LIGHTMODE_CONTROL 4

enum Light_States {
    ALL_OFF = 0,
    MIRROR_ON,
    MAIN_ON,
    ALL_ON
};

typedef struct
{
    uint32_t uMsgType;
    uint32_t uMsg;
} Relay_Msg_t;

/*!
 *  @brief Init relay_control unit and start Relay_Control Task
 *
 */

void Relay_Control_Init();

void Relay_Fan_Off(void);
void Relay_Fan_On(void);
void Relay_Light_Off(void);
void Relay_Light_On(void);
void Relay_MainLight_Off(void);
void Relay_MainLight_On(void);
void Relay_MirrorLight_Off(void);
void Relay_MirrorLight_On(void);
void Relay_LoopLightMode(void);
void Relay_SetLightMode(uint32_t new_mode);
uint32_t Relay_GetLightMode(void);
enum Light_States Relay_GetLightState(void);