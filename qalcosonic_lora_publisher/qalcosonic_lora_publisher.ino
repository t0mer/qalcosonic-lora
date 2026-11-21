// qalcosonic_lora_publisher.ino
//
// Indoor gateway: receives water-meter packets from the reader over LoRa
// (DX-LR30, 433 MHz) and publishes them to MQTT with Home Assistant discovery.
//
// Libraries: RadioLib (Jan Gromes) and PubSubClient (Nick O'Leary).

#include <WiFi.h>
#include <PubSubClient.h>
#include "config.h"
#include "lora_link.h"
#include "meter_packet.h"
#include "packet_json.h"

static WiFiClient wifiClient;
static PubSubClient mqtt(wifiClient);

static const String T_STATE  = String(MQTT_BASE_TOPIC) + "/state";   // retained meter JSON
static const String T_STATUS = String(MQTT_BASE_TOPIC) + "/status";  // gateway online/offline (LWT)
static const String T_READER = String(MQTT_BASE_TOPIC) + "/reader";  // reader online/offline
static const String T_ERROR  = String(MQTT_BASE_TOPIC) + "/error";   // reader readout failures
static const String T_LINK   = String(MQTT_BASE_TOPIC) + "/link";    // every packet's RSSI/SNR

static volatile bool rxFlag = false;
static bool radioOk = false;
static bool discoverySent = false;

static bool haveSeq = false;
static uint16_t lastSeq = 0;
static uint32_t lostPackets = 0;
static uint32_t lastPacketMs = 0;
static bool readerOnline = false;
static uint32_t lastWifiAttempt = 0, lastMqttAttempt = 0;

static char jsonBuf[1024];

#if defined(ESP32)
IRAM_ATTR
#endif
static void onDio1() { rxFlag = true; }

static void startRx() {
  int16_t st = radio.startReceive();
  if (st != RADIOLIB_ERR_NONE) Serial.printf("[radio] startReceive failed: %d\n", st);
}

static void setReaderOnline(bool online) {
  if (online == readerOnline) return;
  readerOnline = online;
  Serial.printf("[link] reader %s\n", online ? "online" : "OFFLINE (no packets)");
  if (mqtt.connected()) mqtt.publish(T_READER.c_str(), online ? "online" : "offline", true);
}

// ------------------------------------------------------------------ Home Assistant discovery

#if HA_DISCOVERY
static void disc(const char *component, const char *objectId, const char *name, const char *tpl,
                 const char *extra, bool needsReader = true) {
  String topic = String(HA_DISCOVERY_PREFIX) + "/" + component + "/" + HA_DEVICE_ID + "/" + objectId + "/config";
  String p = "{\"name\":\"" + String(name) + "\"";
  p += ",\"unique_id\":\"" + String(HA_DEVICE_ID) + "_" + objectId + "\"";
  p += ",\"state_topic\":\"" + T_STATE + "\"";
  p += ",\"value_template\":\"" + String(tpl) + "\"";
  // Available only while both the gateway and the reader are online.
  p += ",\"availability\":[{\"topic\":\"" + T_STATUS + "\"}";
  if (needsReader) p += ",{\"topic\":\"" + T_READER + "\"}";
  p += "],\"availability_mode\":\"all\"";
  if (extra && *extra) p += String(",") + extra;
  p += ",\"device\":{\"identifiers\":[\"" + String(HA_DEVICE_ID) + "\"],\"name\":\"Water meter\","
       "\"manufacturer\":\"Axioma Metering\",\"model\":\"Qalcosonic W1 (NFC + LoRa)\"}}";
  if (!mqtt.publish(topic.c_str(), p.c_str(), true)) Serial.printf("[mqtt] discovery failed: %s\n", objectId);
}

static void sendDiscovery() {
  disc("sensor", "volume", "Water usage", "{{ value_json.volume_m3 }}",
       "\"unit_of_measurement\":\"m³\",\"device_class\":\"water\",\"state_class\":\"total_increasing\",\"suggested_display_precision\":3");
  disc("sensor", "flow", "Water flow", "{{ value_json.flow_m3h }}",
       "\"unit_of_measurement\":\"m³/h\",\"device_class\":\"volume_flow_rate\",\"state_class\":\"measurement\"");
  disc("sensor", "water_temp", "Water temperature", "{{ value_json.water_temp_c }}",
       "\"unit_of_measurement\":\"°C\",\"device_class\":\"temperature\",\"state_class\":\"measurement\"");
  disc("sensor", "external_temp", "Meter ambient temperature", "{{ value_json.external_temp_c }}",
       "\"unit_of_measurement\":\"°C\",\"device_class\":\"temperature\",\"state_class\":\"measurement\"");
  disc("sensor", "battery", "Meter battery", "{{ value_json.battery_pct }}",
       "\"unit_of_measurement\":\"%\",\"device_class\":\"battery\",\"entity_category\":\"diagnostic\"");
  disc("sensor", "meter_id", "Meter ID", "{{ value_json.meter_id }}", "\"entity_category\":\"diagnostic\"");
  disc("sensor", "lora_rssi", "LoRa RSSI", "{{ value_json.lora.rssi_dbm }}",
       "\"unit_of_measurement\":\"dBm\",\"device_class\":\"signal_strength\",\"state_class\":\"measurement\",\"entity_category\":\"diagnostic\"");
  disc("sensor", "lora_snr", "LoRa SNR", "{{ value_json.lora.snr_db }}",
       "\"unit_of_measurement\":\"dB\",\"state_class\":\"measurement\",\"entity_category\":\"diagnostic\"");
  disc("binary_sensor", "leakage", "Leakage", "{{ 'ON' if value_json.alarms.leakage else 'OFF' }}",
       "\"device_class\":\"moisture\"");
  disc("binary_sensor", "burst", "Pipe burst", "{{ 'ON' if value_json.alarms.burst else 'OFF' }}",
       "\"device_class\":\"problem\"");
  disc("binary_sensor", "reverse_flow", "Reverse flow", "{{ 'ON' if value_json.alarms.reverse_flow else 'OFF' }}",
       "\"device_class\":\"problem\"");
  disc("binary_sensor", "low_battery", "Meter low battery", "{{ 'ON' if value_json.alarms.low_battery else 'OFF' }}",
       "\"device_class\":\"battery\",\"entity_category\":\"diagnostic\"");
  // Reader link: driven by <base>/reader, available whenever the gateway is.
  String t = String(HA_DISCOVERY_PREFIX) + "/binary_sensor/" + HA_DEVICE_ID + "/reader_link/config";
  String p = "{\"name\":\"Meter reader link\",\"unique_id\":\"" + String(HA_DEVICE_ID) + "_reader_link\"";
  p += ",\"state_topic\":\"" + T_READER + "\",\"payload_on\":\"online\",\"payload_off\":\"offline\"";
  p += ",\"availability_topic\":\"" + T_STATUS + "\",\"device_class\":\"connectivity\",\"entity_category\":\"diagnostic\"";
  p += ",\"device\":{\"identifiers\":[\"" + String(HA_DEVICE_ID) + "\"]}}";
  mqtt.publish(t.c_str(), p.c_str(), true);
  discoverySent = true;
}
#endif

// ------------------------------------------------------------------ connectivity

static void ensureWifi() {
  if (WiFi.status() == WL_CONNECTED) return;
  if (lastWifiAttempt && millis() - lastWifiAttempt < 10000) return;
  lastWifiAttempt = millis();
  Serial.printf("[wifi] connecting to %s\n", WIFI_SSID);
  WiFi.disconnect();
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
}

static void ensureMqtt() {
  if (WiFi.status() != WL_CONNECTED || mqtt.connected()) return;
  if (lastMqttAttempt && millis() - lastMqttAttempt < 5000) return;
  lastMqttAttempt = millis();
  bool ok = strlen(MQTT_USER)
                ? mqtt.connect(DEVICE_NAME, MQTT_USER, MQTT_PASSWORD, T_STATUS.c_str(), 1, true, "offline")
                : mqtt.connect(DEVICE_NAME, T_STATUS.c_str(), 1, true, "offline");
  if (!ok) { Serial.printf("[mqtt] connect failed, state=%d\n", mqtt.state()); return; }
  Serial.println("[mqtt] connected");
  mqtt.publish(T_STATUS.c_str(), radioOk ? "online" : "radio_error", true);
  mqtt.publish(T_READER.c_str(), readerOnline ? "online" : "offline", true);
#if HA_DISCOVERY
  if (!discoverySent) sendDiscovery();
#endif
}

// ------------------------------------------------------------------ packet handling

static void handlePacket() {
  MeterPacket m;
  size_t len = radio.getPacketLength();
  int16_t st = radio.readData((uint8_t *)&m, sizeof(m));
  LinkInfo link{radio.getRSSI(), radio.getSNR(), radio.getFrequencyError(), 0};
  startRx();  // re-arm right away

  if (st == RADIOLIB_ERR_CRC_MISMATCH) {
    Serial.printf("[lora] CRC error (RSSI %.1f, SNR %.1f) - signal too weak or interference\n", link.rssi, link.snr);
    return;
  }
  if (st != RADIOLIB_ERR_NONE) { Serial.printf("[lora] readData failed: %d\n", st); return; }
  if (len != sizeof(m) || m.magic[0] != PKT_MAGIC0 || m.magic[1] != PKT_MAGIC1) {
    Serial.printf("[lora] ignoring foreign packet (%u bytes)\n", (unsigned)len);
    return;
  }
  if (m.version != PKT_VERSION) { Serial.printf("[lora] packet version %u, expected %u - update both sides\n", m.version, PKT_VERSION); return; }
  if (m.nodeId != READER_NODE_ID) { Serial.printf("[lora] packet from node %u ignored\n", m.nodeId); return; }

  // Duplicate / loss detection. HELLO or a big jump back means the reader rebooted.
  if (m.type == PKT_HELLO) {
    haveSeq = false;
  } else if (haveSeq) {
    uint16_t diff = uint16_t(m.seq - lastSeq);
    if (diff == 0) { Serial.printf("[lora] duplicate seq %u dropped\n", m.seq); return; }
    if (diff < 1000) lostPackets += diff - 1;
  }
  haveSeq = true;
  lastSeq = m.seq;
  lastPacketMs = millis();
  link.lostPackets = lostPackets;
  setReaderOnline(true);

  if (mqtt.connected()) {
    snprintf(jsonBuf, sizeof(jsonBuf), "{\"rssi_dbm\":%.1f,\"snr_db\":%.1f,\"freq_error_hz\":%ld,\"seq\":%u,\"type\":%u,\"lost_packets\":%lu}",
             link.rssi, link.snr, (long)link.freqErrorHz, m.seq, m.type, (unsigned long)lostPackets);
    mqtt.publish(T_LINK.c_str(), jsonBuf, false);
  }

  switch (m.type) {
    case PKT_HELLO:
      Serial.printf("[lora] reader node %u booted (RSSI %.1f, SNR %.1f)\n", m.nodeId, link.rssi, link.snr);
      break;

    case PKT_ERROR:
      Serial.printf("[lora] reader error: %s (%u in a row) RSSI %.1f\n", readErrorText(m.errorCode), m.consecutiveErrors, link.rssi);
      if (mqtt.connected()) {
        snprintf(jsonBuf, sizeof(jsonBuf), "{\"error\":\"%s\",\"code\":%u,\"consecutive_errors\":%u}",
                 readErrorText(m.errorCode), m.errorCode, m.consecutiveErrors);
        mqtt.publish(T_ERROR.c_str(), jsonBuf, false);
      }
      break;

    case PKT_READING:
      if (!buildStateJson(m, link, jsonBuf, sizeof(jsonBuf))) { Serial.println("[json] buffer too small"); return; }
      Serial.printf("[meter] %.3f m3  RSSI %.1f dBm  SNR %.1f dB  seq %u\n", m.volumeL / 1000.0, link.rssi, link.snr, m.seq);
      if (mqtt.connected() && !mqtt.publish(T_STATE.c_str(), jsonBuf, true))
        Serial.printf("[mqtt] state publish failed (%u bytes)\n", (unsigned)strlen(jsonBuf));
      break;

    default:
      Serial.printf("[lora] unknown packet type %u\n", m.type);
  }
}

// ------------------------------------------------------------------ Arduino entry points

void setup() {
  pinMode(PIN_LORA_NSS, OUTPUT);
  digitalWrite(PIN_LORA_NSS, HIGH);
  Serial.begin(115200);
  delay(200);
  Serial.println("Qalcosonic W1 LoRa -> MQTT publisher");

  int16_t st = loraBegin();
  loraReport(Serial, st);
  radioOk = (st == RADIOLIB_ERR_NONE);
  if (radioOk) {
    radio.setDio1Action(onDio1);
    startRx();
  }

  WiFi.mode(WIFI_STA);
  WiFi.setHostname(DEVICE_NAME);
  WiFi.setAutoReconnect(true);
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setBufferSize(1280);  // discovery payloads exceed the 256-byte default
  ensureWifi();
}

void loop() {
  ensureWifi();
  ensureMqtt();
  mqtt.loop();

  if (!radioOk) {
    static uint32_t lastTry = 0;
    if (millis() - lastTry > 10000) {
      lastTry = millis();
      int16_t st = loraBegin();
      loraReport(Serial, st);
      radioOk = (st == RADIOLIB_ERR_NONE);
      if (radioOk) {
        radio.setDio1Action(onDio1);
        startRx();
        if (mqtt.connected()) mqtt.publish(T_STATUS.c_str(), "online", true);
      }
    }
    return;
  }

  if (rxFlag) {
    rxFlag = false;
    handlePacket();
  }

  if (readerOnline && millis() - lastPacketMs > READER_TIMEOUT_MS) setReaderOnline(false);
}
