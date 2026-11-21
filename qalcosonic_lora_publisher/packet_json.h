// packet_json.h - turns a received MeterPacket into the MQTT state JSON.
// Plain C++ (no Arduino types) so it can be unit-tested on a PC.
#pragma once

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "meter_packet.h"

struct LinkInfo {
  float rssi;
  float snr;
  int32_t freqErrorHz;
  uint32_t lostPackets;  // since publisher boot
};

namespace pj {

struct Buf {
  char *p;
  size_t cap, len;
  bool overflow;
  void add(const char *fmt, ...) __attribute__((format(printf, 2, 3)));
};

inline void Buf::add(const char *fmt, ...) {
  if (overflow) return;
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(p + len, cap - len, fmt, ap);
  va_end(ap);
  if (n < 0 || (size_t)n >= cap - len) { overflow = true; return; }
  len += n;
}

inline const char *tf(bool b) { return b ? "true" : "false"; }

// M-Bus type F date/time -> "YYYY-MM-DD HH:MM" (meter clock, no DST)
inline void meterTimeStr(const uint8_t *t, char *out, size_t n) {
  int minute = t[0] & 0x3F, hour = t[1] & 0x1F, day = t[2] & 0x1F, month = t[3] & 0x0F;
  int year = ((t[2] >> 5) | ((t[3] >> 1) & 0x78)) + 2000;
  snprintf(out, n, "%04d-%02d-%02d %02d:%02d", year, month, day, hour, minute);
}

}  // namespace pj

// Returns false if the buffer was too small.
inline bool buildStateJson(const MeterPacket &m, const LinkInfo &l, char *out, size_t cap) {
  pj::Buf b{out, cap, 0, false};
  out[0] = 0;
  const uint16_t h = m.present;

  b.add("{");
  if (h & HAS_VOLUME) b.add("\"volume_m3\":%.3f", m.volumeL / 1000.0); else b.add("\"volume_m3\":null");
  if (h & HAS_VOLUME_POS) b.add(",\"volume_positive_m3\":%.3f", m.volumePosL / 1000.0);
  if (h & HAS_VOLUME_NEG) b.add(",\"volume_negative_m3\":%.3f", m.volumeNegL / 1000.0);
  if (h & HAS_FLOW) b.add(",\"flow_m3h\":%.3f", m.flowLh / 1000.0);
  if (h & HAS_WATER_TEMP) b.add(",\"water_temp_c\":%.2f", m.waterTemp_c100 / 100.0);
  if (h & HAS_EXT_TEMP) b.add(",\"external_temp_c\":%.1f", m.extTemp_c10 / 10.0);
  if (h & HAS_BATTERY) b.add(",\"battery_pct\":%u", m.batteryPct);
  if (h & HAS_OPER_TIME) b.add(",\"operating_time_s\":%lu", (unsigned long)m.operatingTimeS);
  if (h & HAS_METER_TIME) {
    char t[20];
    pj::meterTimeStr(m.meterTime, t, sizeof(t));
    b.add(",\"meter_time\":\"%s\"", t);
  }
  b.add(",\"meter_id\":\"%08lu\"", (unsigned long)m.meterId);
  if (h & HAS_SERIAL) b.add(",\"serial_number\":\"%08lu\"", (unsigned long)m.serialNumber);

  if (h & HAS_ERROR_FLAGS) {
    const uint8_t *e = m.errorFlags;
    // Bit mapping as documented by the upstream project (LCD error digits 1..4).
    b.add(",\"error_flags_raw\":\"%02X %02X %02X %02X\"", e[0], e[1], e[2], e[3]);
    b.add(",\"alarms\":{\"reconfiguration_warning\":%s,\"no_consumption\":%s,\"housing_damage\":%s,"
          "\"calculator_hw_failure\":%s,\"leakage\":%s,\"burst\":%s,\"optical_comm_stopped\":%s,"
          "\"low_battery\":%s,\"software_failure\":%s,\"hardware_failure\":%s,\"no_signal_empty_pipe\":%s,"
          "\"reverse_flow\":%s,\"flow_rate_over_q4\":%s,\"freeze_alert\":%s}",
          pj::tf(e[0] & 0x02), pj::tf(e[0] & 0x04), pj::tf(e[0] & 0x08), pj::tf(e[0] & 0x10),
          pj::tf(e[1] & 0x02), pj::tf(e[1] & 0x04), pj::tf(e[1] & 0x08), pj::tf(e[1] & 0x10),
          pj::tf(e[2] & 0x08), pj::tf(e[2] & 0x10),
          pj::tf(e[3] & 0x02), pj::tf(e[3] & 0x04), pj::tf(e[3] & 0x08), pj::tf(e[3] & 0x10));
  }

  b.add(",\"lora\":{\"rssi_dbm\":%.1f,\"snr_db\":%.1f,\"freq_error_hz\":%ld,\"seq\":%u,\"lost_packets\":%lu}",
        l.rssi, l.snr, (long)l.freqErrorHz, m.seq, (unsigned long)l.lostPackets);
  b.add(",\"reader_uptime_s\":%lu}", (unsigned long)m.readerUptimeS);
  return !b.overflow;
}
