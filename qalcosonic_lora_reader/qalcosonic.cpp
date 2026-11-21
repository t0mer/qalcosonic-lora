// qalcosonic.cpp - origin and license: see qalcosonic.h (LGPL v2.1+).
#include "qalcosonic.h"
#include "debug_log.h"

// ST25DV04K custom ISO 15693 commands
#define ST25_WRITE_MESSAGE     (0xAA)
#define ST25_READ_MSG_LENGTH   (0xAB)
#define ST25_READ_MSG          (0xAC)
#define ST25_READ_DYN_CONFIG   (0xAD)
#define ST25_WRITE_DYN_CONFIG  (0xAE)
#define ST25_MFG_CODE          (0x02)   // STMicroelectronics
#define ST25_REQUEST_FLAGS     (0x22)   // high data rate + addressed (UID in request)

// Dynamic registers
#define ST25_MB_CTRL_DYN       (0x0D)   // mailbox control
#define ST25_MB_EN             (0x01)
#define ST25_EH_CTRL_DYN       (0x02)   // energy harvesting control
#define ST25_EH_EN             (0x01)
#define ST25_EH_ON             (0x02)
#define ST25_FIELD_ON          (0x04)
#define ST25_VCC_ON            (0x08)

#define MBUS_LONG_FRAME_START            (0x68)
#define MBUS_LONG_FRAME_ADDITIONAL_BYTES (0x06)
#define MBUS_FRAME_TERMINATOR            (0x16)

static String toHex(const uint8_t *b, size_t n, const char *sep = "") {
  String s;
  s.reserve(n * (2 + strlen(sep)));
  char tmp[3];
  for (size_t i = 0; i < n; i++) {
    snprintf(tmp, sizeof(tmp), "%02X", b[i]);
    s += tmp;
    if (*sep && i + 1 < n) s += sep;
  }
  return s;
}

static uint32_t bcd8(const uint8_t *b) {  // 4 bytes BCD, LSB byte first
  return (b[3] >> 4) * 10000000UL + (b[3] & 0x0F) * 1000000UL +
         (b[2] >> 4) * 100000UL + (b[2] & 0x0F) * 10000UL +
         (b[1] >> 4) * 1000UL + (b[1] & 0x0F) * 100UL +
         (b[0] >> 4) * 10UL + (b[0] & 0x0F);
}

static int32_t le32(const uint8_t *b) {
  return int32_t(uint32_t(b[3]) << 24 | uint32_t(b[2]) << 16 | uint32_t(b[1]) << 8 | b[0]);
}

static int16_t le16(const uint8_t *b) { return int16_t(uint16_t(b[1]) << 8 | b[0]); }

// ---------------------------------------------------------------- setup

bool QalcosonicMeter::begin() {
  nfc_.begin();
  if (!nfc_.reset()) { versions_ = "reset failed, no IDLE IRQ"; return false; }

  uint8_t pv[2] = {0xFF, 0xFF}, fw[2] = {0, 0}, ee[2] = {0, 0};
  bool okPv = nfc_.readEEprom(PRODUCT_VERSION, pv, 2);
  bool okFw = nfc_.readEEprom(FIRMWARE_VERSION, fw, 2);
  bool okEe = nfc_.readEEprom(EEPROM_VERSION, ee, 2);
  LOGV("PN5180: EEPROM reads product %s %02X %02X, firmware %s %02X %02X, eeprom %s %02X %02X",
       okPv ? "ok" : "FAIL", pv[0], pv[1], okFw ? "ok" : "FAIL", fw[0], fw[1], okEe ? "ok" : "FAIL", ee[0], ee[1]);
  if (pv[1] == 0xFF) LOGV("PN5180: product version 0xFF - MISO idle high, chip not driving it");
  char b[64];
  snprintf(b, sizeof(b), "product %u.%u, firmware %u.%u, eeprom %u.%u", pv[1], pv[0], fw[1], fw[0], ee[1], ee[0]);
  versions_ = b;
  return pv[1] != 0xFF;  // 255 means the PN5180 didn't really answer
}

// ---------------------------------------------------------------- ST25 helpers

bool QalcosonicMeter::st25ReadDynConfig(uint8_t pointerAddress) {
  uint8_t cmd[12] = {ST25_REQUEST_FLAGS, ST25_READ_DYN_CONFIG, ST25_MFG_CODE};
  memcpy(&cmd[3], uid_, 8);
  cmd[11] = pointerAddress;
  return nfc_.issueISO15693Command(cmd, sizeof(cmd), &buf_, &len_) == ISO15693_EC_OK;
}

bool QalcosonicMeter::st25WriteDynConfig(uint8_t pointerAddress, uint8_t value) {
  uint8_t cmd[13] = {ST25_REQUEST_FLAGS, ST25_WRITE_DYN_CONFIG, ST25_MFG_CODE};
  memcpy(&cmd[3], uid_, 8);
  cmd[11] = pointerAddress;
  cmd[12] = value;
  return nfc_.issueISO15693Command(cmd, sizeof(cmd), &buf_, &len_) == ISO15693_EC_OK;
}

bool QalcosonicMeter::st25MailboxEnable() {
  if (!st25WriteDynConfig(ST25_MB_CTRL_DYN, ST25_MB_EN)) return false;
  if (!st25ReadDynConfig(ST25_MB_CTRL_DYN)) return false;
  return (buf_[1] & ST25_MB_EN) != 0;
}

bool QalcosonicMeter::st25WriteMessage(const uint8_t *message, uint8_t messageLength) {
  uint8_t cmd[12 + messageLength];
  cmd[0] = ST25_REQUEST_FLAGS;
  cmd[1] = ST25_WRITE_MESSAGE;
  cmd[2] = ST25_MFG_CODE;
  memcpy(&cmd[3], uid_, 8);
  cmd[11] = messageLength - 1;  // ST25 encodes length as N-1
  memcpy(&cmd[12], message, messageLength);
  return nfc_.issueISO15693Command(cmd, sizeof(cmd), &buf_, &len_) == ISO15693_EC_OK;
}

bool QalcosonicMeter::st25GetMessageLength() {
  uint8_t cmd[11] = {ST25_REQUEST_FLAGS, ST25_READ_MSG_LENGTH, ST25_MFG_CODE};
  memcpy(&cmd[3], uid_, 8);
  return nfc_.issueISO15693Command(cmd, sizeof(cmd), &buf_, &len_) == ISO15693_EC_OK;
}

bool QalcosonicMeter::st25GetMessage() {
  st25GetMessageLength();  // result in buf_[1] (length - 1)
  uint8_t cmd[13] = {ST25_REQUEST_FLAGS, ST25_READ_MSG, ST25_MFG_CODE};
  memcpy(&cmd[3], uid_, 8);
  cmd[11] = 0x00;     // mailbox pointer
  cmd[12] = buf_[1];  // message length
  return nfc_.issueISO15693Command(cmd, sizeof(cmd), &buf_, &len_) == ISO15693_EC_OK;
}

// Builds an M-Bus short frame: cmd bytes + checksum (sum of all but the start byte) + 0x16.
bool QalcosonicMeter::issueMeterCommand(const uint8_t *cmd, uint8_t cmdLen) {
  uint8_t frame[cmdLen + 2];
  uint8_t cs = 0;
  for (uint8_t i = 0; i < cmdLen; i++) {
    frame[i] = cmd[i];
    if (i > 0) cs += cmd[i];
  }
  frame[cmdLen] = cs;
  frame[cmdLen + 1] = MBUS_FRAME_TERMINATOR;
  if (!st25WriteMessage(frame, cmdLen + 2)) return false;
  return st25GetMessage();
}

bool QalcosonicMeter::validateMbusFrame(String &why) {
  // Response layout: [0]=ISO 15693 flags, [1..]=M-Bus long frame, last byte unknown (F0).
  if (len_ < 8 || buf_[1] != MBUS_LONG_FRAME_START) {
    why = "no M-Bus long frame in reply";
    return false;
  }
  uint8_t L = buf_[2];
  uint16_t startOfPayload = 5;
  uint16_t endOfPayload = startOfPayload + L;
  uint16_t endOfFrame = MBUS_LONG_FRAME_ADDITIONAL_BYTES + L;
  uint16_t crcPos = endOfFrame - 1;
  uint16_t total = endOfFrame + 2;
  if (total != len_) { why = "unexpected reply length " + String(len_) + ", expected " + String(total); return false; }
  if (buf_[endOfFrame] != MBUS_FRAME_TERMINATOR) { why = "missing M-Bus stop byte"; return false; }
  uint8_t cs = 0;
  for (uint16_t i = startOfPayload; i < endOfPayload; i++) cs += buf_[i];
  if (buf_[crcPos] != cs) { why = "M-Bus checksum mismatch"; return false; }
  return true;
}

// ---------------------------------------------------------------- read cycle

MeterReading QalcosonicMeter::read() {
  MeterReading r;

  auto fail = [&](uint8_t code, const String &detail = String()) -> MeterReading & {
    r.ok = false;
    r.error = code;
    needsReset_ = true;
    nfc_.setRF_off();
    if (detail.length()) LOGE("%s: %s", readErrorText(code), detail.c_str());
    return r;
  };

  if (!nfc_.setupRF()) { r.error = RE_RF_SETUP; needsReset_ = true; return r; }

  if (needsReset_) {  // previous cycle failed: reset the PN5180 like the original does
    if (!nfc_.reset() || !nfc_.setupRF()) { r.error = RE_PN5180_RESET; return r; }
    needsReset_ = false;
  }

  ISO15693ErrorCode rc = nfc_.getInventory(uid_);
  if (rc != ISO15693_EC_OK) return fail(RE_NO_METER, PN5180ISO15693::errorToStr(rc));
  uint8_t uidMsbFirst[8];
  for (int i = 0; i < 8; i++) uidMsbFirst[i] = uid_[7 - i];
  r.tagUid = toHex(uidMsbFirst, 8, ":");  // same order NFC Tools shows

  // Refuse to talk unless the ST25 runs on harvested RF energy, so the meter's
  // own battery is not used for NFC (as the original does).
  if (!st25ReadDynConfig(ST25_EH_CTRL_DYN)) return fail(RE_EH_READ);
  uint8_t eh = buf_[1];
  if (!(eh & ST25_EH_EN) || !(eh & ST25_EH_ON) || !(eh & ST25_FIELD_ON) || !(eh & ST25_VCC_ON)) {
    char b[48];
    snprintf(b, sizeof(b), "EH_EN=%u EH_ON=%u FIELD=%u VCC=%u", eh & 1, (eh >> 1) & 1, (eh >> 2) & 1, (eh >> 3) & 1);
    return fail(RE_EH_INACTIVE, b);
  }

  if (!st25MailboxEnable()) return fail(RE_MAILBOX);

  static const uint8_t SND_NKE[] = {0x10, 0x40, 0xFE};  // reset application, broadcast address
  static const uint8_t REQ_UD2[] = {0x10, 0x7B, 0xFE};  // request class 2 user data
  if (!issueMeterCommand(SND_NKE, sizeof(SND_NKE))) return fail(RE_SND_NKE);
  if (!issueMeterCommand(REQ_UD2, sizeof(REQ_UD2))) return fail(RE_REQ_UD2);

  r.rawHex = toHex(buf_, len_);
  String why;
  if (!validateMbusFrame(why)) return fail(RE_BAD_FRAME, why + " | raw " + r.rawHex);

  parseMbus(r);  // buf_ still holds the reply; parse before the driver buffer is reused
  nfc_.setRF_off();
  if (!(r.present & HAS_VOLUME)) { r.error = RE_NO_VOLUME; needsReset_ = true; return r; }
  r.ok = true;
  return r;
}

// ---------------------------------------------------------------- M-Bus decoding

void QalcosonicMeter::parseMbus(MeterReading &r) {
  const uint8_t *b = buf_;
  const int n = len_;

  int s = -1;  // locate 68 L L 68
  for (int i = 0; i <= n - 6; i++) {
    if (b[i] == 0x68 && b[i + 3] == 0x68 && b[i + 1] == b[i + 2]) { s = i; break; }
  }
  if (s < 0) return;
  uint8_t L = b[s + 1];
  if (s + 4 + L + 2 > n) return;

  uint8_t ci = b[s + 6];
  int payload = s + 7;
  if (ci == 0x72 || ci == 0x7A) payload += 12;  // long/short data header

  r.meterId = bcd8(&b[s + 7]);
  if (ci == 0x72 || ci == 0x7A) {
    uint16_t man = uint16_t(b[s + 12]) << 8 | b[s + 11];
    char m[4] = {char(((man >> 10) & 0x1F) + 64), char(((man >> 5) & 0x1F) + 64), char((man & 0x1F) + 64), 0};
    r.manufacturer = m;
  }

  const uint8_t *p = b + payload;
  const uint8_t *end = b + s + 4 + L;

  while (p < end) {
    uint8_t dif = *p++;
    if (dif == 0x0F || dif == 0x1F) break;               // manufacturer data / more records follow
    while ((p[-1] & 0x80) && p < end) p++;               // skip DIFEs
    if (p >= end) break;

    uint8_t vif = *p++;
    uint8_t vife = 0;
    if (vif & 0x80) { if (p >= end) break; vife = *p++; }
    while ((p[-1] & 0x80) && p < end) p++;               // skip further VIFEs

    uint8_t size = 0;
    switch (dif & 0x0F) {
      case 0x01: case 0x09: size = 1; break;
      case 0x02: case 0x0A: size = 2; break;
      case 0x03: case 0x0B: size = 3; break;
      case 0x04: case 0x05: case 0x0C: size = 4; break;
      case 0x06: case 0x0E: size = 6; break;
      case 0x07: size = 8; break;
      case 0x0D: size = (p < end) ? (*p + 1) : 0; break;  // variable length
      default: size = 0; break;
    }
    if (p + size > end) break;

    switch (uint16_t(vif) << 8 | vife) {
      case 0x1300: r.present |= HAS_VOLUME;     r.volumeL = le32(p); break;
      case 0x933B: r.present |= HAS_VOLUME_POS; r.volumePosL = le32(p); break;
      case 0x933C: r.present |= HAS_VOLUME_NEG; r.volumeNegL = le32(p); break;
      case 0x3B00: r.present |= HAS_FLOW;       r.flowLh = le16(p); break;
      case 0x5900: r.present |= HAS_WATER_TEMP; r.waterTemp_c100 = le16(p); break;
      case 0x6600: r.present |= HAS_EXT_TEMP;   r.extTemp_c10 = le16(p); break;
      case 0xFD74: r.present |= HAS_BATTERY;    r.batteryPct = p[0]; break;
      case 0x2400: r.present |= HAS_OPER_TIME;  r.operatingTimeS = uint32_t(le32(p)); break;
      case 0x7800:
        r.present |= HAS_SERIAL;
        r.serialNumber = ((dif & 0x0F) == 0x0C) ? bcd8(p) : uint32_t(le32(p));
        break;
      case 0x6D00: r.present |= HAS_METER_TIME;  memcpy(r.meterTime, p, 4); break;
      case 0xFD17: r.present |= HAS_ERROR_FLAGS; memcpy(r.errorFlags, p, 4); break;
    }
    p += size;
  }
}
