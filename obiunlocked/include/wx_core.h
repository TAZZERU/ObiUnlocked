// wx_core.h -- 868.3 MHz FSK weather sensors (portable, host-tested by tests/test_radio_cores.py).
//
//   LaCrosse IT+ (TX29, TX35-IT, TFA 30.3155/30.3156 ...)   17.241 kbps, sync 0x2DD4, 5 bytes + CRC-8 (0x31)
//   Bresser 5-in-1 / 6-in-1 / 7-in-1 weather stations        8.21 kbps, sync 0x2DD4, own checksums
//
// Every decoder validates its checksum before anything is reported, so radio noise never becomes a "sensor".
// Field layouts follow rtl_433; the Bresser 6-in-1/7-in-1 layouts are marked experimental in the UI.
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace wx {

enum Proto : uint8_t { P_NONE = 0, P_LACROSSE = 1, P_BRESSER5 = 2, P_BRESSER6 = 3, P_BRESSER7 = 4 };
const char *protoName(uint8_t p);

struct Reading {
  uint8_t  proto = P_NONE;
  uint32_t id = 0;
  uint8_t  channel = 0;
  bool hasTemp = false, hasHum = false, hasWind = false, hasRain = false, hasUv = false, hasLight = false,
       hasBatt = false;
  bool  battLow = false, newBatt = false;
  float tempC = 0, windAvg = 0, windGust = 0, windDir = 0, rainMm = 0, uv = 0;
  uint8_t  hum = 0;
  uint32_t lux = 0;
};

uint8_t  crc8(const uint8_t *p, size_t n, uint8_t poly, uint8_t init);
uint16_t lfsrDigest16(const uint8_t *p, size_t n, uint16_t gen, uint16_t key);

bool decodeLaCrosse(const uint8_t *p, size_t n, Reading &r);   // p = bytes after the 0x2DD4 sync word
bool decodeBresser(const uint8_t *p, size_t n, Reading &r);    // tries 7-in-1, 6-in-1, 5-in-1

}   // namespace wx
