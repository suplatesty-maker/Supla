#pragma once
#include <cstdint>
#include <cstdio>
#include <cmath>
#include <cstring>
#define HIGH 1
#define LOW 0
#define INPUT 0
#define OUTPUT 1
using std::isfinite;
extern uint32_t g_fakeMillis;
inline uint32_t millis() { return g_fakeMillis; }
inline void delay(uint32_t ms) { g_fakeMillis += ms; }
inline void pinMode(int, int) {}
inline void digitalWrite(int, int) {}
struct SerialStub {
  template <class... A> void printf(const char*, A...) {}
  void println(const char*) {}
};
extern SerialStub Serial;
