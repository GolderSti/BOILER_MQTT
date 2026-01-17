// #include "freertos/FreeRTOS.h"
// #include "freertos/task.h"
// #include "freertos/semphr.h"
// #include "freertos/queue.h"

#define RELAY_ON_STATE 1
#define RELAY_OFF_STATE 0
#define RLY_STATE_DEFAULT_VALUE 0;
#define RLY_MSG_CONTROL 1

typedef struct
{
    uint32_t uMsgType;
    uint32_t uMsg;
} Relay_Msg_t;

void Relay_Control_Init();
void Relay_Change_State(uint32_t uNewState);