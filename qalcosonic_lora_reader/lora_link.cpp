// lora_link.cpp - must be identical in both sketch folders.
#include "lora_link.h"
#include <SPI.h>

static Module loraModule(PIN_LORA_NSS, PIN_LORA_DIO1, PIN_LORA_RESET, PIN_LORA_BUSY);
SX1262 radio(&loraModule);

// RXEN stays HIGH except while transmitting: on the carrier board something pulls
// RXEN high, and driving it LOW sinks ~26 mA (measured in lora-door-sensor).
static const uint32_t rfSwitchPins[Module::RFSWITCH_MAX_PINS] = {
    PIN_LORA_RXEN, PIN_LORA_TXEN, RADIOLIB_NC, RADIOLIB_NC, RADIOLIB_NC};
static const Module::RfSwitchMode_t rfSwitchTable[] = {
    {Module::MODE_IDLE, {HIGH, LOW}},
    {Module::MODE_RX, {HIGH, LOW}},
    {Module::MODE_TX, {LOW, HIGH}},
    END_OF_MODE_TABLE,
};

static constexpr uint16_t REG_XTA_TRIM = 0x0911;
static constexpr uint16_t REG_XTB_TRIM = 0x0912;

int16_t loraApplyTrim() {
  int16_t state = radio.standby(RADIOLIB_SX126X_STANDBY_XOSC);  // trim only writable in STDBY_XOSC
  if (state != RADIOLIB_ERR_NONE) return state;
  loraModule.SPIwriteRegister(REG_XTA_TRIM, LORA_XTA_TRIM);
  loraModule.SPIwriteRegister(REG_XTB_TRIM, LORA_XTB_TRIM);
  return radio.standby();
}

int16_t loraBegin() {
  // On the ESP32, SPI.begin() is a no-op if the bus is already up, so whichever
  // side calls it first (PN5180 on the reader) sets the pins; this keeps them identical.
  SPI.begin(PIN_LORA_SCK, PIN_LORA_MISO, PIN_LORA_MOSI, -1);

  radio.tcxoVoltage = VoltageNone;  // plain crystal, nothing on DIO3
  radio.useRegulatorLDO = false;    // DC-DC

  ConfigLoRa_t cfg;
  cfg.frequency = RADIOLIB_UNIT_MEGA_FLT(LORA_FREQ_MHZ);
  cfg.bandwidth = RADIOLIB_UNIT_KILO(LORA_BW_KHZ);
  cfg.spreadingFactor = LORA_SF;
  cfg.codingRate = LORA_CR;
  cfg.syncWord = LORA_SYNC_WORD;
  cfg.power = LORA_TX_POWER_DBM;
  cfg.preambleLength = LORA_PREAMBLE;
  int16_t state = radio.begin(cfg);
  if (state != RADIOLIB_ERR_NONE) return state;

  radio.setRfSwitchTable(rfSwitchPins, rfSwitchTable);
  state = radio.setDio2AsRfSwitch(false);
  if (state != RADIOLIB_ERR_NONE) return state;
  state = radio.setCRC(2);
  if (state != RADIOLIB_ERR_NONE) return state;
  return loraApplyTrim();
}

void loraReport(Print &log, int16_t state) {
  if (state == RADIOLIB_ERR_NONE) {
    log.printf("[radio] OK  %.3f MHz SF%d BW%d kHz %d dBm\n", (double)LORA_FREQ_MHZ, LORA_SF, LORA_BW_KHZ,
               LORA_TX_POWER_DBM);
  } else {
    log.printf("[radio] FAIL (RadioLib code %d) - check LR30 wiring and 3V3 power\n", state);
  }
}
