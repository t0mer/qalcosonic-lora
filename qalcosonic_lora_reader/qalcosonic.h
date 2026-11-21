// qalcosonic.h - reads an Axioma Qalcosonic W1 over NFC (PN5180).
//
// Protocol ported from dbmaxpayne/esphome_qalcosonicnfc (Mark Hermann, 2025,
// LGPL v2.1+). The meter's ST25DV04K tag exposes a "mailbox" (Fast Transfer
// Mode). Wired M-Bus frames are written into it and the meter answers in it:
//   1. ISO 15693 inventory -> tag UID
//   2. check energy-harvesting state (so the meter's battery isn't used)
//   3. enable the mailbox
//   4. M-Bus SND_NKE  (10 40 3E 16) - reset application
//   5. M-Bus REQ_UD2  (10 7B 79 16) - request user data
//   6. read the M-Bus long frame (68 L L 68 ...) and decode DIF/VIF records
#pragma once

#include <Arduino.h>
#include "PN5180ISO15693.h"
#include "meter_packet.h"

// Values are kept in the meter's integer units (see meter_packet.h).
struct MeterReading {
  bool ok = false;
  uint8_t error = RE_NONE;   // ReadError when ok == false
  uint16_t present = 0;      // PresentBits

  uint32_t meterId = 0;
  uint32_t serialNumber = 0;
  String manufacturer;       // "AXI" expected
  int32_t volumeL = 0, volumePosL = 0, volumeNegL = 0;
  int16_t flowLh = 0, waterTemp_c100 = 0, extTemp_c10 = 0;
  uint8_t batteryPct = 0;
  uint8_t errorFlags[4] = {0, 0, 0, 0};
  uint8_t meterTime[4] = {0, 0, 0, 0};
  uint32_t operatingTimeS = 0;

  String tagUid;             // for the serial log
  String rawHex;             // M-Bus reply, for the serial log
};

class QalcosonicMeter {
 public:
  QalcosonicMeter(PN5180ISO15693 &nfc) : nfc_(nfc) {}

  bool begin();               // false if the PN5180 does not answer
  String pn5180Versions() const { return versions_; }
  MeterReading read();        // one full readout; RF field is off afterwards

 private:
  bool st25ReadDynConfig(uint8_t pointerAddress);
  bool st25WriteDynConfig(uint8_t pointerAddress, uint8_t value);
  bool st25MailboxEnable();
  bool st25WriteMessage(const uint8_t *message, uint8_t messageLength);
  bool st25GetMessageLength();
  bool st25GetMessage();
  bool issueMeterCommand(const uint8_t *cmd, uint8_t cmdLen);
  bool validateMbusFrame(String &why);
  void parseMbus(MeterReading &r);

  PN5180ISO15693 &nfc_;
  uint8_t uid_[8] = {0};
  uint8_t *buf_ = nullptr;   // points into the PN5180 driver's receive buffer
  uint16_t len_ = 0;
  bool needsReset_ = false;
  String versions_;
};
