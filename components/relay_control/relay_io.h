#pragma once

#include <stdint.h>
#include "esp_err.h"

#define RELAY_OUTPUT_IO_1   4
#define RELAY_OUTPUT_IO_2   5
#define RELAY_OUTPUT_IO_3   6
#define RELAY_COUNT         3

esp_err_t Relay_IO_Init(void);
esp_err_t Relay_IO_Set(uint32_t relay_number, uint32_t relay_state);
esp_err_t Relay_IO_On(uint32_t relay_number);
esp_err_t Relay_IO_Off(uint32_t relay_number);
uint32_t Relay_IO_GetState(uint32_t relay_number);
