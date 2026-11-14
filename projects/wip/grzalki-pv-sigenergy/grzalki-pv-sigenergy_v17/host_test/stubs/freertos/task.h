#pragma once
#include "FreeRTOS.h"
inline void vTaskDelay(uint32_t) {}
inline BaseType_t xTaskCreatePinnedToCore(void (*)(void*), const char*, uint32_t,
                                          void*, int, void*, int) { return pdPASS; }
