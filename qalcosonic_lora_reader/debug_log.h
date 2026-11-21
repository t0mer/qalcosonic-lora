// debug_log.h - tiny serial logging helpers.
// Set VERBOSE_NFC_LOG to 1 in config.h to see every ISO 15693 frame.
#pragma once

#include <Arduino.h>
#include "config.h"

#ifndef VERBOSE_NFC_LOG
#define VERBOSE_NFC_LOG 0
#endif

#define LOGI(fmt, ...) Serial.printf("[I] " fmt "\n", ##__VA_ARGS__)
#define LOGE(fmt, ...) Serial.printf("[E] " fmt "\n", ##__VA_ARGS__)

#if VERBOSE_NFC_LOG
#define LOGV(fmt, ...) Serial.printf("[V] " fmt "\n", ##__VA_ARGS__)
#define LOGV_HEX(label, buf, len) logHex(label, buf, len)
#else
#define LOGV(fmt, ...) do {} while (0)
#define LOGV_HEX(label, buf, len) do {} while (0)
#endif

inline void logHex(const char *label, const uint8_t *buf, size_t len) {
  Serial.printf("[V] %s (%u):", label, (unsigned)len);
  for (size_t i = 0; i < len; i++) Serial.printf(" %02X", buf[i]);
  Serial.println();
}
