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
 *  @param[in] cfg   : unit config structure
 *
 */

void Relay_Control_Init();

/*!
 *  @brief add msg to Relay_msg_queue
 *
 *  @param[in] uRelayNumber:    Relay Nuber 0 for 1st, 1 for second, 2 for third
 *  @param[in] uNewState   :    Desireable relay new state
 *
 */
void Relay_Change_State(uint32_t uRelayNumber, uint32_t uNewState);

void Relay_Fan_Off(void);
void Relay_Fan_On(void);
