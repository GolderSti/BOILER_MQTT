#include <stdio.h>
#include <string.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "nvs_flash.h"
#include "mbedtls/sha256.h"
#include "version.h"
#include "ota.h"

#define TAG "OTA"

// Константы
#define OTA_CHECK_BIT BIT0
#define OTA_UPDATE_BIT BIT1
#define OTA_MAX_RETRY 3
#define OTA_BUFFER_SIZE 4096
#define VERSION_BUFFER_SIZE 32

// Структура для хранения состояния OTA
static struct {
    ota_config_t config;
    ota_status_t status;
    uint8_t progress;
    char last_error[128];
    char server_version[VERSION_BUFFER_SIZE];
    TaskHandle_t background_task;
    EventGroupHandle_t ota_event_group;
    bool initialized;
    uint8_t retry_count;
    uint32_t last_check_time;
} ota_state = {0};

typedef struct {
    int total_len;
    int current_len;
    mbedtls_sha256_context sha256;
} ota_http_ctx_t;

// Получение версии с сервера
static esp_err_t fetch_server_version(char* version, size_t len) {
    char url[510];
    snprintf(url, sizeof(url), "%s%s%s", ota_state.config.server_url, ota_state.config.version_path, ota_state.config.app_name);
    
    ESP_LOGI(TAG, "Проверяю версию на сервере: %s", url);
    
    esp_http_client_config_t http_config = {
        .url = url,
        .timeout_ms = 5000,
        .buffer_size = 512,
    };
    
    esp_http_client_handle_t client = esp_http_client_init(&http_config);
    if (!client) {
        snprintf(ota_state.last_error, sizeof(ota_state.last_error), 
                 "Ошибка инициализации HTTP клиента");
        return ESP_FAIL;
    }
    
    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        esp_http_client_cleanup(client);
        snprintf(ota_state.last_error, sizeof(ota_state.last_error), 
                 "Ошибка подключения к серверу: %s", esp_err_to_name(err));
        return err;
    }
    
    int content_length = esp_http_client_fetch_headers(client);
    if (content_length < 0) {
        esp_http_client_cleanup(client);
        snprintf(ota_state.last_error, sizeof(ota_state.last_error), 
                "Ошибка получения заголовков");
        return ESP_FAIL;
    }
    
    // СНАЧАЛА проверяем статус код!
    int status_code = esp_http_client_get_status_code(client);
    
    if (status_code != 200) {
        esp_http_client_cleanup(client);
        
        switch(status_code) {
            case 404:
                snprintf(ota_state.last_error, sizeof(ota_state.last_error), 
                         "Прошивка не найдена на сервере");
                break;
            case 400:
                snprintf(ota_state.last_error, sizeof(ota_state.last_error), 
                         "Не указано имя приложения");
                break;
            case 500:
                snprintf(ota_state.last_error, sizeof(ota_state.last_error), 
                         "Ошибка сервера");
                break;
            default:
                snprintf(ota_state.last_error, sizeof(ota_state.last_error), 
                         "HTTP ошибка: %d", status_code);
        }
        return ESP_FAIL;
    }
    
    // Читаем версию
    memset(version, 0, len);
    int read_len = esp_http_client_read(client, version, len - 1);
    esp_http_client_cleanup(client);
    
    if (read_len <= 0) {
        snprintf(ota_state.last_error, sizeof(ota_state.last_error), 
                 "Ошибка чтения версии");
        return ESP_FAIL;
    }
    
    // Убираем лишние символы (переносы строк, пробелы)
    version[strcspn(version, "\r\n")] = 0;
    ESP_LOGI(TAG, "Версия на сервере: %s", version);
    
    return ESP_OK;
}

// Проверка наличия обновления
static esp_err_t check_for_update_internal(void) {
    ota_state.status = OTA_STATUS_CHECKING;
    memset(ota_state.last_error, 0, sizeof(ota_state.last_error));
    
    // Получаем версию с сервера
    char server_version[VERSION_BUFFER_SIZE];
    esp_err_t err = fetch_server_version(server_version, sizeof(server_version));
    if (err != ESP_OK) {
        ota_state.status = OTA_STATUS_VERSION_ERROR;
        return err;
    }
    
    // Копируем для доступа извне
    strlcpy(ota_state.server_version, server_version, sizeof(ota_state.server_version));
    
    // Сравниваем версии
    const char* current_version = version_get();
    ESP_LOGI(TAG, "Текущая версия: %s, версия на сервере: %s", 
             current_version, server_version);
    
    if (version_is_newer(current_version, server_version)) {
        ESP_LOGI(TAG, "Доступно обновление: %s -> %s", current_version, server_version);
        ota_state.status = OTA_STATUS_UPDATE_AVAILABLE;
        return ESP_OK;
    } else {
        ESP_LOGI(TAG, "Обновление не требуется");
        ota_state.status = OTA_STATUS_NO_UPDATE;
        return ESP_OK;
    }
}

// Callback для отслеживания прогресса OTA
static esp_err_t http_event_handler(esp_http_client_event_t *e) {  

    ota_http_ctx_t *ctx = (ota_http_ctx_t *)e->user_data;
    if (!ctx) return ESP_OK;
    switch(e->event_id) {
        case HTTP_EVENT_ON_HEADER:
            // Получаем общий размер файла
            if (strcasecmp(e->header_key, "Content-Length") == 0) {
                ctx->total_len = atoi(e->header_value);

                const esp_partition_t *update_partition =
                    esp_ota_get_next_update_partition(NULL);

                if (!update_partition) {
                    ESP_LOGE(TAG, "No OTA partition");
                    return ESP_FAIL;
                }

                if (ctx->total_len > update_partition->size) {
                    ESP_LOGE(TAG,
                        "Firmware too large: %d > %lu",
                        ctx->total_len,
                        update_partition->size
                    );
                    ota_state.status = OTA_STATUS_FAILED;
                    strcpy(ota_state.last_error, "Firmware too large");
                    return ESP_FAIL; // ← OTA будет прерван
                }

                ESP_LOGI(TAG,
                    "Firmware size OK: %d / %lu",
                    ctx->total_len,
                    update_partition->size
                );
            }            
            break;
            
        case HTTP_EVENT_ON_DATA:
            // Обновляем прогресс
            if (ctx->total_len > 0) {
                mbedtls_sha256_update(&ctx->sha256, e->data, e->data_len);
                ctx->current_len += e->data_len;
            }
            break;
            
        case HTTP_EVENT_ON_FINISH:
            ESP_LOGI(TAG, "Загрузка завершена");
            // ctx->total_len = 0;
            // ctx->current_len = 0;
            break;
            
        default:
            break;
    }
    return ESP_OK;
}

static void debug_current_ota_state(const char* stage) {
    ESP_LOGI(TAG, "=== OTA Debug: %s ===", stage);
    
    // 1. Текущий работающий раздел
    const esp_partition_t* running = esp_ota_get_running_partition();
    ESP_LOGI(TAG, "Running partition: %s (0x%x)", 
             running ? running->label : "NULL", 
             running ? running->address : 0);
    
    // 2. Раздел, с которого будет загрузка после перезагрузки
    const esp_partition_t* boot = esp_ota_get_boot_partition();
    ESP_LOGI(TAG, "Boot partition: %s (0x%x)", 
             boot ? boot->label : "NULL", 
             boot ? boot->address : 0);
    
    // 3. Проверяем otadata
    // esp_ota_select_entry_t otadata[2];
    // if (esp_ota_get_boot_selection(&otadata[0], &otadata[1]) == ESP_OK) {
    //     ESP_LOGI(TAG, "otadata[0]: ota_seq=%d, crc=0x%04x, valid=%d", 
    //              otadata[0].ota_seq, otadata[0].crc, otadata[0].ota_state);
    //     ESP_LOGI(TAG, "otadata[1]: ota_seq=%d, crc=0x%04x, valid=%d", 
    //              otadata[1].ota_seq, otadata[1].crc, otadata[1].ota_state);
    // }
    
    // 4. Все доступные OTA разделы
    ESP_LOGI(TAG, "Available OTA partitions:");
    esp_partition_iterator_t it = esp_partition_find(ESP_PARTITION_TYPE_APP, 
                                                    ESP_PARTITION_SUBTYPE_APP_OTA_MIN, NULL);
    while (it) {
        const esp_partition_t* p = esp_partition_get(it);
        ESP_LOGI(TAG, "  - %s: 0x%x (size: 0x%x)", 
                p->label, p->address, p->size);
        it = esp_partition_next(it);
    }
    esp_partition_iterator_release(it);
}

//convert hex sha to bytes
static esp_err_t hex_to_bytes(const char *hex, uint8_t *out, size_t out_len)
{
    size_t len = strlen(hex);

    if (len < out_len * 2) {
        return ESP_ERR_INVALID_SIZE;
    }

    for (size_t i = 0; i < out_len; i++) {
        char tmp[3] = {
            hex[i * 2],
            hex[i * 2 + 1],
            0
        };
        char *end;
        long val = strtol(tmp, &end, 16);
        if (*end != '\0' || val < 0 || val > 255) {
            return ESP_ERR_INVALID_ARG;
        }
        out[i] = (uint8_t)val;
    }
    return ESP_OK;
}

esp_err_t get_firmware_sha_from_server(uint8_t expected_sha[32])
{
    char url[510];
    snprintf(url, sizeof(url), "%s%s%s", 
            ota_state.config.server_url, 
            ota_state.config.sha256_path, 
            ota_state.config.app_name);

    ESP_LOGI(TAG, "Fetching firmware SHA256: %s", url);

    esp_http_client_config_t http_config = {
        .url = url,
        .timeout_ms = 5000,
        .buffer_size = 512,
    };
    
    esp_http_client_handle_t client = esp_http_client_init(&http_config);
    if (!client) {
        snprintf(ota_state.last_error, sizeof(ota_state.last_error), 
                 "Ошибка инициализации HTTP клиента");
        return ESP_FAIL;
    }
    
    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        esp_http_client_cleanup(client);
        snprintf(ota_state.last_error, sizeof(ota_state.last_error), 
                 "Ошибка подключения к серверу: %s", esp_err_to_name(err));
        return err;
    }
    
    int content_length = esp_http_client_fetch_headers(client);
    if (content_length < 0) {
        esp_http_client_cleanup(client);
        snprintf(ota_state.last_error, sizeof(ota_state.last_error), 
                "Ошибка получения заголовков");
        return ESP_FAIL;
    }
    
    // СНАЧАЛА проверяем статус код!
    int status_code = esp_http_client_get_status_code(client);
    
    if (status_code != 200) {
        esp_http_client_cleanup(client);
        
        switch(status_code) {
            case 404:
                snprintf(ota_state.last_error, sizeof(ota_state.last_error), 
                         "Прошивка не найдена на сервере");
                break;
            case 400:
                snprintf(ota_state.last_error, sizeof(ota_state.last_error), 
                         "Не указано имя приложения");
                break;
            case 500:
                snprintf(ota_state.last_error, sizeof(ota_state.last_error), 
                         "Ошибка сервера");
                break;
            default:
                snprintf(ota_state.last_error, sizeof(ota_state.last_error), 
                         "HTTP ошибка: %d", status_code);
        }
        return ESP_FAIL;
    }

    char buf[128] = {0};
    int read_len = esp_http_client_read(client, buf, sizeof(buf) - 1);
    esp_http_client_cleanup(client);

    if (read_len <= 0) {
        strcpy(ota_state.last_error, "SHA: read failed");
        return ESP_FAIL;
    }

    // Убираем всё после первого не-hex символа
    for (int i = 0; i < read_len; i++) {
        if (!isxdigit((unsigned char)buf[i])) {
            buf[i] = '\0';
            break;
        }
    }

    if (strlen(buf) < 64) {
        strcpy(ota_state.last_error, "SHA: too short");
        return ESP_ERR_INVALID_SIZE;
    }

    err = hex_to_bytes(buf, expected_sha, 32);
    if (err != ESP_OK) {
        strcpy(ota_state.last_error, "SHA: invalid hex");
        return err;
    }

    ESP_LOGI(TAG, "Firmware SHA256 received successfully");
    return ESP_OK;
}

// Выполнение OTA обновления
static esp_err_t perform_ota_update(void) {
    esp_err_t err;
    esp_https_ota_handle_t handle = NULL;
    ota_http_ctx_t http_ctx = {0};
    mbedtls_sha256_init(&http_ctx.sha256);
    mbedtls_sha256_starts(&http_ctx.sha256, 0); // 0 = SHA256

    ota_state.status = OTA_STATUS_UPDATING;
    ota_state.progress = 0;
    memset(ota_state.last_error, 0, sizeof(ota_state.last_error));

    ESP_LOGD(TAG,"Get OTA Partition size");
    const esp_partition_t *update_partition =
        esp_ota_get_next_update_partition(NULL);

    if (!update_partition) {
        ESP_LOGE(TAG, "No OTA partition found");
        strcpy(ota_state.last_error, "No OTA partition");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "OTA partition: %s, size: %lu",
            update_partition->label,
            update_partition->size); 

    char url[510];
    snprintf(url, sizeof(url), "%s%s%s", 
            ota_state.config.server_url, 
            ota_state.config.firmware_path, 
            ota_state.config.app_name);
    ESP_LOGI(TAG, "Start OTA from: %s", url);

    esp_http_client_config_t http_cfg = {
        .url = url,
        .timeout_ms = 10000,
        .event_handler = http_event_handler,
        .user_data = &http_ctx,
        .buffer_size = OTA_BUFFER_SIZE,
        .keep_alive_enable = true,
    };

    esp_https_ota_config_t ota_cfg = {
        .http_config = &http_cfg,
    };
    ESP_LOGD(TAG,"Call OTA BEGIN");
    err = esp_https_ota_begin(&ota_cfg, &handle);
    if (err != ESP_OK) {
        snprintf(ota_state.last_error, sizeof(ota_state.last_error),
                 "ota_begin failed: %s", esp_err_to_name(err));
        goto fail;
    }
    
    ESP_LOGD(TAG,"OTA BEGIN: OK. Call OTA PERFORM ");
    while (1) {
        err = esp_https_ota_perform(handle);
        if (err != ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
        int progress = (http_ctx.current_len * 100) / http_ctx.total_len;
        if (progress != ota_state.progress) {
            ota_state.progress = progress;
            ESP_LOGI(TAG, "Прогресс: %d%%", progress);
        }
    }

    if (err != ESP_OK) {
        snprintf(ota_state.last_error, sizeof(ota_state.last_error),
                 "ota_perform failed: %s", esp_err_to_name(err));
        goto fail;
    }
    ESP_LOGD(TAG,"OTA PERFORM: OK. Check OTA IS COMPLETE");
    if (!esp_https_ota_is_complete_data_received(handle)) {
        strcpy(ota_state.last_error, "Incomplete firmware");
        err = ESP_FAIL;
        goto fail;
    }
    ESP_LOGD(TAG,"OTA IS COMPLETE: OK. Compare SHA");
    uint8_t calculated_sha[32];
    mbedtls_sha256_finish(&http_ctx.sha256, calculated_sha);
    ESP_LOG_BUFFER_HEX("SHA256 firmware Hash", calculated_sha, sizeof(calculated_sha)); 

    uint8_t expected_sha[32];
    err = get_firmware_sha_from_server(expected_sha);
    if (err != ESP_OK) {
        goto fail;
    }
    ESP_LOG_BUFFER_HEX("SHA256 Server Hash", expected_sha, sizeof(expected_sha)); 

    if (memcmp(expected_sha, calculated_sha, 32) != 0) {
        ESP_LOGE(TAG, "SHA256 mismatch!");
        strcpy(ota_state.last_error, "SHA256 mismatch");
        err = ESP_ERR_INVALID_CRC;
        goto fail;
    }

    ESP_LOGD(TAG,"SHA: OK: Call OTA FINISH");
    err = esp_https_ota_finish(handle);
    if (err != ESP_OK) {
        snprintf(ota_state.last_error, sizeof(ota_state.last_error),
                 "ota_finish failed: %s", esp_err_to_name(err));
        goto fail;
    }
    debug_current_ota_state("После esp_https_ota_finish");
    ota_state.progress = 100;
    ESP_LOGI(TAG, "OTA successful, rebooting...");
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();

fail:
    mbedtls_sha256_free(&http_ctx.sha256);    
    ota_state.status = OTA_STATUS_FAILED;
    if (handle) {
        ESP_LOGD(TAG,"OTA: FAIL. Call OTA ABORT");
        esp_https_ota_abort(handle);
    }
    return err;
}


// Фоновая задача для проверки обновлений
static void ota_background_task(void *arg) {
    ESP_LOGI(TAG, "Запущена фоновая задача проверки OTA");
    
    while (1) {
        // Ждем сигнал проверки или таймаут
        EventBits_t bits = xEventGroupWaitBits(
            ota_state.ota_event_group,
            OTA_CHECK_BIT,
            pdTRUE,
            pdFALSE,
            pdMS_TO_TICKS(ota_state.config.check_interval_ms)
        );
        
        uint32_t current_time = xTaskGetTickCount() * portTICK_PERIOD_MS;
        
        // Проверяем обновление по таймеру или по сигналу
        if ((bits & OTA_CHECK_BIT) || 
            (current_time - ota_state.last_check_time >= ota_state.config.check_interval_ms)) {
            
            ESP_LOGI(TAG, "Проверяю обновление...");
            ota_state.last_check_time = current_time;
            
            esp_err_t err = check_for_update_internal();
            if (err == ESP_OK) {
                // Если есть обновление, запускаем его
                if (version_is_newer(version_get(), ota_state.server_version)) {
                    ESP_LOGI(TAG, "Начинаю автоматическое обновление");
                    xEventGroupSetBits(ota_state.ota_event_group, OTA_UPDATE_BIT);
                }
            } else {
                ESP_LOGW(TAG, "Ошибка проверки обновления: %s", 
                        ota_state.last_error);
                ota_state.retry_count++;
                
                if (ota_state.retry_count >= ota_state.config.max_retries) {
                    ESP_LOGW(TAG, "Превышено число попыток, следующая проверка через 1 час");
                    vTaskDelay(pdMS_TO_TICKS(3600000)); // 1 час
                    ota_state.retry_count = 0;
                }
            }
        }
        
        // Проверяем сигнал на обновление
        bits = xEventGroupGetBits(ota_state.ota_event_group);
        if (bits & OTA_UPDATE_BIT) {
            xEventGroupClearBits(ota_state.ota_event_group, OTA_UPDATE_BIT);
            perform_ota_update();
        }
        
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

// Публичные функции

esp_err_t ota_init(const ota_config_t* config) {
    if (ota_state.initialized) {
        ESP_LOGW(TAG, "OTA уже инициализирован");
        return ESP_OK;
    }
    
    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }
    
    // Копируем конфигурацию
    memcpy(&ota_state.config, config, sizeof(ota_config_t));
    ota_state.status = OTA_STATUS_IDLE;
    ota_state.progress = 0;
    ota_state.retry_count = 0;
    ota_state.last_check_time = 0;
    memset(ota_state.last_error, 0, sizeof(ota_state.last_error));
    memset(ota_state.server_version, 0, sizeof(ota_state.server_version));
    
    // Создаем event group
    ota_state.ota_event_group = xEventGroupCreate();
    if (!ota_state.ota_event_group) {
        return ESP_ERR_NO_MEM;
    }
    
    ota_state.initialized = true;
    ESP_LOGI(TAG, "OTA инициализирован");
    ESP_LOGI(TAG, "Сервер: %s", config->server_url);
    ESP_LOGI(TAG, "Интервал проверки: %lu мс", config->check_interval_ms);
    
    return ESP_OK;
}

esp_err_t ota_check_for_update(void) {
    if (!ota_state.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    
    xEventGroupSetBits(ota_state.ota_event_group, OTA_CHECK_BIT);
    return ESP_OK;
}

esp_err_t ota_update_to_version(const char* version) {
    if (!ota_state.initialized || !version) {
        return ESP_ERR_INVALID_ARG;
    }
    
    // Пока просто запускаем обновление на текущую версию сервера
    // В будущем можно реализовать выбор конкретной версии
    xEventGroupSetBits(ota_state.ota_event_group, OTA_UPDATE_BIT);
    return ESP_OK;
}

ota_status_t ota_get_status(void) {
    return ota_state.status;
}

uint8_t ota_get_progress(void) {
    return ota_state.progress;
}

const char* ota_get_last_error(void) {
    return ota_state.last_error;
}

const char* ota_get_server_version(void) {
    return ota_state.server_version;
}

void ota_start_background_check(void) {
    if (!ota_state.initialized || ota_state.background_task) {
        return;
    }
    
    xTaskCreate(
        ota_background_task,
        "ota_bg_task",
        8192,
        NULL,
        5,
        &ota_state.background_task
    );
    
    // Первая проверка при старте
    if (ota_state.config.check_on_boot) {
        // vTaskDelay(pdMS_TO_TICKS(5000)); // Даем время для подключения к Wi-Fi
        ota_check_for_update();
    }
}

void ota_stop_background_check(void) {
    if (ota_state.background_task) {
        vTaskDelete(ota_state.background_task);
        ota_state.background_task = NULL;
    }
}