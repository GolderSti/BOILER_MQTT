#ifndef VERSION_H
#define VERSION_H

#include <stdbool.h>

// // Версия определяется через CMake
// #ifndef FIRMWARE_VERSION_STR
// #define FIRMWARE_VERSION_STR "1.0.0"
// #endif

// extern const char* FIRMWARE_VERSION;

int version_compare(const char* v1, const char* v2);
const char* version_get(void);
bool version_is_newer(const char* v1, const char* v2);

#endif