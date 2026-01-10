#include <stdio.h>
#include <string.h>
#include "esp_app_desc.h"
#include "version.h"

// Текущая версия прошивки
const char* FIRMWARE_VERSION = NULL;

// Сравнение версий (возвращает: -1 если v1 < v2, 0 если равны, 1 если v1 > v2)
int version_compare(const char* v1, const char* v2) {
    int major1, minor1, patch1;
    int major2, minor2, patch2;
    
    sscanf(v1, "%d.%d.%d", &major1, &minor1, &patch1);
    sscanf(v2, "%d.%d.%d", &major2, &minor2, &patch2);
    
    if (major1 != major2) return (major1 > major2) ? 1 : -1;
    if (minor1 != minor2) return (minor1 > minor2) ? 1 : -1;
    if (patch1 != patch2) return (patch1 > patch2) ? 1 : -1;
    
    return 0;
}

// Получение текущей версии
const char* version_get(void) {
    if (FIRMWARE_VERSION == NULL) {
        const esp_app_desc_t *app_desc = esp_app_get_description();
        if (app_desc) {
            // Копируем строку в статический буфер
            static char version_buffer[32];
            strlcpy(version_buffer, app_desc->version, sizeof(version_buffer));
            FIRMWARE_VERSION = version_buffer;
        } else {
            FIRMWARE_VERSION = "unknown";
        }
    }
    return FIRMWARE_VERSION;
}

// Проверка, является ли версия v2 новее v1
bool version_is_newer(const char* v1, const char* v2) {
    return version_compare(v2, v1) > 0;
}