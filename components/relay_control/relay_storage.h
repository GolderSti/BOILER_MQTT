#pragma once

#include <stdint.h>
#include "esp_err.h"

esp_err_t relay_storage_init(const char *partition_name);
esp_err_t relay_storage_set_u32(const char *key, uint32_t value);
esp_err_t relay_storage_get_u32(const char *key, uint32_t *out_value);
esp_err_t relay_storage_deinit(const char *partition_name);

