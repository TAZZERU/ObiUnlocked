// wx_core.cpp -- see include/wx_core.h
#include "wx_core.h"

#include <string.h>

namespace wx {

const char *protoName(uint8_t p) {
  switch (p) {
    case P_LACROSSE: return "LaCrosse IT+";
    case P_BRESSER5: return "Bresser 5-in-1";
    case P_BRESSER6: return "Bresser 6-in-1";
    case P_BRESSER7: return "Bresser 7-in-1";
    default: return "?";
  }
}

uint8_t crc8(const uint8_t *p, size_t n, uint8_t poly, uint8_t init) {
  uint8_t c = init;
  for (size_t i = 0; i < n; i++) {
    c ^= p[i];
    for (int b = 0; b < 8; b++) c = (c & 0x80) ? (uint8_t)((c << 1) ^ poly) : (uint8_t)(c << 1);
  }
  return c;
}

uint16_t lfsrDigest16(const uint8_t *p, size_t n, uint16_t gen, uint16_t key) {
  uint16_t sum = 0;
  for (size_t k = 0; k < n; k++)
    for (int i = 7; i >= 0; i--) {
      if ((p[k] >> i) & 1) sum ^= key;
      key = (key & 1) ? (uint16_t)((key >> 1) ^ gen) : (uint16_t)(key >> 1);
    }
  return sum;
}

static bool bcdOk(uint8_t b) { return (b >> 4) <= 9 && (b & 0x0F) <= 9; }
static int bcd2(uint8_t b) { return (b >> 4) * 10 + (b & 0x0F); }

bool decodeLaCrosse(const uint8_t *p, size_t n, Reading &r) {
  if (n < 5 || (p[0] >> 4) != 0x9 || crc8(p, 4, 0x31, 0) != p[4]) return false;
  uint8_t d1 = p[1] & 0x0F, d2 = p[2] >> 4, d3 = p[2] & 0x0F;
  if (d1 > 9 || d2 > 9 || d3 > 9) return false;
  r = Reading();
  r.proto = P_LACROSSE;
  r.id = (uint32_t)(((p[0] & 0x0F) << 2) | (p[1] >> 6));
  r.newBatt = (p[1] >> 5) & 1;
  r.hasTemp = true;
  r.tempC = (d1 * 100 + d2 * 10 + d3) / 10.0f - 40.0f;
  r.hasBatt = true;
  r.battLow = p[3] >> 7;
  uint8_t h = p[3] & 0x7F;
  if (h == 0x7D) r.id += 0x40;                     // second probe of a two-channel sensor, no humidity
  else if (h <= 100) { r.hasHum = true; r.hum = h; }   // 0x6A = sensor without humidity
  return true;
}

// 5-in-1: 26 bytes; bytes 0..12 are the bit-inverse of 13..25 (that is the whole integrity check).
static bool bresser5(const uint8_t *m, size_t n, Reading &r) {
  if (n < 26) return false;
  for (int i = 0; i < 13; i++) if ((m[i] ^ m[i + 13]) != 0xFF) return false;
  r = Reading();
  r.proto = P_BRESSER5;
  r.id = m[14];
  int t = (m[20] & 0x0F) + (m[20] >> 4) * 10 + (m[21] & 0x0F) * 100;
  if (m[25] & 0x0F) t = -t;
  r.hasTemp = true; r.tempC = t / 10.0f;
  r.hasHum = true; r.hum = (uint8_t)((m[22] & 0x0F) + (m[22] >> 4) * 10);
  r.hasWind = true;
  r.windDir = (m[17] >> 4) * 22.5f;
  r.windGust = (((m[17] & 0x0F) << 8) | m[16]) / 10.0f;
  r.windAvg = ((m[18] & 0x0F) + (m[18] >> 4) * 10 + (m[19] & 0x0F) * 100) / 10.0f;
  r.hasRain = true;
  r.rainMm = ((m[23] & 0x0F) + (m[23] >> 4) * 10 + (m[24] & 0x0F) * 100 + (m[24] >> 4) * 1000) / 10.0f;
  r.hasBatt = true; r.battLow = (m[25] & 0x80) != 0;
  return true;
}

// 6-in-1: 18 bytes, LFSR-16 digest (gen 0x8810, key 0x5412) over 2..16 + byte sum over 2..17 == 0xFF.
static bool bresser6(const uint8_t *src, size_t n, Reading &r) {
  if (n < 18) return false;
  uint8_t m[18];
  memcpy(m, src, 18);
  if (((m[0] << 8) | m[1]) != lfsrDigest16(m + 2, 15, 0x8810, 0x5412)) return false;
  unsigned sum = 0;
  for (int i = 2; i < 18; i++) sum += m[i];
  if ((sum & 0xFF) != 0xFF) return false;
  r = Reading();
  r.proto = P_BRESSER6;
  r.id = ((uint32_t)m[2] << 24) | ((uint32_t)m[3] << 16) | ((uint32_t)m[4] << 8) | m[5];
  r.channel = m[6] & 0x07;
  int type = m[6] >> 4;                       // 1 weather station, 2 thermo/hygro, 3 pool, 4 soil
  if (m[16] & 1) {                            // rain telegram: bytes 12..14 = inverted 6-digit BCD
    uint8_t a = (uint8_t)~m[12], b = (uint8_t)~m[13], c = (uint8_t)~m[14];
    if (bcdOk(a) && bcdOk(b) && bcdOk(c)) { r.hasRain = true; r.rainMm = (bcd2(a) * 10000 + bcd2(b) * 100 + bcd2(c)) / 10.0f; }
  } else if (bcdOk(m[12]) && (m[13] >> 4) <= 9 && bcdOk(m[14])) {   // temperature/humidity telegram
    int t = bcd2(m[12]) * 10 + (m[13] >> 4);
    float c = t / 10.0f;
    if ((m[13] >> 3) & 1) { c = (t - 1000) / 10.0f; if (c < -50) c = -t / 10.0f; }
    r.hasTemp = true; r.tempC = c;
    r.hasHum = true; r.hum = (uint8_t)bcd2(m[14]);
    r.hasBatt = true; r.battLow = !((m[13] >> 1) & 1);
  }
  uint8_t w7 = (uint8_t)~m[7], w8 = (uint8_t)~m[8], w9 = (uint8_t)~m[9];
  if (type != 2 && type != 4 && bcdOk(w7) && bcdOk(w8) && bcdOk(w9) && bcdOk(m[10]) && (m[11] >> 4) <= 9) {
    r.hasWind = true;
    r.windGust = (bcd2(w7) * 10 + (w8 >> 4)) / 10.0f;
    r.windAvg = (bcd2(w9) * 10 + (w8 & 0x0F)) / 10.0f;
    r.windDir = (float)(bcd2(m[10]) * 10 + (m[11] >> 4));
  }
  return r.hasTemp || r.hasWind || r.hasRain;
}

// 7-in-1: 25 bytes XOR 0xAA, LFSR-16 digest (gen 0x8810, key 0xBA95) over 2..24, xor 0x6DF1.
static bool bresser7(const uint8_t *src, size_t n, Reading &r) {
  if (n < 25 || src[21] == 0x00) return false;
  uint8_t m[25];
  for (int i = 0; i < 25; i++) m[i] = src[i] ^ 0xAA;
  if ((((m[0] << 8) | m[1]) ^ lfsrDigest16(m + 2, 23, 0x8810, 0xBA95)) != 0x6DF1) return false;
  r = Reading();
  r.proto = P_BRESSER7;
  r.id = ((uint32_t)m[2] << 8) | m[3];
  r.channel = src[6] & 0x07;
  int type = src[6] >> 4;                     // read before whitening, like rtl_433
  if (type != 1 && type != 12 && type != 13) return false;   // air-quality variants: not decoded
  r.hasBatt = true; r.battLow = (m[15] & 0x06) == 0x06;
  r.hasWind = true;
  r.windDir = (float)(bcd2(m[4]) * 10 + (m[5] >> 4));
  r.windGust = ((m[7] >> 4) * 100 + (m[7] & 0x0F) * 10 + (m[8] >> 4)) / 10.0f;
  r.windAvg = ((m[8] & 0x0F) * 100 + bcd2(m[9])) / 10.0f;
  r.hasRain = true;
  r.rainMm = (bcd2(m[10]) * 10000 + bcd2(m[11]) * 100 + bcd2(m[12])) / 10.0f;
  int t = (m[14] >> 4) * 100 + (m[14] & 0x0F) * 10 + (m[15] >> 4);
  r.hasTemp = true; r.tempC = t > 600 ? (t - 1000) / 10.0f : t / 10.0f;
  r.hasHum = true; r.hum = (uint8_t)bcd2(m[16]);
  r.hasLight = true; r.lux = (uint32_t)(bcd2(m[17]) * 10000 + bcd2(m[18]) * 100 + bcd2(m[19]));
  r.hasUv = true; r.uv = ((m[20] >> 4) * 100 + (m[20] & 0x0F) * 10 + (m[21] >> 4)) / 10.0f;
  return true;
}

bool decodeBresser(const uint8_t *p, size_t n, Reading &r) {
  return bresser7(p, n, r) || bresser6(p, n, r) || bresser5(p, n, r);
}

}   // namespace wx
