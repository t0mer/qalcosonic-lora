// qalcosonic_lora_reader.ino
//
// Sits at the water meter: reads the Axioma Qalcosonic W1 over NFC (PN5180)
// and sends the values over LoRa (DX-LR30, 433 MHz) to the publisher.
// No WiFi needed on this side.
//
// Battery operation: every wake does one job and goes back to deep sleep.
//   wake -> init PN5180 + LR30 -> read meter -> send packet
//        -> LR30 to sleep, PN5180 held in reset (hard power-down) -> deep sleep
// setup() runs on every wake; loop() is never reached. State that must survive
// deep sleep (packet counter, error streak, next poll time) lives in RTC memory.
//
// With a power bank, KEEPALIVE_INTERVAL_MS > 0 cuts the sleep into short chunks;
// those "keep-alive" wakes only draw a current burst so the bank stays on.
//
// Libraries: RadioLib (Jan Gromes) from the Library Manager.
// Meter protocol ported from dbmaxpayne/esphome_qalcosonicnfc (LGPL v2.1+).

#include <SPI.h>
#include <esp_wifi.h>
#include <esp_bt.h>
#include <esp_sleep.h>
#include <driver/gpio.h>
#include <sys/time.h>
#include "config.h"
#if KEEPALIVE_WIFI
#include <WiFi.h>
#endif
#include "debug_log.h"
#include "lora_link.h"
#include "meter_packet.h"
#include "PN5180ISO15693.h"
#include "qalcosonic.h"

static PN5180ISO15693 nfc(PIN_PN5180_SCK, PIN_PN5180_MISO, PIN_PN5180_MOSI,
                          PIN_PN5180_NSS, PIN_PN5180_BUSY, PIN_PN5180_RST);
static QalcosonicMeter meter(nfc);

static bool pn5180Ok = false;
static bool radioOk = false;

// Survive deep sleep (cleared on power-on / reset).
RTC_DATA_ATTR static uint16_t seq = 0;
RTC_DATA_ATTR static uint8_t consecutiveErrors = 0;
RTC_DATA_ATTR static uint64_t nextPollMs = 0;  // monoMs() at which the next meter read is due

// Pins whose level is latched through deep sleep so neither chip wakes up or
// burns current while the ESP32 sleeps (see sleepNow()).
static const gpio_num_t HELD_PINS[] = {
    (gpio_num_t)PIN_LORA_NSS,   (gpio_num_t)PIN_LORA_RESET, (gpio_num_t)PIN_LORA_TXEN,
    (gpio_num_t)PIN_LORA_RXEN,  (gpio_num_t)PIN_PN5180_NSS, (gpio_num_t)PIN_PN5180_RST,
};

// Milliseconds since power-on. The ESP32 keeps this clock running through deep
// sleep (RTC timer), unlike millis(), which restarts at every wake.
static uint64_t monoMs() {
  struct timeval tv;
  gettimeofday(&tv, nullptr);
  return (uint64_t)tv.tv_sec * 1000ULL + tv.tv_usec / 1000;
}

static uint32_t uptimeS() { return (uint32_t)(monoMs() / 1000); }
static uint64_t wakeStartMs = 0;  // monoMs() when this wake's setup() began

static MeterPacket newPacket(uint8_t type) {
  MeterPacket p;
  memset(&p, 0, sizeof(p));
  p.magic[0] = PKT_MAGIC0;
  p.magic[1] = PKT_MAGIC1;
  p.version = PKT_VERSION;
  p.type = type;
  p.nodeId = NODE_ID;
  p.seq = seq++;
  p.consecutiveErrors = consecutiveErrors;
  p.readerUptimeS = uptimeS();
  return p;
}

static void sendPacket(const MeterPacket &p) {
  if (!radioOk) return;
  for (int i = 0; i < TX_REPEATS; i++) {
    if (i) delay(TX_REPEAT_GAP_MS);
    uint32_t t0 = millis();
    int16_t st = radio.transmit((const uint8_t *)&p, sizeof(p));
    if (st == RADIOLIB_ERR_NONE) {
      LOGI("LoRa: sent type %u seq %u (%u bytes, %lu ms)", p.type, p.seq, (unsigned)sizeof(p),
           (unsigned long)(millis() - t0));
    } else {
      LOGE("LoRa: transmit failed, RadioLib code %d", st);
    }
  }
  radio.standby();  // RX/TX switch back to idle (RXEN high, TXEN low)
}

static void pollMeter() {
  if (!pn5180Ok) {
    pn5180Ok = meter.begin();
    if (!pn5180Ok) {
      if (consecutiveErrors < 255) consecutiveErrors++;
      MeterPacket p = newPacket(PKT_ERROR);
      p.errorCode = RE_PN5180_INIT;
      sendPacket(p);
      return;
    }
    LOGI("PN5180 now OK (%s)", meter.pn5180Versions().c_str());
  }

  uint32_t t0 = millis();
  MeterReading r = meter.read();
  uint32_t dt = millis() - t0;

  if (!r.ok) {
    if (consecutiveErrors < 255) consecutiveErrors++;
    LOGE("Read failed (%u in a row): %s", consecutiveErrors, readErrorText(r.error));
    MeterPacket p = newPacket(PKT_ERROR);
    p.errorCode = r.error;
    sendPacket(p);
    return;
  }

  consecutiveErrors = 0;
  LOGI("Meter %08lu (%s, tag %s): %.3f m3, flow %d l/h, water %.2f C, battery %u%% (%lu ms)",
       (unsigned long)r.meterId, r.manufacturer.c_str(), r.tagUid.c_str(), r.volumeL / 1000.0,
       r.flowLh, r.waterTemp_c100 / 100.0, r.batteryPct, (unsigned long)dt);
  LOGV("M-Bus: %s", r.rawHex.c_str());

  MeterPacket p = newPacket(PKT_READING);
  p.present = r.present;
  p.meterId = r.meterId;
  p.serialNumber = r.serialNumber;
  p.volumeL = r.volumeL;
  p.volumePosL = r.volumePosL;
  p.volumeNegL = r.volumeNegL;
  p.flowLh = r.flowLh;
  p.waterTemp_c100 = r.waterTemp_c100;
  p.extTemp_c10 = r.extTemp_c10;
  p.batteryPct = r.batteryPct;
  memcpy(p.errorFlags, r.errorFlags, 4);
  memcpy(p.meterTime, r.meterTime, 4);
  p.operatingTimeS = r.operatingTimeS;
  sendPacket(p);
}

// The reader talks only LoRa: make sure the WiFi and Bluetooth radios are off
// and release the BT controller's RAM. The stop/deinit calls are harmless if
// the radios were never started.
static void radiosOff() {
  esp_wifi_stop();
  esp_wifi_deinit();
  btStop();
  esp_bt_controller_mem_release(ESP_BT_MODE_BTDM);
}

// Deep-sleeps until the next poll is due, or for one keep-alive chunk if that is
// shorter. The pins must already be latched (holds stay on across wakes).
static void deepSleepUntilNextPoll() {
  uint64_t now = monoMs();
  uint64_t sleepMs = nextPollMs > now + MIN_SLEEP_MS ? nextPollMs - now : MIN_SLEEP_MS;
  if (KEEPALIVE_INTERVAL_MS > 0 && sleepMs > KEEPALIVE_INTERVAL_MS) sleepMs = KEEPALIVE_INTERVAL_MS;
  gpio_deep_sleep_hold_en();
  esp_sleep_enable_timer_wakeup(sleepMs * 1000ULL);
  esp_deep_sleep_start();  // does not return; the next wake starts at setup()
}

// Keep-alive wake: draw current for a moment so the power bank stays on, then
// sleep again. The meter, PN5180 and LR30 are not touched; their pins stay latched.
static void keepAliveWake() {
  Serial.begin(115200);
  Serial.printf("[I] keep-alive pulse, next read in %lu s\n",
                (unsigned long)((nextPollMs - monoMs()) / 1000));
  Serial.flush();
#if KEEPALIVE_WIFI
  WiFi.mode(WIFI_STA);  // radio on, no connection attempt
  delay(KEEPALIVE_PULSE_MS);
  WiFi.mode(WIFI_OFF);
#else
  uint32_t t0 = millis();
  while (millis() - t0 < KEEPALIVE_PULSE_MS) {}  // busy CPU at full clock
#endif
  deepSleepUntilNextPoll();
}

// Puts both chips into their lowest-power state, latches the control pins and
// deep-sleeps so that the next meter read starts about `intervalMs` after this one.
static void sleepNow(uint32_t intervalMs) {
  // LR30: cold sleep (~0.2 uA). It is fully re-initialised on the next wake.
  if (radioOk) radio.sleep(false);
  // PN5180: RST low = hard power-down. meter.begin() resets it on the next wake.
  digitalWrite(PIN_PN5180_RST, LOW);
  SPI.end();

  // Without a hold the pins float in deep sleep: a floating NSS can wake the
  // LR30, RST low would reset it, and RXEN driven LOW sinks ~26 mA.
  pinMode(PIN_LORA_NSS, OUTPUT);   digitalWrite(PIN_LORA_NSS, HIGH);
  pinMode(PIN_PN5180_NSS, OUTPUT); digitalWrite(PIN_PN5180_NSS, HIGH);
  pinMode(PIN_LORA_RESET, OUTPUT); digitalWrite(PIN_LORA_RESET, HIGH);
  pinMode(PIN_LORA_TXEN, OUTPUT);  digitalWrite(PIN_LORA_TXEN, LOW);
  pinMode(PIN_LORA_RXEN, OUTPUT);  digitalWrite(PIN_LORA_RXEN, HIGH);
  for (gpio_num_t pin : HELD_PINS) gpio_hold_en(pin);

  nextPollMs = wakeStartMs + intervalMs;
  uint64_t now = monoMs();
  LOGI("Awake %lu ms, next read in %lu s%s", (unsigned long)(now - wakeStartMs),
       (unsigned long)((nextPollMs > now ? nextPollMs - now : 0) / 1000),
       KEEPALIVE_INTERVAL_MS > 0 ? " (power-bank keep-alive on)" : "");
  Serial.flush();
  deepSleepUntilNextPoll();
}

void setup() {
  wakeStartMs = monoMs();
  bool timerWake = esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_TIMER;

  // Not yet time to read the meter: this is a power-bank keep-alive wake.
  if (timerWake && wakeStartMs + MIN_SLEEP_MS < nextPollMs) keepAliveWake();

  // Release the pin latches from the previous sleep, or writes to them are ignored.
  gpio_deep_sleep_hold_dis();
  for (gpio_num_t pin : HELD_PINS) gpio_hold_dis(pin);

  radiosOff();

  // Both chips share the SPI bus: deselect both before anything talks on it.
  pinMode(PIN_PN5180_NSS, OUTPUT);
  digitalWrite(PIN_PN5180_NSS, HIGH);
  pinMode(PIN_LORA_NSS, OUTPUT);
  digitalWrite(PIN_LORA_NSS, HIGH);

  Serial.begin(115200);
  delay(50);

  bool powerOn = !timerWake;
  if (powerOn) LOGI("Qalcosonic W1 LoRa reader, node %d (power-on)", NODE_ID);
  else LOGI("Wake, seq %u, up %lu s", seq, (unsigned long)uptimeS());

  pn5180Ok = meter.begin();  // also starts the shared SPI bus
  if (pn5180Ok) LOGI("PN5180 OK (%s)", meter.pn5180Versions().c_str());
  else LOGE("PN5180 not responding (%s). Check 5V + 3.3V, BUSY, RST, NSS=GPIO%d.",
            meter.pn5180Versions().c_str(), PIN_PN5180_NSS);

  int16_t st = loraBegin();
  loraReport(Serial, st);
  radioOk = (st == RADIOLIB_ERR_NONE);

  if (!radioOk) {  // nothing useful to do without the radio: try again soon
    sleepNow(RADIO_RETRY_MS);
  }

  // HELLO only on a real boot; the publisher treats it as "reader restarted".
  if (powerOn) sendPacket(newPacket(PKT_HELLO));

  pollMeter();
  sleepNow(POLL_INTERVAL_MS);
}

void loop() {}  // never reached: setup() always ends in deep sleep
