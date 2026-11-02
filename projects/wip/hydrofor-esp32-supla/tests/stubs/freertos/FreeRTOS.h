#pragma once
// Host-only stand-in for the short critical sections used by mailbox.h.
#include <mutex>
#include <stdint.h>
using portMUX_TYPE = std::mutex;
#define portMUX_INITIALIZER_UNLOCKED {}
#define portENTER_CRITICAL(mux) (mux)->lock()
#define portEXIT_CRITICAL(mux) (mux)->unlock()
