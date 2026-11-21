// PN5180ISO15693.cpp - origin and license: see PN5180.h (LGPL v2.1+).
#include "PN5180ISO15693.h"
#include "debug_log.h"

// Inventory: flags 0x26 = high data rate + inventory + 1 slot, cmd 0x01, mask length 0.
ISO15693ErrorCode PN5180ISO15693::getInventory(uint8_t *uid) {
  uint8_t inventory[] = {0x26, 0x01, 0x00};
  memset(uid, 0, 8);
  uint8_t *resp;
  uint16_t len;
  ISO15693ErrorCode rc = issueISO15693Command(inventory, sizeof(inventory), &resp, &len);
  if (rc != ISO15693_EC_OK) return rc;
  for (int i = 0; i < 8; i++) uid[i] = resp[2 + i];  // [0]=flags, [1]=DSFID, [2..9]=UID (LSB first)
  return ISO15693_EC_OK;
}

ISO15693ErrorCode PN5180ISO15693::issueISO15693Command(uint8_t *cmd, uint8_t cmdLen, uint8_t **resultPtr,
                                                       uint16_t *responseLength) {
  LOGV_HEX("ISO15693 >", cmd, cmdLen);
  sendData(cmd, cmdLen);
  delay(10);

  uint32_t status = getIRQStatus();
  if ((status & RX_SOF_DET_IRQ_STAT) == 0) return EC_NO_CARD;

  // The ST25 mailbox reply can take a while: wait up to 500 ms for RX end.
  uint32_t waitStart = millis();
  while ((status & RX_IRQ_STAT) == 0) {
    if (millis() - waitStart > 500) {
      LOGV("ISO15693: timeout waiting for RX_IRQ");
      clearIRQStatus(0xffffffff);
      return ISO15693_EC_UNKNOWN_ERROR;
    }
    delay(10);
    status = getIRQStatus();
  }

  uint32_t rxStatus;
  readRegister(RX_STATUS, &rxStatus);
  *responseLength = (uint16_t)(rxStatus & 0x000001ff);

  *resultPtr = readData(*responseLength);
  if (*resultPtr == nullptr) return ISO15693_EC_UNKNOWN_ERROR;
  LOGV_HEX("ISO15693 <", *resultPtr, *responseLength);

  uint32_t irq = getIRQStatus();
  if ((irq & RX_SOF_DET_IRQ_STAT) == 0) {
    clearIRQStatus(TX_IRQ_STAT | IDLE_IRQ_STAT);
    return EC_NO_CARD;
  }

  uint8_t responseFlags = (*resultPtr)[0];
  if (responseFlags & 0x01) {  // error flag
    uint8_t errorCode = (*resultPtr)[1];
    LOGV("ISO15693: tag error 0x%02X", errorCode);
    return errorCode >= 0xA0 ? ISO15693_EC_CUSTOM_CMD_ERROR : (ISO15693ErrorCode)errorCode;
  }

  clearIRQStatus(RX_SOF_DET_IRQ_STAT | IDLE_IRQ_STAT | TX_IRQ_STAT | RX_IRQ_STAT);
  return ISO15693_EC_OK;
}

bool PN5180ISO15693::setupRF() {
  if (!loadRFConfig(0x0d, 0x8d)) return false;  // ISO 15693 ASK100, 26 kbit/s
  if (!setRF_on()) return false;
  writeRegisterWithAndMask(SYSTEM_CONFIG, 0xfffffff8);  // Idle/StopCom
  writeRegisterWithOrMask(SYSTEM_CONFIG, 0x00000003);   // Transceive
  return true;
}

const char *PN5180ISO15693::errorToStr(ISO15693ErrorCode ec) {
  switch (ec) {
    case EC_NO_CARD: return "no card detected";
    case ISO15693_EC_OK: return "OK";
    case ISO15693_EC_NOT_SUPPORTED: return "command not supported";
    case ISO15693_EC_NOT_RECOGNIZED: return "command not recognized";
    case ISO15693_EC_OPTION_NOT_SUPPORTED: return "option not supported";
    case ISO15693_EC_UNKNOWN_ERROR: return "unknown error / timeout";
    case ISO15693_EC_CUSTOM_CMD_ERROR: return "custom command error";
    default: return "undefined ISO15693 error";
  }
}
