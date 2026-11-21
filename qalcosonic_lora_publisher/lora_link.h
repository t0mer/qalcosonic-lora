// lora_link.h - DX-LR30-433M22S (SX1262) setup, shared by reader and publisher.
//
// THIS FILE MUST BE IDENTICAL IN BOTH SKETCH FOLDERS (frequency, SF, BW, CR and
// sync word have to match on both sides).
//
// Radio setup follows svcisr/lora-door-sensor, measured on real LR30 boards:
// crystal (no TCXO) with trim 0x04/0x2F, DC-DC regulator, antenna switch on
// TXEN/RXEN (not DIO2), and RXEN held HIGH except while transmitting.
#pragma once

#include <RadioLib.h>
#include "config.h"   // per-side settings, e.g. LORA_TX_POWER_DBM

// ---- LR30 wiring (same on both ESP32s; matches the range-test wiring) ----
// LR30 VCC -> 3V3 only. The LR30 shares the VSPI bus (18/19/23).
#define PIN_LORA_SCK    18
#define PIN_LORA_MISO   19
#define PIN_LORA_MOSI   23
#define PIN_LORA_NSS    5
#define PIN_LORA_RESET  14
#define PIN_LORA_BUSY   26
#define PIN_LORA_DIO1   27
#define PIN_LORA_TXEN   32
#define PIN_LORA_RXEN   33

// ---- LoRa parameters ----
#define LORA_FREQ_MHZ      433.5f   // 433.05-434.79 MHz SRD band
#define LORA_BW_KHZ        125
#define LORA_SF            9        // a ~45-byte packet is ~250 ms on air; use 11-12 for more range
#define LORA_CR            5        // 4/5
#define LORA_SYNC_WORD     0x34     // differs from the range test (0x12) so the two never mix
#ifndef LORA_TX_POWER_DBM         // set per side in config.h; may differ between sides
#define LORA_TX_POWER_DBM  10
#endif
#define LORA_PREAMBLE      8
#define LORA_XTA_TRIM      0x04
#define LORA_XTB_TRIM      0x2F

extern SX1262 radio;

// Resets and configures the radio. Returns a RadioLib status code.
int16_t loraBegin();
// Re-applies the crystal trim (needed after the radio has slept).
int16_t loraApplyTrim();
void loraReport(Print &log, int16_t state);
