#line 1 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/Timer_micros.h"
#ifndef __TIMERS_MICROS_H__
#define __TIMERS_MICROS_H__

#include <stdint.h>

// Periods (µs) — compile-time immediates in microsTimer() / microsTimer2().
static constexpr uint32_t kTimer50us  = 50;
static constexpr uint32_t kTimer51us  = 51;
static constexpr uint32_t kTimer99us  = 99;
// static constexpr uint32_t kTimer100us = 100;
static constexpr uint32_t kTimer1ms   = 1001;
// static constexpr uint32_t kTimer223us  = 223;
// static constexpr uint32_t kTimer5ms    = 5000;
// static constexpr uint32_t kTimer11ms   = 11000;
// static constexpr uint32_t kTimer23ms   = 23000;
// static constexpr uint32_t kTimer31ms   = 31000;
// static constexpr uint32_t kTimer67ms   = 67000;
// static constexpr uint32_t kTimer200ms  = 200000;
// static constexpr uint32_t kTimer500ms  = 500000;
// static constexpr uint32_t kTimer1000ms = 1000000;

// --- Core 0 (loop / microsTimer) ---
unsigned long timer50micros = 0;
unsigned long timer51micros = 0;
unsigned long timer99micros = 0;
// unsigned long timer100micros = 0;
unsigned long timer1ms = 0;
// unsigned long timer223micros = 0;
// unsigned long timer5ms = 0;
// unsigned long timer11ms = 0;
// unsigned long timer23ms = 0;
// unsigned long timer31ms = 0;
// unsigned long timer67ms = 0;
// unsigned long timer200ms = 0;
// unsigned long timer500ms = 0;
// unsigned long timer1000ms = 0;

bool timer50microsFlag = 0;
bool timer51microsFlag = 0;
bool timer99microsFlag = 0;
// bool timer100microsFlag = 0;
bool timer1msFlag = 0;
// bool timer223microsFlag = 0;
// bool timer5msFlag = 0;
// bool timer11msFlag = 0;
// bool timer23msFlag = 0;
// bool timer31msFlag = 0;
// bool timer67msFlag = 0;
// bool timer200msFlag = 0;
// bool timer500msFlag = 0;
// bool timer1000msFlag = 0;

// --- Core 1 (loop1 / microsTimer2) ---
unsigned long timer50micros2 = 0;
unsigned long timer51micros2 = 0;
unsigned long timer99micros2 = 0;
// unsigned long timer100micros2 = 0;
unsigned long timer1ms2 = 0;
// unsigned long timer223micros2 = 0;
// unsigned long timer5ms2 = 0;
// unsigned long timer11ms2 = 0;
// unsigned long timer23ms2 = 0;
// unsigned long timer31ms2 = 0;
// unsigned long timer67ms2 = 0;
// unsigned long timer200ms2 = 0;
// unsigned long timer500ms2 = 0;
// unsigned long timer1000ms2 = 0;

bool timer50microsFlag2 = 0;
bool timer51microsFlag2 = 0;
bool timer99microsFlag2 = 0;
// bool timer100microsFlag2 = 0;
bool timer1msFlag2 = 0;
// bool timer223microsFlag2 = 0;
// bool timer5msFlag2 = 0;
// bool timer11msFlag2 = 0;
// bool timer23msFlag2 = 0;
// bool timer31msFlag2 = 0;
// bool timer67msFlag2 = 0;
// bool timer200msFlag2 = 0;
// bool timer500msFlag2 = 0;
// bool timer1000msFlag2 = 0;

void init_micros_timers();
void microsTimer();
void microsTimer2();

#endif
