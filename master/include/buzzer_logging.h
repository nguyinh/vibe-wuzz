#pragma once

#include <Arduino.h>

#define VIBEWUZZ_LOG_LEVEL_QUIET 0
#define VIBEWUZZ_LOG_LEVEL_ERROR 1
#define VIBEWUZZ_LOG_LEVEL_INFO 2
#define VIBEWUZZ_LOG_LEVEL_DEBUG 3

#ifndef VIBEWUZZ_LOG_LEVEL
#define VIBEWUZZ_LOG_LEVEL VIBEWUZZ_LOG_LEVEL_INFO
#endif

#if VIBEWUZZ_LOG_LEVEL >= VIBEWUZZ_LOG_LEVEL_ERROR
#define LOG_ERRORF(...) Serial.printf(__VA_ARGS__)
#define LOG_ERRORLN(message) Serial.println(message)
#else
#define LOG_ERRORF(...)
#define LOG_ERRORLN(message)
#endif

#if VIBEWUZZ_LOG_LEVEL >= VIBEWUZZ_LOG_LEVEL_INFO
#define LOG_INFOF(...) Serial.printf(__VA_ARGS__)
#define LOG_INFOLN(message) Serial.println(message)
#else
#define LOG_INFOF(...)
#define LOG_INFOLN(message)
#endif

#if VIBEWUZZ_LOG_LEVEL >= VIBEWUZZ_LOG_LEVEL_DEBUG
#define LOG_DEBUGF(...) Serial.printf(__VA_ARGS__)
#define LOG_DEBUGLN(message) Serial.println(message)
#else
#define LOG_DEBUGF(...)
#define LOG_DEBUGLN(message)
#endif
