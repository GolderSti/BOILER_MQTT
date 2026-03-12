#include "relay_storage.h"

#include "esp_log.h"
#include "esp_partition.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "RLYSTRG";
static const char *STORAGE_PARTITION = "storage";
static const char *STORAGE_NAMESPACE = "config";

esp_err_t relay_storage_init(const char *partition_name)
{
    ESP_LOGI(TAG, "Initializing custom storage partition...");

    const esp_partition_t *partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA,
        ESP_PARTITION_SUBTYPE_ANY,
        STORAGE_PARTITION);

    if (partition == NULL) {
        ESP_LOGE(TAG, "Storage partition not found!");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Found storage partition: size=%dKB", partition->size / 1024);

    esp_err_t ret = nvs_flash_init_partition(STORAGE_PARTITION);
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "Erasing %s partition...", partition_name);
        ESP_ERROR_CHECK(nvs_flash_erase_partition(STORAGE_PARTITION));
        ret = nvs_flash_init_partition(STORAGE_PARTITION);
    }

    ESP_ERROR_CHECK(ret);
    ESP_LOGI(TAG, "%s partition initialized", partition_name);
    return ESP_OK;
}

esp_err_t relay_storage_set_u32(const char *key, uint32_t value)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open_from_partition(STORAGE_PARTITION, STORAGE_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open namespace '%s' in partition '%s': %s",
                 STORAGE_NAMESPACE, STORAGE_PARTITION, esp_err_to_name(err));
        return err;
    }

    err = nvs_set_u32(handle, key, value);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save key '%s': %s", key, esp_err_to_name(err));
    } else {
        ESP_LOGD(TAG, "Saved u32 key '%s' = %u", key, value);
    }

    nvs_close(handle);
    return err;
}

esp_err_t relay_storage_get_u32(const char *key, uint32_t *out_value)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open_from_partition(STORAGE_PARTITION, STORAGE_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open namespace '%s' in partition '%s': %s",
                 STORAGE_NAMESPACE, STORAGE_PARTITION, esp_err_to_name(err));
        return err;
    }

    err = nvs_get_u32(handle, key, out_value);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "Key '%s' not found", key);
    } else if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get u32 key '%s': %s", key, esp_err_to_name(err));
    } else {
        ESP_LOGD(TAG, "Read u32 key '%s' = %u", key, *out_value);
    }

    nvs_close(handle);
    return err;
}

esp_err_t relay_storage_deinit(const char *partition_name)
{
    esp_err_t err = nvs_flash_deinit_partition(partition_name);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to deinit NVS partition '%s': %s", partition_name, esp_err_to_name(err));
    }
    return err;
}
