// config.h - publisher side (ESP32-WROOM + DX-LR30): LoRa in, MQTT out.
// LoRa radio pins and parameters are in lora_link.h (shared with the reader).
#pragma once

// ---- WiFi ----
#define WIFI_SSID      ""
#define WIFI_PASSWORD  ""
#define DEVICE_NAME    "qalcosonic-lora-gw"   // hostname + MQTT client id

// ---- MQTT ----
#define MQTT_HOST      ""
#define MQTT_PORT      1883
#define MQTT_USER      ""                     // leave empty for anonymous
#define MQTT_PASSWORD  ""
#define MQTT_BASE_TOPIC "watermeter/qalcosonic"
#define HA_DISCOVERY   1
#define HA_DISCOVERY_PREFIX "homeassistant"
#define HA_DEVICE_ID   "qalcosonic_w1"        // Home Assistant device / unique_id prefix

// ---- Reader link ----
#define READER_NODE_ID 1   // must match NODE_ID on the reader
// Mark the meter unavailable in HA if nothing arrives for this long.
// Keep it at about 3x the reader's POLL_INTERVAL_MS.
#define READER_TIMEOUT_MS 10800000UL   // 3 h

// ---- LoRa transmit power ----
// dBm, -9..22 on the LR30. 433 MHz SRD rules usually allow ~10 mW (10 dBm);
// check your local limit before raising it. Frequency, SF etc. are in lora_link.h.
#define LORA_TX_POWER_DBM 10
