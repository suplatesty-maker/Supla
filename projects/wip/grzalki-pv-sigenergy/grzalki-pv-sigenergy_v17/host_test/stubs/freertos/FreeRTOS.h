#pragma once
#include <cstdint>
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(x) ((void)(x))
#define portEXIT_CRITICAL(x) ((void)(x))
typedef int BaseType_t;
#define pdPASS 1
inline uint32_t pdMS_TO_TICKS(uint32_t x) { return x; }
