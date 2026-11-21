// PN5180.cpp - see PN5180.h for origin and license (LGPL v2.1+).
//
// Timing (delays, BUSY handshake, 2 MHz SPI) is kept identical to the
// ESPHome component, which is known to work against the Qalcosonic W1.
#include "PN5180.h"
#include "debug_log.h"

#define PN5180_WRITE_REGISTER           (0x00)
#define PN5180_WRITE_REGISTER_OR_MASK   (0x01)
#define PN5180_WRITE_REGISTER_AND_MASK  (0x02)
#define PN5180_READ_REGISTER            (0x04)
#define PN5180_READ_EEPROM              (0x07)
#define PN5180_SEND_DATA                (0x09)
#define PN5180_READ_DATA                (0x0A)
#define PN5180_LOAD_RF_CONFIG           (0x11)
#define PN5180_RF_ON                    (0x16)
#define PN5180_RF_OFF                   (0x17)

uint8_t PN5180::readBuffer_[508];

PN5180::PN5180(int8_t sck, int8_t miso, int8_t mosi, int8_t nss, int8_t busy, int8_t rst)
    : sck_(sck), miso_(miso), mosi_(mosi), nss_(nss), busy_(busy), rst_(rst),
      spiSettings_(2000000, MSBFIRST, SPI_MODE0) {}  // PN5180: CPOL=0, CPHA=0, max 7 MHz

void PN5180::begin() {
  pinMode(nss_, OUTPUT);
  pinMode(busy_, INPUT);
  pinMode(rst_, OUTPUT);
  digitalWrite(nss_, HIGH);  // deselected
  digitalWrite(rst_, HIGH);  // not in reset
  SPI.begin(sck_, miso_, mosi_, -1);  // NSS is driven manually
  LOGV("PN5180: begin SCK=%d MISO=%d MOSI=%d NSS=%d BUSY=%d RST=%d; levels NSS=%d BUSY=%d RST=%d",
       sck_, miso_, mosi_, nss_, busy_, rst_, digitalRead(nss_), digitalRead(busy_), digitalRead(rst_));
}

void PN5180::end() {
  digitalWrite(nss_, HIGH);
  SPI.end();
}

bool PN5180::writeRegister(uint8_t reg, uint32_t value) {
  uint8_t *p = (uint8_t *)&value;  // little endian on ESP32, as PN5180 expects
  uint8_t buf[6] = {PN5180_WRITE_REGISTER, reg, p[0], p[1], p[2], p[3]};
  return transceiveCommand(buf, sizeof(buf));
}

bool PN5180::writeRegisterWithOrMask(uint8_t reg, uint32_t mask) {
  uint8_t *p = (uint8_t *)&mask;
  uint8_t buf[6] = {PN5180_WRITE_REGISTER_OR_MASK, reg, p[0], p[1], p[2], p[3]};
  return transceiveCommand(buf, sizeof(buf));
}

bool PN5180::writeRegisterWithAndMask(uint8_t reg, uint32_t mask) {
  uint8_t *p = (uint8_t *)&mask;
  uint8_t buf[6] = {PN5180_WRITE_REGISTER_AND_MASK, reg, p[0], p[1], p[2], p[3]};
  return transceiveCommand(buf, sizeof(buf));
}

bool PN5180::readRegister(uint8_t reg, uint32_t *value) {
  uint8_t cmd[2] = {PN5180_READ_REGISTER, reg};
  if (!transceiveCommand(cmd, 2, (uint8_t *)value, 4)) {
    LOGV("PN5180: reading register %02X failed", reg);
    return false;
  }
  return true;
}

bool PN5180::readEEprom(uint8_t addr, uint8_t *buffer, uint8_t len) {
  if (addr > 254 || (addr + len) > 254) return false;
  uint8_t cmd[3] = {PN5180_READ_EEPROM, addr, len};
  return transceiveCommand(cmd, 3, buffer, len);
}

bool PN5180::sendData(uint8_t *data, int len, uint8_t validBits) {
  if (len > 260) return false;
  uint8_t buffer[len + 2];
  buffer[0] = PN5180_SEND_DATA;
  buffer[1] = validBits;
  memcpy(&buffer[2], data, len);

  writeRegisterWithAndMask(SYSTEM_CONFIG, 0xfffffff8);  // Idle/StopCom
  writeRegisterWithOrMask(SYSTEM_CONFIG, 0x00000003);   // Transceive

  if (getTransceiveState() != PN5180_TS_WaitTransmit) {
    LOGV("PN5180: transceiver not in WaitTransmit");
    return false;
  }
  return transceiveCommand(buffer, len + 2);
}

uint8_t *PN5180::readData(int len, uint8_t *buffer) {
  if (len > 508) return nullptr;
  if (buffer == nullptr) buffer = readBuffer_;
  uint8_t cmd[2] = {PN5180_READ_DATA, 0x00};
  if (!transceiveCommand(cmd, 2, buffer, len)) return nullptr;
  return buffer;
}

bool PN5180::loadRFConfig(uint8_t txConf, uint8_t rxConf) {
  uint8_t cmd[3] = {PN5180_LOAD_RF_CONFIG, txConf, rxConf};
  return transceiveCommand(cmd, 3);
}

bool PN5180::setRF_on() {
  uint8_t cmd[2] = {PN5180_RF_ON, 0x00};
  transceiveCommand(cmd, 2);
  if (!waitForIRQState(10, TX_RFON_IRQ_STAT)) return false;
  clearIRQStatus(TX_RFON_IRQ_STAT);
  return true;
}

bool PN5180::setRF_off() {
  uint8_t cmd[2] = {PN5180_RF_OFF, 0x00};
  transceiveCommand(cmd, 2);
  if (!waitForIRQState(10, TX_RFOFF_IRQ_STAT)) return false;
  clearIRQStatus(TX_RFOFF_IRQ_STAT);
  return true;
}

bool PN5180::waitForPinState(uint8_t maxWaitTimeMs, int8_t pin, int state) {
  uint32_t start = millis();
  while (millis() - start < maxWaitTimeMs && digitalRead(pin) != state) delay(1);
  return digitalRead(pin) == state;
}

bool PN5180::waitForIRQState(uint8_t maxWaitTimeMs, uint32_t irqState) {
  uint32_t start = millis();
  uint32_t current = getIRQStatus();
  while (millis() - start < maxWaitTimeMs && (current & irqState) == 0 &&
         (current & IRQ_READ_ERROR_IRQ_STAT) == 0) {
    delay(1);
    current = getIRQStatus();
  }
  return (current & irqState) != 0 && (current & IRQ_READ_ERROR_IRQ_STAT) == 0;
}

// Host interface handshake (PN5180 datasheet 11.4.3.1):
// NSS low -> exchange -> wait BUSY high -> NSS high -> wait BUSY low.
bool PN5180::transceiveCommand(uint8_t *sendBuffer, size_t sendLen, uint8_t *recvBuffer, size_t recvLen) {
  LOGV_HEX("PN5180 tx", sendBuffer, sendLen);
  if (!waitForPinState(10, busy_, LOW)) { LOGV("PN5180: BUSY stuck high (pre-send), cmd %02X", sendBuffer[0]); return false; }

  SPI.beginTransaction(spiSettings_);
  digitalWrite(nss_, LOW);
  delay(2);
  SPI.writeBytes(sendBuffer, sendLen);
  bool ok = waitForPinState(10, busy_, HIGH);
  digitalWrite(nss_, HIGH);
  SPI.endTransaction();
  if (!ok) { LOGV("PN5180: BUSY did not go high (send), cmd %02X - NSS not reaching chip or chip unpowered", sendBuffer[0]); return false; }
  delay(1);
  if (!waitForPinState(10, busy_, LOW)) { LOGV("PN5180: BUSY did not go low (send), cmd %02X", sendBuffer[0]); return false; }

  if (recvBuffer == nullptr || recvLen == 0) return true;

  SPI.beginTransaction(spiSettings_);
  digitalWrite(nss_, LOW);
  delay(2);
  for (size_t i = 0; i < recvLen; i++) recvBuffer[i] = SPI.transfer(0xFF);
  ok = waitForPinState(10, busy_, HIGH);
  digitalWrite(nss_, HIGH);
  SPI.endTransaction();
  LOGV_HEX("PN5180 rx", recvBuffer, recvLen);
  if (!ok) { LOGV("PN5180: BUSY did not go high (recv), cmd %02X", sendBuffer[0]); return false; }
  delay(1);
  if (!waitForPinState(10, busy_, LOW)) { LOGV("PN5180: BUSY did not go low (recv), cmd %02X", sendBuffer[0]); return false; }
  return true;
}

bool PN5180::reset() {
  LOGV("PN5180: reset start, BUSY=%d", digitalRead(busy_));
  digitalWrite(rst_, LOW);   // >10 us
  delay(1);
  LOGV("PN5180: RST low, BUSY=%d", digitalRead(busy_));
  digitalWrite(rst_, HIGH);  // 2.5 ms ramp-up
  delay(3);
  LOGV("PN5180: RST high +3 ms, BUSY=%d", digitalRead(busy_));
  if (!waitForIRQState(10, IDLE_IRQ_STAT)) {
    uint32_t irq = getIRQStatus();
    if (irq == IRQ_READ_ERROR_IRQ_STAT) LOGV("PN5180: reset failed - IRQ_STATUS register could not be read");
    else LOGV("PN5180: reset failed - IRQ_STATUS=%08lX, IDLE bit not set", (unsigned long)irq);
    return false;
  }
  LOGV("PN5180: reset OK");
  clearIRQStatus(0xffffffff);
  return true;
}

uint32_t PN5180::getIRQStatus() {
  uint32_t irq;
  if (!readRegister(IRQ_STATUS, &irq)) return IRQ_READ_ERROR_IRQ_STAT;
  return irq;
}

bool PN5180::clearIRQStatus(uint32_t irqMask) {
  return writeRegister(IRQ_CLEAR, irqMask);
}

PN5180TransceiveStat PN5180::getTransceiveState() {
  uint32_t rfStatus;
  if (!readRegister(RF_STATUS, &rfStatus)) return PN5180_TS_Idle;
  return PN5180TransceiveStat((rfStatus >> 24) & 0x07);
}
