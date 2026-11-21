// PN5180ISO15693.h - ISO 15693 layer on top of PN5180.
// Origin and license: see PN5180.h (LGPL v2.1+).
#pragma once

#include "PN5180.h"

enum ISO15693ErrorCode {
  EC_NO_CARD = -1,
  ISO15693_EC_OK = 0,
  ISO15693_EC_NOT_SUPPORTED = 0x01,
  ISO15693_EC_NOT_RECOGNIZED = 0x02,
  ISO15693_EC_OPTION_NOT_SUPPORTED = 0x03,
  ISO15693_EC_UNKNOWN_ERROR = 0x0f,
  ISO15693_EC_CUSTOM_CMD_ERROR = 0xA0
};

class PN5180ISO15693 : public PN5180 {
 public:
  using PN5180::PN5180;

  // Sends a raw ISO 15693 request. On success *resultPtr points to the
  // response (byte 0 = response flags) and *responseLength is its length.
  ISO15693ErrorCode issueISO15693Command(uint8_t *cmd, uint8_t cmdLen, uint8_t **resultPtr, uint16_t *responseLength);
  ISO15693ErrorCode getInventory(uint8_t *uid);
  bool setupRF();
  static const char *errorToStr(ISO15693ErrorCode ec);
};
