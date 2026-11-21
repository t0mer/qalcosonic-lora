// PN5180.h - NXP PN5180 host-interface driver for Arduino-ESP32.
//
// Ported to plain Arduino SPI from the ESPHome version in
// dbmaxpayne/esphome_qalcosonicnfc (Mark Hermann, 2025), which itself is
// based on ATrappmann/PN5180-Library (Andreas Trappmann, 2018).
// Licensed under the GNU LGPL v2.1 or later, like the originals.
#pragma once

#include <Arduino.h>
#include <SPI.h>

// PN5180 registers
#define SYSTEM_CONFIG       (0x00)
#define IRQ_ENABLE          (0x01)
#define IRQ_STATUS          (0x02)
#define IRQ_CLEAR           (0x03)
#define TRANSCEIVE_CONTROL  (0x04)
#define RX_STATUS           (0x13)
#define RF_STATUS           (0x1d)

// PN5180 EEPROM addresses
#define PRODUCT_VERSION     (0x10)
#define FIRMWARE_VERSION    (0x12)
#define EEPROM_VERSION      (0x14)

// IRQ_STATUS bits
#define RX_IRQ_STAT               (1UL << 0)
#define TX_IRQ_STAT               (1UL << 1)
#define IDLE_IRQ_STAT             (1UL << 2)
#define TX_RFOFF_IRQ_STAT         (1UL << 8)
#define TX_RFON_IRQ_STAT          (1UL << 9)
#define RX_SOF_DET_IRQ_STAT       (1UL << 14)
#define IRQ_READ_ERROR_IRQ_STAT   (1UL << 31)  // synthetic: register read failed

enum PN5180TransceiveStat {
  PN5180_TS_Idle = 0, PN5180_TS_WaitTransmit = 1, PN5180_TS_Transmitting = 2,
  PN5180_TS_WaitReceive = 3, PN5180_TS_WaitForData = 4, PN5180_TS_Receiving = 5,
  PN5180_TS_LoopBack = 6, PN5180_TS_RESERVED = 7
};

class PN5180 {
 public:
  PN5180(int8_t sck, int8_t miso, int8_t mosi, int8_t nss, int8_t busy, int8_t rst);

  void begin();
  void end();

  bool writeRegister(uint8_t reg, uint32_t value);
  bool writeRegisterWithOrMask(uint8_t reg, uint32_t mask);
  bool writeRegisterWithAndMask(uint8_t reg, uint32_t mask);
  bool readRegister(uint8_t reg, uint32_t *value);
  bool readEEprom(uint8_t addr, uint8_t *buffer, uint8_t len);
  bool sendData(uint8_t *data, int len, uint8_t validBits = 0);
  uint8_t *readData(int len, uint8_t *buffer = nullptr);
  bool loadRFConfig(uint8_t txConf, uint8_t rxConf);
  bool setRF_on();
  bool setRF_off();

  bool reset();
  uint32_t getIRQStatus();
  bool clearIRQStatus(uint32_t irqMask);
  PN5180TransceiveStat getTransceiveState();

 protected:
  bool waitForPinState(uint8_t maxWaitTimeMs, int8_t pin, int state);
  bool waitForIRQState(uint8_t maxWaitTimeMs, uint32_t irqState);
  bool transceiveCommand(uint8_t *sendBuffer, size_t sendLen, uint8_t *recvBuffer = nullptr, size_t recvLen = 0);

  int8_t sck_, miso_, mosi_, nss_, busy_, rst_;
  SPISettings spiSettings_;
  static uint8_t readBuffer_[508];
};
