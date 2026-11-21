// meter_packet.h - over-the-air format between reader and publisher.
// THIS FILE MUST BE IDENTICAL IN BOTH SKETCH FOLDERS.
//
// Fixed-size binary packet (little endian, packed). Values are kept in the
// meter's own integer units so nothing is lost to float rounding; the
// publisher converts to m³ / °C. LoRa's own CRC protects the payload.
#pragma once

#include <stdint.h>

#define PKT_MAGIC0   'Q'
#define PKT_MAGIC1   'W'
#define PKT_VERSION  1

enum PacketType : uint8_t {
  PKT_READING = 1,   // a successful meter readout
  PKT_ERROR   = 2,   // the reader is alive but could not read the meter
  PKT_HELLO   = 3,   // sent once at boot
};

// Why a readout failed (PKT_ERROR.errorCode). Text in readErrorText().
enum ReadError : uint8_t {
  RE_NONE = 0,
  RE_PN5180_INIT,     // PN5180 does not answer on SPI
  RE_RF_SETUP,        // PN5180 could not switch the RF field on
  RE_PN5180_RESET,
  RE_NO_METER,        // ISO 15693 inventory found no tag (antenna position)
  RE_EH_READ,         // could not read the ST25 energy-harvesting register
  RE_EH_INACTIVE,     // field too weak to power the ST25 (move reader closer)
  RE_MAILBOX,         // could not enable the ST25 mailbox
  RE_SND_NKE,         // meter did not answer M-Bus SND_NKE
  RE_REQ_UD2,         // meter did not answer M-Bus REQ_UD2
  RE_BAD_FRAME,       // M-Bus frame length/terminator/checksum wrong
  RE_NO_VOLUME,       // frame valid but no volume record
};

inline const char *readErrorText(uint8_t e) {
  switch (e) {
    case RE_NONE: return "ok";
    case RE_PN5180_INIT: return "PN5180 not responding";
    case RE_RF_SETUP: return "PN5180 RF setup failed";
    case RE_PN5180_RESET: return "PN5180 reset failed";
    case RE_NO_METER: return "meter not found (antenna position)";
    case RE_EH_READ: return "could not read ST25 energy-harvesting state";
    case RE_EH_INACTIVE: return "energy harvesting not active (field too weak)";
    case RE_MAILBOX: return "could not enable ST25 mailbox";
    case RE_SND_NKE: return "no reply to M-Bus SND_NKE";
    case RE_REQ_UD2: return "no reply to M-Bus REQ_UD2";
    case RE_BAD_FRAME: return "invalid M-Bus frame";
    case RE_NO_VOLUME: return "M-Bus frame had no volume record";
    default: return "unknown error";
  }
}

// Bits in MeterPacket.present: which optional fields the meter sent.
enum PresentBits : uint16_t {
  HAS_VOLUME       = 1 << 0,
  HAS_VOLUME_POS   = 1 << 1,
  HAS_VOLUME_NEG   = 1 << 2,
  HAS_FLOW         = 1 << 3,
  HAS_WATER_TEMP   = 1 << 4,
  HAS_EXT_TEMP     = 1 << 5,
  HAS_BATTERY      = 1 << 6,
  HAS_ERROR_FLAGS  = 1 << 7,
  HAS_METER_TIME   = 1 << 8,
  HAS_SERIAL       = 1 << 9,
  HAS_OPER_TIME    = 1 << 10,
};

struct __attribute__((packed)) MeterPacket {
  uint8_t  magic[2];          // 'Q','W'
  uint8_t  version;           // PKT_VERSION
  uint8_t  type;              // PacketType
  uint8_t  nodeId;            // lets several readers share one publisher
  uint16_t seq;               // increments per packet; publisher drops duplicates
  uint8_t  errorCode;         // ReadError (PKT_ERROR)
  uint8_t  consecutiveErrors; // reader-side failure streak, saturates at 255
  uint16_t present;           // PresentBits
  uint32_t meterId;           // 8-digit M-Bus secondary address as a number
  uint32_t serialNumber;      // VIF 0x78
  int32_t  volumeL;           // litres          (VIF 0x13)
  int32_t  volumePosL;        // litres, forward (VIF 0x93 0x3B)
  int32_t  volumeNegL;        // litres, reverse (VIF 0x93 0x3C)
  int16_t  flowLh;            // l/h             (VIF 0x3B)
  int16_t  waterTemp_c100;    // 0.01 °C         (VIF 0x59)
  int16_t  extTemp_c10;       // 0.1 °C          (VIF 0x66)
  uint8_t  batteryPct;        // %               (VIF 0xFD 0x74)
  uint8_t  errorFlags[4];     // meter alarm bytes (VIF 0xFD 0x17)
  uint8_t  meterTime[4];      // raw M-Bus type F date/time (VIF 0x6D)
  uint32_t operatingTimeS;    // VIF 0x24
  uint32_t readerUptimeS;
};

static_assert(sizeof(MeterPacket) == 54, "MeterPacket layout changed - update both sides");
