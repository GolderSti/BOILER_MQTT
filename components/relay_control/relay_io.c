#include <stddef.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_log.h"
#include "relay_control.h"
#include "relay_io.h"

#define GPIO_OUTPUT_PIN_SEL ((1ULL << RELAY_OUTPUT_IO_1) | (1ULL << RELAY_OUTPUT_IO_2) | (1ULL << RELAY_OUTPUT_IO_3))

static const char *TAG = "RLY_IO";
static const gpio_num_t s_relay_pins[] = {
    RELAY_OUTPUT_IO_1,
    RELAY_OUTPUT_IO_2,
    RELAY_OUTPUT_IO_3,
};

static uint32_t s_relay_states[RELAY_COUNT] = {
    RELAY_OFF_STATE,
    RELAY_OFF_STATE,
    RELAY_OFF_STATE,
};

esp_err_t Relay_IO_Init(void)
{
    gpio_config_t io_conf = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = GPIO_OUTPUT_PIN_SEL,
        .pull_down_en = 0,
        .pull_up_en = 0,
    };

    esp_err_t err = gpio_config(&io_conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "GPIO init failed: %s", esp_err_to_name(err));
        return err;
    }

    for (uint32_t i = 0; i < RELAY_COUNT; i++) {
        gpio_set_level(s_relay_pins[i], RELAY_OFF_STATE);
        s_relay_states[i] = RELAY_OFF_STATE;
    }

    return ESP_OK;
}

esp_err_t Relay_IO_Set(uint32_t relay_number, uint32_t relay_state)
{
    if (relay_number >= RELAY_COUNT) {
        ESP_LOGE(TAG, "Invalid relay number: %lu", relay_number);
        return ESP_ERR_INVALID_ARG;
    }

    const uint32_t normalized_state = relay_state ? RELAY_ON_STATE : RELAY_OFF_STATE;
    esp_err_t err = gpio_set_level(s_relay_pins[relay_number], normalized_state);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set relay %lu: %s", relay_number + 1, esp_err_to_name(err));
        return err;
    }

    s_relay_states[relay_number] = normalized_state;
    return ESP_OK;
}

esp_err_t Relay_IO_On(uint32_t relay_number)
{
    return Relay_IO_Set(relay_number, RELAY_ON_STATE);
}

esp_err_t Relay_IO_Off(uint32_t relay_number)
{
    return Relay_IO_Set(relay_number, RELAY_OFF_STATE);
}

uint32_t Relay_IO_GetState(uint32_t relay_number)
{
    if (relay_number >= RELAY_COUNT) {
        ESP_LOGE(TAG, "Invalid relay number: %lu", relay_number);
        return RELAY_OFF_STATE;
    }

    return s_relay_states[relay_number];
}
