// wmbus_core.cpp -- see include/wmbus_core.h. Pure C++, no Arduino: compiled natively by tests/test_wmbus.py.
#include "wmbus_core.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

namespace wmb {

// ============================================================================ data link layer
const char *modeName(uint8_t mode) {
  switch (mode) {
    case MODE_T1: return "T1";
    case MODE_C1A: return "C1a";
    case MODE_C1B: return "C1b";
    case MODE_S1: return "S1";
    default: return "?";
  }
}

uint16_t crc16(const uint8_t *p, size_t n) {
  uint16_t crc = 0;
  for (size_t i = 0; i < n; i++) {
    crc ^= (uint16_t)p[i] << 8;
    for (int b = 0; b < 8; b++) crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x3D65) : (uint16_t)(crc << 1);
  }
  return (uint16_t)~crc;
}

// EN 13757-4 table 10: 4-bit nibble -> 6-bit code. The decoder is the inverse (0xFF = invalid code).
static int8_t sym3of6(uint8_t code) {
  static const uint8_t ENC[16] = { 0x16, 0x0D, 0x0E, 0x0B, 0x1C, 0x19, 0x1A, 0x13,
                                   0x2C, 0x25, 0x26, 0x23, 0x34, 0x31, 0x32, 0x29 };
  for (int i = 0; i < 16; i++) if (ENC[i] == code) return (int8_t)i;
  return -1;
}

size_t decode3of6(const uint8_t *in, size_t inLen, uint8_t *out, size_t outMax) {
  size_t bitPos = 0, totalBits = inLen * 8, n = 0;
  while (n < outMax && bitPos + 12 <= totalBits) {
    int8_t nib[2];
    for (int k = 0; k < 2; k++) {
      uint8_t code = 0;
      for (int b = 0; b < 6; b++, bitPos++)
        code = (uint8_t)((code << 1) | ((in[bitPos >> 3] >> (7 - (bitPos & 7))) & 1));
      nib[k] = sym3of6(code);
      if (nib[k] < 0) return n;
    }
    out[n++] = (uint8_t)((nib[0] << 4) | nib[1]);
  }
  return n;
}

static bool crcOk(const uint8_t *blk, size_t n) {   // n data bytes, followed by the big-endian CRC
  uint16_t want = (uint16_t)((blk[n] << 8) | blk[n + 1]);
  return crc16(blk, n) == want;
}

// Format A: 10-byte first block + CRC, then 16-byte blocks + CRC each; L excludes the CRCs.
static RadioResult stripFormatA(const uint8_t *b, size_t bLen, Frame &f) {
  if (bLen < 12) return RR_TOO_SHORT;
  uint8_t L = b[0];
  if (L < 9) return RR_BAD_LEN;
  size_t rest = (size_t)L - 9;
  size_t need = 12 + rest + 2 * ((rest + 15) / 16);
  if (bLen < need) return RR_TOO_SHORT;
  if (!crcOk(b, 10)) return RR_BAD_CRC;
  memcpy(f.data, b, 10);
  size_t out = 10, pos = 12;
  while (rest) {
    size_t blk = rest > 16 ? 16 : rest;
    if (!crcOk(b + pos, blk)) return RR_BAD_CRC;
    memcpy(f.data + out, b + pos, blk);
    out += blk; pos += blk + 2; rest -= blk;
  }
  f.len = (uint16_t)out;
  return RR_OK;
}

// Format B: L includes the CRCs. Up to 128 bytes one CRC at the end; longer frames carry a second CRC
// after byte 125 (blocks 1+2) and one at the very end (block 3).
static RadioResult stripFormatB(const uint8_t *b, size_t bLen, Frame &f) {
  if (bLen < 1) return RR_TOO_SHORT;
  uint8_t L = b[0];
  if (L < 12) return RR_BAD_LEN;
  size_t total = (size_t)L + 1;
  if (bLen < total) return RR_TOO_SHORT;
  if (total <= 128) {
    if (!crcOk(b, total - 2)) return RR_BAD_CRC;
    memcpy(f.data, b, total - 2);
    f.len = (uint16_t)(total - 2);
  } else {
    if (!crcOk(b, 126)) return RR_BAD_CRC;
    size_t blk3 = total - 128 - 2;
    if (!crcOk(b + 128, blk3)) return RR_BAD_CRC;
    memcpy(f.data, b, 126);
    memcpy(f.data + 126, b + 128, blk3);
    f.len = (uint16_t)(126 + blk3);
  }
  return RR_OK;
}

RadioResult fromRadio(const uint8_t *raw, size_t rawLen, Frame &f) {
  f = Frame();
  if (rawLen < 3) return RR_TOO_SHORT;
  if (raw[0] == 0x54 && raw[1] == 0xCD) { f.mode = MODE_C1A; return stripFormatA(raw + 2, rawLen - 2, f); }
  if (raw[0] == 0x54 && raw[1] == 0x3D) { f.mode = MODE_C1B; return stripFormatB(raw + 2, rawLen - 2, f); }
  // T1: 3-out-of-6 coded (0x54 is not a valid 3-of-6 symbol start, so this split is unambiguous)
  uint8_t dec[FRAME_MAX + 2 * 16];
  size_t n = decode3of6(raw, rawLen, dec, sizeof dec);
  if (n < 12) return n < 2 ? RR_BAD_3OF6 : RR_TOO_SHORT;
  f.mode = MODE_T1;
  return stripFormatA(dec, n, f);
}

size_t decodeManchester(const uint8_t *in, size_t inLen, uint8_t *out, size_t outMax, bool invert) {
  size_t n = 0, bits = inLen * 8;
  for (size_t pos = 0; n < outMax && pos + 16 <= bits; n++) {
    uint8_t b = 0;
    for (int k = 0; k < 8; k++, pos += 2) {
      int c0 = (in[pos >> 3] >> (7 - (pos & 7))) & 1, c1 = (in[(pos + 1) >> 3] >> (7 - ((pos + 1) & 7))) & 1;
      if (c0 == c1) return n;
      b = (uint8_t)((b << 1) | (c0 ^ (invert ? 1 : 0)));
    }
    out[n] = b;
  }
  return n;
}

RadioResult fromRadioS1(const uint8_t *raw, size_t rawLen, Frame &f) {
  uint8_t dec[FRAME_MAX];
  RadioResult best = RR_BAD_3OF6;
  for (int inv = 0; inv < 2; inv++) {
    f = Frame();
    size_t n = decodeManchester(raw, rawLen, dec, sizeof dec, inv != 0);
    if (n < 12) continue;
    f.mode = MODE_S1;
    RadioResult r = stripFormatA(dec, n, f);
    if (r == RR_OK) return r;
    if (r == RR_BAD_CRC || best == RR_BAD_3OF6) best = r;   // most telling error: CRC > too short > coding
  }
  return best;
}

bool peekHeader(const uint8_t *raw, size_t rawLen, bool s1, uint8_t hdr[10], uint8_t &mode) {
  uint8_t b[12];
  if (s1) {
    for (int inv = 0; inv < 2; inv++)
      if (decodeManchester(raw, rawLen, b, 12, inv != 0) == 12 && crcOk(b, 10)) { memcpy(hdr, b, 10); mode = MODE_S1; return true; }
    return false;
  }
  if (rawLen >= 14 && raw[0] == 0x54 && raw[1] == 0xCD) {
    if (!crcOk(raw + 2, 10)) return false;
    memcpy(hdr, raw + 2, 10); mode = MODE_C1A; return true;
  }
  if (rawLen >= 2 && raw[0] == 0x54 && raw[1] == 0x3D) return false;
  if (decode3of6(raw, rawLen, b, 12) != 12 || !crcOk(b, 10)) return false;
  memcpy(hdr, b, 10); mode = MODE_T1;
  return true;
}

// ============================================================================ link / transport layer
void mfrCode(uint16_t m, char out[4]) {
  out[0] = (char)(((m >> 10) & 0x1F) + 64);
  out[1] = (char)(((m >> 5) & 0x1F) + 64);
  out[2] = (char)((m & 0x1F) + 64);
  out[3] = 0;
  for (int i = 0; i < 3; i++) if (out[i] < 'A' || out[i] > 'Z') out[i] = '?';
}

static uint32_t le32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

bool parseHeader(const Frame &f, Header &h) {
  h = Header();
  const uint8_t *d = f.data;
  size_t n = f.len;
  if (n < 11) return false;
  h.c = d[1];
  h.mfr = (uint16_t)(d[2] | (d[3] << 8));
  h.id = le32(d + 4);
  h.ver = d[8];
  h.type = d[9];
  size_t pos = 10;
  for (int guard = 0; guard < 4; guard++) {
    if (pos >= n) return false;
    uint8_t ci = d[pos];
    h.ci = ci;
    switch (ci) {
      case 0x8C:                                   // ELL I: CC ACC
        if (pos + 3 > n) return false;
        h.hasEll = true; pos += 3; continue;
      case 0x8E:                                   // ELL III: CC ACC M2 A6
        if (pos + 11 > n) return false;
        h.hasEll = true; pos += 11; continue;
      case 0x8D:                                   // ELL II / IV: payload AES-CTR encrypted on link layer
      case 0x8F:
        h.hasEll = true; h.ellEncrypted = true;
        h.apOff = (uint16_t)(pos + 1); h.apLen = (uint16_t)(n - pos - 1);
        return true;
      case 0x90: {                                 // AFL: CI AFLL FCL [MCL] [KI] [MCR] [MAC] [ML]
        if (pos + 4 > n) return false;
        uint8_t afll = d[pos + 1];
        size_t end = pos + 2 + afll;
        if (end > n) return false;
        size_t p = pos + 2;
        uint16_t fcl = (uint16_t)(d[p] | (d[p + 1] << 8));
        p += 2;
        h.hasAfl = true;
        if (fcl & 0x2000) p += 1;                  // MCL present
        if (fcl & 0x0200) p += 2;                  // KI present
        if ((fcl & 0x0800) && p + 4 <= end) { h.aflHasMcr = true; h.aflMcr = le32(d + p); }
        pos = end;
        continue;
      }
      case 0x7A: case 0x5A:                        // short TPL: ACC ST CW
        if (pos + 5 > n) return false;
        h.hasTpl = true;
        h.acc = d[pos + 1]; h.status = d[pos + 2];
        h.cw = (uint16_t)(d[pos + 3] | (d[pos + 4] << 8));
        pos += 5;
        break;
      case 0x72: case 0x5B:                        // long TPL: ID4 M2 VER TYPE ACC ST CW
        if (pos + 13 > n) return false;
        h.hasTpl = true; h.longTpl = true;
        h.tplId = le32(d + pos + 1);
        h.tplMfr = (uint16_t)(d[pos + 5] | (d[pos + 6] << 8));
        h.tplVer = d[pos + 7]; h.tplType = d[pos + 8];
        h.acc = d[pos + 9]; h.status = d[pos + 10];
        h.cw = (uint16_t)(d[pos + 11] | (d[pos + 12] << 8));
        pos += 13;
        break;
      default:                                     // 0x78 (no header), 0x79 compact, 0xA0..0xB7 mfr, ...
        pos += 1;
        break;
    }
    break;
  }
  if (h.hasTpl) { h.secMode = (uint8_t)((h.cw >> 8) & 0x1F); h.encBlocks = (uint8_t)((h.cw >> 4) & 0x0F); }
  if (pos > n) return false;
  h.apOff = (uint16_t)pos;
  h.apLen = (uint16_t)(n - pos);
  return true;
}

const char *deviceTypeName(uint8_t t) {
  switch (t) {
    case 0x00: return "other";              case 0x01: return "oil meter";
    case 0x02: return "electricity meter";  case 0x03: return "gas meter";
    case 0x04: return "heat meter";         case 0x05: return "steam meter";
    case 0x06: return "warm water meter";   case 0x07: return "water meter";
    case 0x08: return "heat cost allocator";case 0x09: return "compressed air";
    case 0x0A: return "cooling meter (outlet)"; case 0x0B: return "cooling meter (inlet)";
    case 0x0C: return "heat meter (inlet)"; case 0x0D: return "heat/cooling meter";
    case 0x0E: return "bus/system component"; case 0x0F: return "unknown medium";
    case 0x15: return "hot water meter";    case 0x16: return "cold water meter";
    case 0x17: return "dual water meter";   case 0x18: return "pressure meter";
    case 0x19: return "A/D converter";      case 0x1A: return "smoke detector";
    case 0x1B: return "room sensor";        case 0x1C: return "gas detector";
    case 0x20: return "breaker";            case 0x21: return "valve";
    case 0x25: return "customer unit";      case 0x28: return "waste water meter";
    case 0x29: return "garbage";            case 0x31: return "OMS MUC";
    case 0x32: return "OMS repeater (uni)"; case 0x33: return "OMS repeater (bi)";
    case 0x36: case 0x37: return "radio converter";
    default: return "unknown";
  }
}

const char *deviceTypeKey(uint8_t t) {
  switch (t) {
    case 0x02: return "electricity"; case 0x03: return "gas";
    case 0x04: case 0x0C: case 0x0D: return "heat";
    case 0x06: case 0x15: return "warm_water";
    case 0x07: case 0x16: case 0x17: return "water";
    case 0x08: return "hca";         case 0x1A: return "smoke";
    case 0x1B: return "room";        case 0x0A: case 0x0B: return "cooling";
    default: return "other";
  }
}

// ============================================================================ security
const char *decryptResultName(int8_t r) {
  switch (r) {
    case DR_PLAIN: return "plain";        case DR_OK: return "decrypted";
    case DR_NO_KEY: return "no_key";      case DR_BAD_KEY: return "wrong_key";
    case DR_UNSUPPORTED: return "unsupported"; default: return "malformed";
  }
}

static void shiftLeft1(const uint8_t in[16], uint8_t out[16]) {
  for (int i = 0; i < 16; i++) out[i] = (uint8_t)((in[i] << 1) | (i < 15 ? (in[i + 1] >> 7) : 0));
}

void aesCmac(const uint8_t key[16], const uint8_t *msg, size_t len, uint8_t mac[16]) {
  uint8_t zero[16] = {0}, L[16], k1[16], k2[16];
  aes128EncryptBlock(key, zero, L);
  shiftLeft1(L, k1);  if (L[0] & 0x80) k1[15] ^= 0x87;
  shiftLeft1(k1, k2); if (k1[0] & 0x80) k2[15] ^= 0x87;
  size_t nBlocks = len ? (len + 15) / 16 : 1;
  bool complete = len && (len % 16 == 0);
  uint8_t x[16] = {0}, y[16];
  for (size_t b = 0; b < nBlocks; b++) {
    uint8_t blk[16];
    if (b + 1 < nBlocks) {
      memcpy(blk, msg + 16 * b, 16);
    } else if (complete) {
      for (int i = 0; i < 16; i++) blk[i] = (uint8_t)(msg[16 * b + i] ^ k1[i]);
    } else {
      size_t rem = len - 16 * b;
      memset(blk, 0, 16);
      memcpy(blk, msg + 16 * b, rem);
      blk[rem] = 0x80;
      for (int i = 0; i < 16; i++) blk[i] ^= k2[i];
    }
    for (int i = 0; i < 16; i++) y[i] = (uint8_t)(x[i] ^ blk[i]);
    aes128EncryptBlock(key, y, x);
  }
  memcpy(mac, x, 16);
}

static void cbcDecrypt(const uint8_t key[16], const uint8_t iv[16], const uint8_t *in, size_t n, uint8_t *out) {
  uint8_t prev[16];
  memcpy(prev, iv, 16);
  for (size_t off = 0; off < n; off += 16) {
    uint8_t blk[16];
    aes128DecryptBlock(key, in + off, blk);
    for (int i = 0; i < 16; i++) blk[i] ^= prev[i];
    memcpy(prev, in + off, 16);   // in and out may alias, so keep the ciphertext block first
    memcpy(out + off, blk, 16);
  }
}

DecryptResult decrypt(const Frame &f, const Header &h, const uint8_t *key, uint8_t *out, size_t outMax,
                      size_t &outLen) {
  outLen = 0;
  if (h.apOff + h.apLen > f.len || h.apLen > outMax) return DR_MALFORMED;
  const uint8_t *ap = f.data + h.apOff;
  memcpy(out, ap, h.apLen);
  outLen = h.apLen;
  if (h.ellEncrypted) return DR_UNSUPPORTED;
  if (!h.hasTpl || h.secMode == 0) return DR_PLAIN;
  if (h.secMode != 5 && h.secMode != 7) return DR_UNSUPPORTED;
  if (!key) return DR_NO_KEY;
  size_t encLen = (size_t)h.encBlocks * 16;
  if (encLen == 0) encLen = h.apLen - (h.apLen % 16);
  if (encLen == 0 || encLen > h.apLen) return DR_MALFORMED;

  uint8_t iv[16], k[16];
  if (h.secMode == 5) {
    uint16_t m = h.meterMfr();
    uint32_t id = h.meterId();
    iv[0] = (uint8_t)m; iv[1] = (uint8_t)(m >> 8);
    iv[2] = (uint8_t)id; iv[3] = (uint8_t)(id >> 8); iv[4] = (uint8_t)(id >> 16); iv[5] = (uint8_t)(id >> 24);
    iv[6] = h.meterVer(); iv[7] = h.meterType();
    for (int i = 8; i < 16; i++) iv[i] = h.acc;
    memcpy(k, key, 16);
  } else {                                   // mode 7: Kenc = CMAC(K, 0x00 | MCR | ID | 0x07 * 7), IV = 0
    if (!h.aflHasMcr) return DR_MALFORMED;
    uint8_t in[16];
    uint32_t id = h.meterId();
    in[0] = 0x00;
    in[1] = (uint8_t)h.aflMcr; in[2] = (uint8_t)(h.aflMcr >> 8);
    in[3] = (uint8_t)(h.aflMcr >> 16); in[4] = (uint8_t)(h.aflMcr >> 24);
    in[5] = (uint8_t)id; in[6] = (uint8_t)(id >> 8); in[7] = (uint8_t)(id >> 16); in[8] = (uint8_t)(id >> 24);
    for (int i = 9; i < 16; i++) in[i] = 0x07;
    aesCmac(key, in, 16, k);
    memset(iv, 0, 16);
  }
  cbcDecrypt(k, iv, ap, encLen, out);
  if (out[0] != 0x2F || out[1] != 0x2F) {
    memcpy(out, ap, h.apLen);                // leave the ciphertext, not garbage, for display
    return DR_BAD_KEY;
  }
  return DR_OK;
}

// ============================================================================ application layer
static const QuantityInfo Q_INFO[Q_COUNT_] = {
  /* Q_UNKNOWN            */ { "value", "", nullptr, nullptr },
  /* Q_ENERGY             */ { "energy", "kWh", "energy", "total_increasing" },
  /* Q_VOLUME             */ { "volume", "m³", "volume", "total_increasing" },
  /* Q_MASS               */ { "mass", "kg", "weight", nullptr },
  /* Q_ON_TIME            */ { "on_time", "h", "duration", "total_increasing" },
  /* Q_OPERATING_TIME     */ { "operating_time", "h", "duration", "total_increasing" },
  /* Q_POWER              */ { "power", "W", "power", "measurement" },
  /* Q_VOLUME_FLOW        */ { "flow", "m³/h", "volume_flow_rate", "measurement" },
  /* Q_MASS_FLOW          */ { "mass_flow", "kg/h", nullptr, "measurement" },
  /* Q_FLOW_TEMP          */ { "flow_temp", "°C", "temperature", "measurement" },
  /* Q_RETURN_TEMP        */ { "return_temp", "°C", "temperature", "measurement" },
  /* Q_TEMP_DIFF          */ { "temp_diff", "K", nullptr, "measurement" },
  /* Q_EXTERNAL_TEMP      */ { "ext_temp", "°C", "temperature", "measurement" },
  /* Q_PRESSURE           */ { "pressure", "bar", "pressure", "measurement" },
  /* Q_DATE               */ { "date", "", "date", nullptr },
  /* Q_DATETIME           */ { "datetime", "", nullptr, nullptr },
  /* Q_HCA                */ { "hca", "", nullptr, "total_increasing" },
  /* Q_AVG_DURATION       */ { "avg_duration", "s", "duration", nullptr },
  /* Q_ACTUALITY_DURATION */ { "actuality", "s", "duration", nullptr },
  /* Q_FABRICATION_NO     */ { "fabrication_no", "", nullptr, nullptr },
  /* Q_ENHANCED_ID        */ { "enhanced_id", "", nullptr, nullptr },
  /* Q_BUS_ADDRESS        */ { "bus_address", "", nullptr, nullptr },
  /* Q_ACCESS_NO          */ { "access_no", "", nullptr, nullptr },
  /* Q_MEDIUM             */ { "medium", "", nullptr, nullptr },
  /* Q_MANUFACTURER       */ { "manufacturer", "", nullptr, nullptr },
  /* Q_PARAMETER_SET      */ { "parameter_set", "", nullptr, nullptr },
  /* Q_MODEL_VERSION      */ { "model_version", "", nullptr, nullptr },
  /* Q_HW_VERSION         */ { "hw_version", "", nullptr, nullptr },
  /* Q_FW_VERSION         */ { "fw_version", "", nullptr, nullptr },
  /* Q_SW_VERSION         */ { "sw_version", "", nullptr, nullptr },
  /* Q_CUSTOMER_LOCATION  */ { "customer_location", "", nullptr, nullptr },
  /* Q_CUSTOMER           */ { "customer", "", nullptr, nullptr },
  /* Q_ERROR_FLAGS        */ { "error_flags", "", nullptr, nullptr },
  /* Q_DIGITAL_OUTPUT     */ { "digital_out", "", nullptr, nullptr },
  /* Q_DIGITAL_INPUT      */ { "digital_in", "", nullptr, nullptr },
  /* Q_DIMENSIONLESS      */ { "dimensionless", "", nullptr, "measurement" },
  /* Q_VOLTAGE            */ { "voltage", "V", "voltage", "measurement" },
  /* Q_CURRENT            */ { "current", "A", "current", "measurement" },
  /* Q_RESET_COUNTER      */ { "reset_counter", "", nullptr, "total_increasing" },
  /* Q_CUMULATION_COUNTER */ { "cumulation_counter", "", nullptr, "total_increasing" },
  /* Q_BATTERY_DAYS       */ { "battery_days", "d", "duration", "measurement" },
  /* Q_HUMIDITY           */ { "humidity", "%", "humidity", "measurement" },
  /* Q_MFR_SPECIFIC       */ { "mfr_data", "", nullptr, nullptr },
  /* Q_ROOM_TEMP          */ { "room_temp", "°C", "temperature", "measurement" },
  /* Q_RADIATOR_TEMP      */ { "radiator_temp", "°C", "temperature", "measurement" },
  /* Q_BATTERY_YEARS      */ { "battery_years", "a", nullptr, "measurement" },
  /* Q_TX_PERIOD          */ { "transmit_period", "s", "duration", nullptr },
  /* Q_STATUS             */ { "status", "", nullptr, nullptr },
  /* Q_DUST               */ { "dust_level", "", nullptr, "measurement" },
  /* Q_CONSUMPTION        */ { "consumption", "", nullptr, "total_increasing" },
};

const QuantityInfo &quantityInfo(uint8_t q) { return Q_INFO[q < Q_COUNT_ ? q : 0]; }

static double p10(int e) { return pow(10.0, e); }

// time unit nn (0 s, 1 min, 2 h, 3 d) -> factor to hours / to seconds
static double nnHours(int nn) { static const double F[4] = { 1.0 / 3600, 1.0 / 60, 1, 24 }; return F[nn & 3]; }
static double nnSeconds(int nn) { static const double F[4] = { 1, 60, 3600, 86400 }; return F[nn & 3]; }

static void mapVif(uint8_t table, uint8_t c, uint8_t &q, double &mult) {
  q = Q_UNKNOWN; mult = 1;
  int n = c & 7, nn = c & 3;
  if (table == 0) {
    if (c <= 0x07)      { q = Q_ENERGY; mult = p10(n - 6); }               // Wh*10^(n-3) -> kWh
    else if (c <= 0x0F) { q = Q_ENERGY; mult = p10(n) / 3.6e6; }           // J -> kWh
    else if (c <= 0x17) { q = Q_VOLUME; mult = p10(n - 6); }
    else if (c <= 0x1F) { q = Q_MASS; mult = p10(n - 3); }
    else if (c <= 0x23) { q = Q_ON_TIME; mult = nnHours(nn); }
    else if (c <= 0x27) { q = Q_OPERATING_TIME; mult = nnHours(nn); }
    else if (c <= 0x2F) { q = Q_POWER; mult = p10(n - 3); }
    else if (c <= 0x37) { q = Q_POWER; mult = p10(n) / 3600.0; }           // J/h -> W
    else if (c <= 0x3F) { q = Q_VOLUME_FLOW; mult = p10(n - 6); }
    else if (c <= 0x47) { q = Q_VOLUME_FLOW; mult = p10(n - 7) * 60; }
    else if (c <= 0x4F) { q = Q_VOLUME_FLOW; mult = p10(n - 9) * 3600; }
    else if (c <= 0x57) { q = Q_MASS_FLOW; mult = p10(n - 3); }
    else if (c <= 0x5B) { q = Q_FLOW_TEMP; mult = p10(nn - 3); }
    else if (c <= 0x5F) { q = Q_RETURN_TEMP; mult = p10(nn - 3); }
    else if (c <= 0x63) { q = Q_TEMP_DIFF; mult = p10(nn - 3); }
    else if (c <= 0x67) { q = Q_EXTERNAL_TEMP; mult = p10(nn - 3); }
    else if (c <= 0x6B) { q = Q_PRESSURE; mult = p10(nn - 3); }
    else if (c == 0x6C) q = Q_DATE;
    else if (c == 0x6D) q = Q_DATETIME;
    else if (c == 0x6E) q = Q_HCA;
    else if (c >= 0x70 && c <= 0x73) { q = Q_AVG_DURATION; mult = nnSeconds(nn); }
    else if (c >= 0x74 && c <= 0x77) { q = Q_ACTUALITY_DURATION; mult = nnSeconds(nn); }
    else if (c == 0x78) q = Q_FABRICATION_NO;
    else if (c == 0x79) q = Q_ENHANCED_ID;
    else if (c == 0x7A) q = Q_BUS_ADDRESS;
    else if (c == 0x7F) q = Q_MFR_SPECIFIC;
  } else if (table == 0xFD) {
    switch (c) {
      case 0x08: q = Q_ACCESS_NO; break;        case 0x09: q = Q_MEDIUM; break;
      case 0x0A: q = Q_MANUFACTURER; break;     case 0x0B: q = Q_PARAMETER_SET; break;
      case 0x0C: q = Q_MODEL_VERSION; break;    case 0x0D: q = Q_HW_VERSION; break;
      case 0x0E: q = Q_FW_VERSION; break;       case 0x0F: q = Q_SW_VERSION; break;
      case 0x10: q = Q_CUSTOMER_LOCATION; break;case 0x11: q = Q_CUSTOMER; break;
      case 0x17: q = Q_ERROR_FLAGS; break;      case 0x1A: q = Q_DIGITAL_OUTPUT; break;
      case 0x1B: q = Q_DIGITAL_INPUT; break;    case 0x3A: q = Q_DIMENSIONLESS; break;
      case 0x60: q = Q_RESET_COUNTER; break;    case 0x61: q = Q_CUMULATION_COUNTER; break;
      case 0x74: q = Q_BATTERY_DAYS; break;
      default:
        if (c >= 0x40 && c <= 0x4F)      { q = Q_VOLTAGE; mult = p10((c & 0x0F) - 9); }
        else if (c >= 0x50 && c <= 0x5F) { q = Q_CURRENT; mult = p10((c & 0x0F) - 12); }
    }
  } else if (table == 0xFB) {
    int e = c & 1;
    if (c <= 0x01)                   { q = Q_ENERGY; mult = p10(e - 1) * 1000; }          // MWh -> kWh
    else if (c >= 0x08 && c <= 0x09) { q = Q_ENERGY; mult = p10(e - 1) * 1e9 / 3.6e6; }   // GJ -> kWh
    else if (c >= 0x10 && c <= 0x11) { q = Q_VOLUME; mult = p10(e + 2); }
    else if (c >= 0x18 && c <= 0x19) { q = Q_MASS; mult = p10(e + 2) * 1000; }            // t -> kg
    else if (c >= 0x1A && c <= 0x1B) { q = Q_HUMIDITY; mult = p10(e - 1); }
    else if (c >= 0x28 && c <= 0x29) { q = Q_POWER; mult = p10(e - 1) * 1e6; }            // MW -> W
    else if (c >= 0x30 && c <= 0x31) { q = Q_POWER; mult = p10(e - 1) * 1e9 / 3600; }     // GJ/h -> W
  }
}

void recordKey(const Record &r, char *out, size_t outMax) {
  if (r.key) { snprintf(out, outMax, "%s", r.key); return; }
  const QuantityInfo &qi = quantityInfo(r.quantity);
  int k;
  if (r.quantity == Q_UNKNOWN) {
    if (r.vifeTable) k = snprintf(out, outMax, "%s_%02x", r.vifeTable == 0xFB ? "fb" : "fd", r.vif);
    else             k = snprintf(out, outMax, "vif_%02x", r.vif);
  } else {
    k = snprintf(out, outMax, "%s", qi.key);
  }
  static const char *FN[4] = { "", "_max", "_min", "_err" };
  if (k < 0 || (size_t)k >= outMax) return;
  k += snprintf(out + k, outMax - k, "%s", FN[r.function & 3]);
  if (r.storage && (size_t)k < outMax) k += snprintf(out + k, outMax - k, "_s%u", (unsigned)r.storage);
  if (r.tariff && (size_t)k < outMax)  k += snprintf(out + k, outMax - k, "_t%u", (unsigned)r.tariff);
  if (r.subunit && (size_t)k < outMax) snprintf(out + k, outMax - k, "_u%u", (unsigned)r.subunit);
}

static bool bcdValue(const uint8_t *p, size_t n, double &v) {
  double acc = 0;
  bool neg = false;
  for (size_t i = n; i-- > 0;) {
    uint8_t hi = p[i] >> 4, lo = p[i] & 0x0F;
    if (i == n - 1 && hi == 0x0F) { neg = true; hi = 0; }
    if (hi > 9 || lo > 9) return false;
    acc = acc * 100 + hi * 10 + lo;
  }
  v = neg ? -acc : acc;
  return true;
}

static int64_t intValue(const uint8_t *p, size_t n) {
  uint64_t u = 0;
  for (size_t i = n; i-- > 0;) u = (u << 8) | p[i];
  if (n && n < 8 && (p[n - 1] & 0x80)) u |= ~(uint64_t)0 << (8 * n);   // sign-extend
  return (int64_t)u;
}

int parseRecords(const uint8_t *p, size_t n, RecordSink sink, void *ctx) {
  size_t i = 0;
  int count = 0;
  while (i < n) {
    uint8_t dif = p[i++];
    if (dif == 0x2F || dif == 0x7F) continue;                  // idle filler / global readout request
    Record r;
    r.dif = dif;
    if (dif == 0x0F || dif == 0x1F) {                          // manufacturer-specific data up to the end
      if (i < n) {
        r.quantity = Q_MFR_SPECIFIC; r.kind = VK_HEX;
        r.raw = p + i; r.rawLen = (uint8_t)((n - i) > 255 ? 255 : (n - i));
        sink(r, ctx); count++;
      }
      return count;
    }
    r.function = (dif >> 4) & 3;
    r.storage = (dif >> 6) & 1;
    uint8_t b = dif;
    for (int e = 0; b & 0x80; e++) {                           // DIFE chain
      if (i >= n || e >= 10) return -1;
      b = p[i++];
      r.storage |= (uint32_t)(b & 0x0F) << (1 + 4 * e);
      r.tariff |= (uint8_t)(((b >> 4) & 3) << (2 * e));
      r.subunit |= (uint16_t)(((b >> 6) & 1) << e);
    }
    if (i >= n) return -1;
    uint8_t vif = p[i++];
    uint8_t code = vif & 0x7F;
    b = vif;
    if (vif == 0xFB || vif == 0xFD) {                          // first-extension tables
      if (i >= n) return -1;
      r.vifeTable = vif;
      b = p[i++];
      code = b & 0x7F;
    } else if (code == 0x7C) {                                 // plain-text VIF: LEN + ASCII
      if (i >= n || i + 1 + p[i] > n) return -1;
      i += 1 + p[i];
    }
    r.vif = code;
    for (int e = 0; b & 0x80; e++) {                           // combinable VIFE chain (first one kept)
      if (i >= n || e >= 10) return -1;
      b = p[i++];
      if (e == 0) r.vife0 = b;
    }
    size_t len;
    uint8_t df = dif & 0x0F;
    static const uint8_t DLEN[16] = { 0, 1, 2, 3, 4, 4, 6, 8, 0, 1, 2, 3, 4, 0, 6, 0 };
    bool text = false, bcd = (df >= 0x09 && df <= 0x0C) || df == 0x0E, bin = false;
    if (df == 0x0D) {                                          // variable length: LVAR
      if (i >= n) return -1;
      uint8_t lv = p[i++];
      if (lv <= 0xBF)                    { len = lv; text = true; }
      else if (lv >= 0xC0 && lv <= 0xCF) { len = lv - 0xC0; bcd = true; }
      else if (lv >= 0xD0 && lv <= 0xDF) { len = lv - 0xD0; bcd = true; }
      else if (lv >= 0xE0 && lv <= 0xEF) { len = lv - 0xE0; bin = true; }
      else return -1;
    } else if (df == 0x0F) {
      return count;                                            // special function mid-stream: stop
    } else {
      len = DLEN[df];
    }
    if (i + len > n) return -1;
    r.raw = p + i;
    r.rawLen = (uint8_t)len;
    i += len;
    if (len == 0) continue;                                    // selection for readout, no data

    double mult;
    mapVif(r.vifeTable, code, r.quantity, mult);
    if (r.vifeTable == 0 && code == 0x7C) r.quantity = Q_UNKNOWN;

    if (r.quantity == Q_DATE || r.quantity == Q_DATETIME) {
      const uint8_t *d = r.raw;
      if (len == 2) {                                          // type G
        r.kind = VK_DATE;
        r.day = d[0] & 0x1F; r.month = d[1] & 0x0F;
        r.year = (uint16_t)(2000 + (((d[0] & 0xE0) >> 5) | ((d[1] & 0xF0) >> 1)));
      } else if (len == 4) {                                   // type F
        r.kind = VK_DATETIME;
        r.minute = d[0] & 0x3F; r.hour = d[1] & 0x1F; r.day = d[2] & 0x1F; r.month = d[3] & 0x0F;
        r.year = (uint16_t)(2000 + (((d[2] & 0xE0) >> 5) | ((d[3] & 0xF0) >> 1)));
      } else if (len == 6) {                                   // type I
        r.kind = VK_DATETIME;
        r.minute = d[1] & 0x3F; r.hour = d[2] & 0x1F; r.day = d[3] & 0x1F; r.month = d[4] & 0x0F;
        r.year = (uint16_t)(2000 + (((d[3] & 0xE0) >> 5) | ((d[4] & 0xF0) >> 1)));
      } else {
        r.kind = VK_HEX;
      }
      if (r.kind != VK_HEX && (r.month < 1 || r.month > 12 || r.day < 1)) r.kind = VK_HEX;  // "not set"
    } else if (r.quantity == Q_MFR_SPECIFIC) {
      r.kind = VK_HEX;
    } else if (text) {
      r.kind = VK_TEXT;
    } else if (bin && len > 8) {
      r.kind = VK_HEX;
    } else if (bcd) {
      double v;
      if (bcdValue(r.raw, len, v)) { r.kind = VK_NUMBER; r.value = v * mult; }
      else r.kind = VK_HEX;                                    // 0xFF.. = "no value" etc.
    } else if (df == 0x05) {
      float fv;
      memcpy(&fv, r.raw, 4);                                   // little-endian IEEE 754 on every target we build
      r.kind = VK_NUMBER; r.value = (double)fv * mult;
    } else {
      r.kind = VK_NUMBER; r.value = (double)intValue(r.raw, len) * mult;
    }
    sink(r, ctx);
    count++;
  }
  return count;
}

}   // namespace wmb

// ============================================================================ manufacturer-specific formats
namespace wmb {

static uint16_t packMfr(const char *c) {
  return (uint16_t)(((c[0] - 64) << 10) | ((c[1] - 64) << 5) | (c[2] - 64));
}
static uint32_t be32(const uint8_t *p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
static void emitNum(RecordSink sink, void *ctx, uint8_t q, double v, uint32_t storage = 0) {
  Record r;
  r.quantity = q; r.kind = VK_NUMBER; r.value = v; r.storage = storage;
  sink(r, ctx);
}
static void emitDate(RecordSink sink, void *ctx, uint16_t y, uint8_t m, uint8_t d, uint32_t storage) {
  if (m < 1 || m > 12 || d < 1 || d > 31) return;
  Record r;
  r.quantity = Q_DATE; r.kind = VK_DATE; r.year = y; r.month = m; r.day = d; r.storage = storage;
  sink(r, ctx);
}

// ---- Diehl IZAR / PRIOS: LFSR-scrambled with a key derived from two public default keys and the header
static bool isIzar(const Frame &f, const Header &h) {
  return (h.mfr == packMfr("SAP") || h.mfr == packMfr("DME") || h.mfr == packMfr("HYD")) &&
         h.ci >= 0xA0 && h.ci <= 0xA3 && !h.hasTpl && f.len >= 26;
}
static bool izarDescramble(const Frame &f, uint32_t key, uint8_t *out, size_t &outLen) {
  const uint8_t *d = f.data;   // d[0] = L field
  key ^= be32(d + 2);          // manufacturer + address[0..1]
  key ^= be32(d + 6);          // address[2..3] + version + type
  key ^= be32(d + 10);         // CI + first header bytes
  outLen = f.len - 15;
  for (size_t i = 0; i < outLen; i++) {
    for (int j = 0; j < 8; j++) {
      uint32_t bit = ((key >> 1) ^ (key >> 2) ^ (key >> 11) ^ (key >> 31)) & 1;
      key = (key << 1) | bit;
    }
    out[i] = (uint8_t)(d[i + 15] ^ (key & 0xFF));
    if (i == 0 && out[0] != 0x4B) return false;   // check byte
  }
  return true;
}
static int decodeIzar(const Frame &f, RecordSink sink, void *ctx) {
  static const uint32_t KEYS[2] = { 0x39BC8A10u ^ 0xE66D83F8u, 0x51728910u ^ 0xE66D83F8u };
  uint8_t dec[FRAME_MAX];
  size_t n = 0;
  bool ok = false;
  for (uint32_t k : KEYS) if ((ok = izarDescramble(f, k, dec, n))) break;
  if (!ok || n < 11) return -1;
  const uint8_t *d = f.data;
  uint32_t total = (uint32_t)dec[1] | ((uint32_t)dec[2] << 8) | ((uint32_t)dec[3] << 16) | ((uint32_t)dec[4] << 24);
  uint32_t last = (uint32_t)dec[5] | ((uint32_t)dec[6] << 8) | ((uint32_t)dec[7] << 16) | ((uint32_t)dec[8] << 24);
  uint16_t y = (uint16_t)(((dec[10] & 0xF0) >> 1) + ((dec[9] & 0xE0) >> 5));
  y = (uint16_t)(y > 80 ? 1900 + y : 2000 + y);
  emitNum(sink, ctx, Q_VOLUME, total / 1000.0);
  emitNum(sink, ctx, Q_VOLUME, last / 1000.0, 1);
  emitDate(sink, ctx, y, dec[10] & 0x0F, dec[9] & 0x1F, 1);
  emitNum(sink, ctx, Q_BATTERY_YEARS, (d[12] & 0x1F) / 2.0);
  emitNum(sink, ctx, Q_TX_PERIOD, (double)(1u << ((d[11] & 0x0F) + 2)));
  return 5;
}

// ---- Techem: [0] status [1..2] period-start date [3..4] previous-period value [5..6] current date
//      [7..8] current value (then room/radiator temperature x100 on FHKV data III v0x69)
static bool isTechem(const Header &h) {
  return h.mfr == packMfr("TCH") && (h.ci == 0xA0 || h.ci == 0xA2) && !h.hasTpl;
}
static bool techemWater(uint8_t type) { return type == 0x62 || type == 0x72 || type == 0x06 || type == 0x07 || type == 0x15 || type == 0x16; }
static int decodeTechem(const Frame &f, const Header &h, RecordSink sink, void *ctx) {
  const uint8_t *c = f.data + h.apOff;
  if (h.apLen < 9) return -1;
  uint16_t dPrev = (uint16_t)(c[1] | (c[2] << 8)), dCurr = (uint16_t)(c[5] | (c[6] << 8));
  uint8_t pDay = dPrev & 0x1F, pMon = (dPrev >> 5) & 0x0F;
  uint16_t pYear = (uint16_t)(2000 + ((dPrev >> 9) & 0x3F));
  uint8_t cDay = (dCurr >> 4) & 0x1F, cMon = (dCurr >> 9) & 0x0F;
  if (!cDay) cDay = 1;
  if (!cMon) cMon = 12;
  uint16_t cYear = (cMon < pMon || (cMon == pMon && cDay <= pDay)) ? pYear + 1 : pYear;
  uint16_t prev = (uint16_t)(c[3] | (c[4] << 8)), curr = (uint16_t)(c[7] | (c[8] << 8));
  int n = 0;
  if (h.type == 0x80) {                                   // heat cost allocator
    emitNum(sink, ctx, Q_HCA, curr); n++;
    emitNum(sink, ctx, Q_HCA, prev, 1); n++;
    if (h.ver == 0x69 && h.apLen >= 13) {
      emitNum(sink, ctx, Q_ROOM_TEMP, (uint16_t)(c[9] | (c[10] << 8)) / 100.0); n++;
      emitNum(sink, ctx, Q_RADIATOR_TEMP, (uint16_t)(c[11] | (c[12] << 8)) / 100.0); n++;
    }
  } else if (techemWater(h.type)) {                        // water: current = consumption since period start
    emitNum(sink, ctx, Q_VOLUME, (prev + curr) / 10.0); n++;
    emitNum(sink, ctx, Q_VOLUME, prev / 10.0, 1); n++;
  } else {
    return -2;
  }
  emitDate(sink, ctx, cYear, cMon, cDay, 0);
  emitDate(sink, ctx, pYear, pMon, pDay, 1);
  return n + 2;
}

const char *manufacturerDecoderName(const Frame &f, const Header &h) {
  if (isIzar(f, h)) return "izar";
  if (isTechem(h)) return h.type == 0x80 ? "techem_hca" : techemWater(h.type) ? "techem_water" : nullptr;
  return nullptr;
}

int decodeManufacturer(const Frame &f, const Header &h, RecordSink sink, void *ctx) {
  if (isIzar(f, h)) return decodeIzar(f, sink, ctx);
  if (isTechem(h)) return decodeTechem(f, h, sink, ctx);
  return -2;
}

}   // namespace wmb

// ============================================================================ interpretation layer
namespace wmb {

const char *statusText(uint8_t st) {
  if (st & 0x08) return "Dauerhafter Fehler";
  if (st & 0x04) return "Batterie schwach";
  if (st & 0x10) return "Vorübergehender Fehler";
  switch (st & 0x03) {
    case 1: return "Beschäftigt";
    case 2: return "Anwendungsfehler";
    case 3: return "Ungewöhnlicher Zustand";
  }
  return "OK";
}

struct Interp {
  RecordSink sink; void *ctx;
  const Header *h;
  bool smoke;
  bool haveCur[3] = {false, false, false}, haveDue[3] = {false, false, false};
  double cur[3] = {0, 0, 0}, due[3] = {0, 0, 0};
  int count = 0;
};
static int qslot(uint8_t q) { return q == Q_VOLUME ? 0 : q == Q_ENERGY ? 1 : q == Q_HCA ? 2 : -1; }

static void out(Interp &x, Record &r) {
  int k = qslot(r.quantity);
  if (k >= 0 && r.kind == VK_NUMBER && !r.function && !r.tariff && !r.subunit) {
    if (r.storage == 0 && !x.haveCur[k]) { x.haveCur[k] = true; x.cur[k] = r.value; }
    if (r.storage == 1 && !x.haveDue[k]) { x.haveDue[k] = true; x.due[k] = r.value; }
  }
  x.sink(r, x.ctx);
  x.count++;
}

static uint32_t bcdLE(const uint8_t *p, int n, bool &ok) {
  uint32_t v = 0;
  ok = true;
  for (int i = n - 1; i >= 0; i--) {
    if ((p[i] >> 4) > 9 || (p[i] & 0x0F) > 9) ok = false;
    v = v * 100 + (p[i] >> 4) * 10 + (p[i] & 0x0F);
  }
  return v;
}
static bool dateG(const uint8_t *b, Record &r) {
  if (b[0] == 0xFF && b[1] == 0xFF) return false;
  r.kind = VK_DATE;
  r.day = b[0] & 0x1F; r.month = b[1] & 0x0F;
  r.year = (uint16_t)(2000 + (((b[0] & 0xE0) >> 5) | ((b[1] & 0xF0) >> 1)));
  return r.month >= 1 && r.month <= 12 && r.day >= 1;
}

// Qundis walk-by blob (DIF 0D, VIF FF 5F, 53 bytes): [8] meter kind (C1 water / B0 HCA), [10..11] error date,
// [12..15] current, [16..17]+[18..21] year-end due date + reading, [22..23]+[24..27] month-end + reading.
static bool qundisBlob(Interp &x, const Record &src) {
  const uint8_t *b = src.raw;
  if (src.rawLen != 53 || b[0] != 0x00 || b[1] != 0x82 || b[3] != 0x00 || b[4] != 0x00) return false;   // [4]=0x35: encrypted
  bool water = b[8] == 0xC1, hca = b[8] == 0xB0;
  if (!water && !hca) return false;
  uint8_t q = water ? Q_VOLUME : Q_HCA;
  double scale = water ? 0.001 : 1;
  bool ok;
  Record r;
  r.quantity = q; r.kind = VK_NUMBER;
  r.value = bcdLE(b + 12, 4, ok) * scale; r.label = water ? "Zählerstand" : "Verbrauchseinheiten aktuell";
  if (ok) out(x, r);
  static const char *DL[2] = { "Stichtag (Jahr)", "Stichtag (Monat)" };
  static const char *VL[2][2] = { { "Stand am Jahresstichtag", "Stand am Monatsstichtag" },
                                  { "Einheiten am Jahresstichtag", "Einheiten am Monatsstichtag" } };
  for (int k = 0; k < 2; k++) {
    Record d;
    d.quantity = Q_DATE; d.storage = (uint32_t)(k + 1); d.label = DL[k];
    if (dateG(b + 16 + 6 * k, d)) out(x, d);
    Record v;
    v.quantity = q; v.kind = VK_NUMBER; v.storage = (uint32_t)(k + 1); v.label = VL[hca][k];
    v.value = bcdLE(b + 18 + 6 * k, 4, ok) * scale;
    if (ok) out(x, v);
  }
  Record e;
  e.quantity = Q_DATE; e.function = 3; e.label = "Fehlerdatum";
  if (dateG(b + 10, e)) out(x, e);
  return true;
}

// Ei Electronics smoke detector head record (DIF 84 40, VIF FF 2C): dust, battery, head status bits.
static bool eiHead(Interp &x, const Record &src) {
  if (src.rawLen != 4) return false;
  uint32_t v = (uint32_t)src.raw[0] | ((uint32_t)src.raw[1] << 8) | ((uint32_t)src.raw[2] << 16) | ((uint32_t)src.raw[3] << 24);
  Record d;
  d.quantity = Q_DUST; d.kind = VK_NUMBER; d.value = v & 0x1F; d.label = "Verschmutzung (0-31)";
  out(x, d);
  Record bv;
  bv.quantity = Q_VOLTAGE; bv.kind = VK_NUMBER; bv.value = 2.25 + ((v >> 8) & 0x0F) * 0.05; bv.label = "Batteriespannung";
  bv.key = "battery_voltage";
  out(x, bv);
  static const struct { uint32_t bit; const char *txt; } F[] = {
    { 0x2000000, "Melder vollständig abgedeckt" }, { 0x1000000, "Hindernis erkannt" },
    { 0x2000, "Rauchsensor defekt" }, { 0x1000, "Batterie schwach" }, { 0x20, "Signalgeber defekt" },
    { 0x40, "Manipulation während Demontage" }, { 0x80, "Lebensdauer erreicht" },
    { 0x8000, "Lebensdauer endet in 12 Monaten" }, { 0x4000, "Hinderniserkennung defekt" },
    { 0x40000, "Kommunikationsfehler Melderkopf" }, { 0x80000, "Funkstörung" }, { 0x20000, "Umgebung verändert" },
  };
  const char *txt = (v & 0x1F) >= 28 ? "Reinigung empfohlen" : "OK";
  for (auto &f : F) if (v & f.bit) { txt = f.txt; break; }
  Record s;
  s.quantity = Q_STATUS; s.kind = VK_STRING; s.text = txt; s.label = "Melderstatus"; s.key = "head_status";
  out(x, s);
  return true;
}

static void interpSink(const Record &rin, void *ctx) {
  Interp &x = *(Interp *)ctx;
  Record r = rin;
  if (r.quantity == Q_MFR_SPECIFIC && r.dif == 0x0D && r.vife0 == 0x5F &&
      (x.h->meterMfr() == 0x4493 /*QDS*/ || x.h->meterMfr() == 0x3265 /*LSE*/) && qundisBlob(x, r))
    return;
  if (r.quantity == Q_MFR_SPECIFIC && r.vife0 == 0x2C && x.h->meterMfr() == 0x1525 /*EIE*/ && eiHead(x, r)) return;
  if (x.smoke && r.tariff >= 1 && r.tariff <= 3 && (r.quantity == Q_DATE || r.quantity == Q_CUMULATION_COUNTER ||
                                                    r.quantity == Q_UNKNOWN)) {
    static const char *DATE[3] = { "Letzter Alarm", "Letzte Demontage", "Letzter Testknopf-Druck" };
    static const char *CNT[3] = { "Anzahl Alarme", "Anzahl Demontagen", "Anzahl Testknopf-Drücke" };
    static const char *KD[3] = { "last_alarm", "last_removal", "last_test" };
    static const char *KC[3] = { "alarms", "removals", "tests" };
    if (r.quantity == Q_DATE) { r.label = DATE[r.tariff - 1]; r.key = KD[r.tariff - 1]; }
    else if (r.quantity == Q_CUMULATION_COUNTER) { r.label = CNT[r.tariff - 1]; r.key = KC[r.tariff - 1]; }
  }
  if (x.smoke && r.quantity == Q_ERROR_FLAGS && r.kind == VK_NUMBER) r.label = "Fehlerflags (0 = OK)";
  out(x, r);
}

int interpret(const Frame &f, const Header &h, const uint8_t *ap, size_t apLen, RecordSink sink, void *ctx) {
  Interp x;
  x.sink = sink; x.ctx = ctx; x.h = &h; x.smoke = h.meterType() == 0x1A;
  int n = decodeManufacturer(f, h, interpSink, &x);
  if (n == -2) {
    if (h.ci >= 0xA0 && h.ci <= 0xB7) return 0;              // unknown manufacturer format: raw only
    n = parseRecords(ap, apLen, interpSink, &x);
  }
  if (h.hasTpl) {
    Record s;
    s.quantity = Q_STATUS; s.kind = VK_STRING; s.text = statusText(h.status); s.label = "Status";
    out(x, s);
  }
  static const char *SINCE[3] = { "Verbrauch seit Stichtag", "Energie seit Stichtag", "Einheiten seit Stichtag" };
  static const char *SK[3] = { "volume_since_due", "energy_since_due", "hca_since_due" };
  static const uint8_t SQ[3] = { Q_VOLUME, Q_ENERGY, Q_HCA };
  for (int k = 0; k < 2; k++)        // HCA "current" usually already counts from the due date -> no derived value
    if (x.haveCur[k] && x.haveDue[k] && x.cur[k] >= x.due[k]) {
      Record d;
      d.quantity = SQ[k]; d.kind = VK_NUMBER; d.value = x.cur[k] - x.due[k]; d.label = SINCE[k]; d.key = SK[k];
      d.storage = 0;
      out(x, d);
    }
  return n < 0 ? n : x.count;
}

}   // namespace wmb
