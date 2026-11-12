// lorawan_core.cpp -- see include/lorawan_core.h
#include "lorawan_core.h"

#include <stdio.h>
#include <string.h>

#include "wmbus_core.h"   // aes128EncryptBlock + aesCmac

namespace lw {

static void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }

bool parse(const uint8_t *p, size_t n, Uplink &u) {
  u = Uplink();
  if (n < 1) return false;
  u.mtype = p[0] >> 5;
  if (u.mtype == 0) {                                  // join request: MHDR JoinEUI(8) DevEUI(8) DevNonce(2) MIC(4)
    if (n != 23) return false;
    memcpy(u.devEui, p + 9, 8);
    return true;
  }
  if (u.mtype != 2 && u.mtype != 4) return false;      // only data uplinks
  if (n < 12) return false;
  u.devaddr = (uint32_t)p[1] | ((uint32_t)p[2] << 8) | ((uint32_t)p[3] << 16) | ((uint32_t)p[4] << 24);
  u.fctrl = p[5];
  u.fcnt16 = (uint16_t)(p[6] | (p[7] << 8));
  size_t hdr = 8 + (u.fctrl & 0x0F);
  if (n < hdr + 4) return false;
  if (n > hdr + 4) {
    u.hasPort = true;
    u.fport = p[hdr];
    u.payOff = (uint8_t)(hdr + 1);
    u.payLen = (uint8_t)(n - hdr - 1 - 4);
  }
  return true;
}

bool micOk(const uint8_t nwkSKey[16], const uint8_t *phy, size_t n, uint32_t devaddr, uint32_t fcnt32) {
  if (n < 5 || n - 4 > 255) return false;
  uint8_t msg[16 + 255];
  memset(msg, 0, 16);
  msg[0] = 0x49;
  msg[5] = 0;                       // uplink
  put32(msg + 6, devaddr);
  put32(msg + 10, fcnt32);
  msg[15] = (uint8_t)(n - 4);
  memcpy(msg + 16, phy, n - 4);
  uint8_t mac[16];
  wmb::aesCmac(nwkSKey, msg, 16 + n - 4, mac);
  return !memcmp(mac, phy + n - 4, 4);
}

void crypt(const uint8_t key[16], uint32_t devaddr, uint32_t fcnt32, uint8_t dir, uint8_t *data, size_t n) {
  uint8_t a[16], s[16];
  for (size_t off = 0, i = 1; off < n; off += 16, i++) {
    memset(a, 0, 16);
    a[0] = 0x01; a[5] = dir;
    put32(a + 6, devaddr);
    put32(a + 10, fcnt32);
    a[15] = (uint8_t)i;
    wmb::aes128EncryptBlock(key, a, s);
    for (size_t k = 0; k < 16 && off + k < n; k++) data[off + k] ^= s[k];
  }
}

int decodeUplink(const uint8_t *phy, size_t n, const Uplink &u, const uint8_t nwkSKey[16],
                 const uint8_t appSKey[16], uint16_t fcntHigh, uint8_t *out, size_t outMax, uint32_t &fcnt32Out) {
  if (u.mtype != 2 && u.mtype != 4) return -1;
  for (uint32_t hi = fcntHigh; hi <= (uint32_t)fcntHigh + 1 && hi <= 0xFFFF; hi++) {
    uint32_t f = (hi << 16) | u.fcnt16;
    if (!micOk(nwkSKey, phy, n, u.devaddr, f)) continue;
    fcnt32Out = f;
    if (!u.hasPort || u.payLen > outMax) return 0;
    memcpy(out, phy + u.payOff, u.payLen);
    crypt(u.fport == 0 ? nwkSKey : appSKey, u.devaddr, f, 0, out, u.payLen);
    return u.payLen;
  }
  return -1;
}

// ---- Cayenne LPP
struct LppType { uint8_t type, size, axes; bool sign; double div; const char *key, *label, *unit, *dc; };
static const LppType LPP[] = {
  { 0x00, 1, 1, false, 1, "din", "Digitaleingang", "", nullptr },
  { 0x01, 1, 1, false, 1, "dout", "Digitalausgang", "", nullptr },
  { 0x02, 2, 1, true, 100, "ain", "Analogeingang", "", nullptr },
  { 0x03, 2, 1, true, 100, "aout", "Analogausgang", "", nullptr },
  { 0x65, 2, 1, false, 1, "lux", "Helligkeit", "lx", "illuminance" },
  { 0x66, 1, 1, false, 1, "presence", "Präsenz", "", nullptr },
  { 0x67, 2, 1, true, 10, "temp", "Temperatur", "°C", "temperature" },
  { 0x68, 1, 1, false, 2, "hum", "Luftfeuchte", "%", "humidity" },
  { 0x71, 2, 3, true, 1000, "accel", "Beschleunigung", "g", nullptr },
  { 0x73, 2, 1, false, 10, "pressure", "Luftdruck", "hPa", "atmospheric_pressure" },
  { 0x74, 2, 1, false, 100, "voltage", "Spannung", "V", "voltage" },
  { 0x75, 2, 1, false, 1000, "current", "Strom", "A", "current" },
  { 0x76, 4, 1, false, 1, "freq", "Frequenz", "Hz", "frequency" },
  { 0x78, 1, 1, false, 1, "percent", "Prozent", "%", nullptr },
  { 0x79, 2, 1, true, 1, "altitude", "Höhe", "m", "distance" },
  { 0x7D, 2, 1, false, 1, "co2", "Konzentration", "ppm", "carbon_dioxide" },
  { 0x80, 2, 1, false, 1, "power", "Leistung", "W", "power" },
  { 0x82, 4, 1, false, 1000, "distance", "Entfernung", "m", "distance" },
  { 0x83, 4, 1, false, 1000, "energy", "Energie", "kWh", "energy" },
  { 0x84, 2, 1, false, 1, "direction", "Richtung", "°", nullptr },
  { 0x86, 2, 3, true, 100, "gyro", "Drehrate", "°/s", nullptr },
  { 0x88, 3, 3, true, 0, "gps", "GPS", "", nullptr },   // lat/lon /10000, alt /100 (special-cased)
  { 0x8E, 1, 1, false, 1, "switch", "Schalter", "", nullptr },
};

static const LppType *lppFind(uint8_t t) {
  for (const auto &x : LPP) if (x.type == t) return &x;
  return nullptr;
}

int parseCayenne(const uint8_t *p, size_t n, LppSink sink, void *ctx) {
  // pass 1: the whole buffer must be valid LPP, otherwise it's some other format -> report nothing
  size_t i = 0;
  while (i < n) {
    if (i + 2 > n) return -1;
    const LppType *t = lppFind(p[i + 1]);
    if (!t || i + 2 + (size_t)t->size * t->axes > n) return -1;
    i += 2 + (size_t)t->size * t->axes;
  }
  if (!n) return -1;
  int count = 0;
  for (i = 0; i < n;) {
    uint8_t ch = p[i];
    const LppType *t = lppFind(p[i + 1]);
    const uint8_t *d = p + i + 2;
    static const char *AX[3] = { "x", "y", "z" };
    static const char *GPS[3] = { "lat", "lon", "alt" };
    for (int a = 0; a < t->axes; a++) {
      int64_t v = 0;
      for (int b = 0; b < t->size; b++) v = (v << 8) | d[a * t->size + b];   // big-endian
      if (t->sign && (d[a * t->size] & 0x80)) v -= (int64_t)1 << (8 * t->size);
      double div = t->type == 0x88 ? (a < 2 ? 10000.0 : 100.0) : t->div;
      char key[24], label[40];
      if (t->axes > 1) {
        const char *ax = t->type == 0x88 ? GPS[a] : AX[a];
        snprintf(key, sizeof key, "%s_%s_%u", t->key, ax, ch);
        snprintf(label, sizeof label, "%s %s (Kanal %u)", t->label, ax, ch);
      } else {
        snprintf(key, sizeof key, "%s_%u", t->key, ch);
        snprintf(label, sizeof label, "%s (Kanal %u)", t->label, ch);
      }
      const char *unit = t->type == 0x88 ? (a < 2 ? "°" : "m") : t->unit;
      sink(key, label, (double)v / div, unit, t->dc, ctx);
      count++;
    }
    i += 2 + (size_t)t->size * t->axes;
  }
  return count;
}

}   // namespace lw
