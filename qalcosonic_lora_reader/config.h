// config.h - reader side (ESP32-WROOM + PN5180 + DX-LR30), at the water meter.
// LoRa radio pins and parameters are in lora_link.h (shared with the publisher).
#pragma once

#define NODE_ID 1   // must match READER_NODE_ID on the publisher

// ---- Polling ----
// Default: one read per hour. Every NFC read wakes the meter and costs its
// sealed battery a little: upstream measured ~1% per 75 days at 60 s, so 1 h
// is roughly 1% every ~12 years. Short intervals (e.g. 10000 = 10 s, ~1% every
// ~12 days) are for bench testing only; keep it at 300000 (5 min) or more in use.
#define POLL_INTERVAL_MS 3600000UL   // 1 h

// ---- Deep sleep ----
// The reader deep-sleeps between polls. If the LR30 fails to start it sleeps
// only this long before trying again. MIN_SLEEP_MS is the shortest sleep ever used.
#define RADIO_RETRY_MS 30000UL
#define MIN_SLEEP_MS   1000UL

// ---- Power-bank keep-alive ----
// Power banks switch their output off when the load stays below ~50-100 mA for
// ~30-60 s, so a deep-sleeping reader would never wake again. With keep-alive on,
// the long sleep is cut into chunks of KEEPALIVE_INTERVAL_MS; each short wake only
// draws a current burst for KEEPALIVE_PULSE_MS (the meter is not touched) and
// sleeps again. Tune it to your bank: shorter interval / longer pulse if it still
// switches off. Costs roughly 3-6 mA on average. Set the interval to 0 when
// running from a bare battery.
#define KEEPALIVE_INTERVAL_MS 5000UL
#define KEEPALIVE_PULSE_MS    1000UL
#define KEEPALIVE_WIFI        1   // 1 = WiFi radio on during the pulse (~100+ mA); 0 = CPU only (~40-60 mA)

// Send each packet this many times (publisher drops the duplicates).
// 2 is useful at long poll intervals, where one lost packet means a long gap.
#define TX_REPEATS 2
#define TX_REPEAT_GAP_MS 400

// ---- LoRa transmit power ----
// dBm, -9..22 on the LR30. 433 MHz SRD rules usually allow ~10 mW (10 dBm);
// check your local limit before raising it. Frequency, SF etc. are in lora_link.h.
#define LORA_TX_POWER_DBM 10

// ---- PN5180 wiring ----
// The PN5180 and the LR30 share the VSPI bus (SCK 18, MISO 19, MOSI 23).
// PN5180 NSS is GPIO4 here (not 14 as upstream) because GPIO14 is the LR30 reset.
// The PN5180 board needs BOTH 5V (RF stage) and 3.3V (logic).
#define PIN_PN5180_SCK   18
#define PIN_PN5180_MISO  19
#define PIN_PN5180_MOSI  23
#define PIN_PN5180_NSS   4
#define PIN_PN5180_BUSY  16
#define PIN_PN5180_RST   17

// ---- Debug ----
#define VERBOSE_NFC_LOG 0 // 1 = print every ISO 15693 frame
