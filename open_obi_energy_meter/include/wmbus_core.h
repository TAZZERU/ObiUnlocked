// wmbus_core.h -- portable wireless M-Bus (EN 13757-4 / OMS) decoder, no Arduino dependencies.
//
// Covers what a passive 868.95 MHz receiver needs:
//   * radio bytes after the 0x543D sync word  ->  T1 (3-out-of-6) or C1 (frame A / B)
//   * data-link-layer CRC check + removal
//   * link / extended-link / authentication-and-fragmentation / transport layer headers
//   * AES-128 security mode 5 (CBC, IV from address + access number) and mode 7 (CBC, zero IV,
//     key derived with AES-CMAC from the message counter)
//   * generic DIF/VIF application-layer record parsing (values + units + storage/tariff)
//
// The AES block primitive is supplied by the platform (wmbus_aes_*.cpp on the device, OpenSSL in the host
// tests), so everything here can be unit-tested natively -- see tests/test_wmbus.py.
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace wmb {

// ---- platform hook --------------------------------------------------------------------------------------
// AES-128 single-block ECB encrypt / decrypt. Must be provided by exactly one translation unit per build.
void aes128EncryptBlock(const uint8_t key[16], const uint8_t in[16], uint8_t out[16]);
void aes128DecryptBlock(const uint8_t key[16], const uint8_t in[16], uint8_t out[16]);

// ---- physical / data link layer -------------------------------------------------------------------------
enum RadioMode : uint8_t { MODE_UNKNOWN = 0, MODE_T1 = 1, MODE_C1A = 2, MODE_C1B = 3, MODE_S1 = 4 };
const char *modeName(uint8_t mode);

uint16_t crc16(const uint8_t *p, size_t n);   // EN 13757: poly 0x3D65, init 0, final complement

// Decode a 3-out-of-6 bit stream (MSB first) into bytes. Stops at the first invalid symbol or when
// outMax bytes are written; returns the number of complete bytes.
size_t decode3of6(const uint8_t *in, size_t inLen, uint8_t *out, size_t outMax);

static const size_t FRAME_MAX = 256;
struct Frame {
  uint8_t mode = MODE_UNKNOWN;
  uint16_t len = 0;              // bytes in data[] (L field + payload, all block CRCs removed)
  uint8_t data[FRAME_MAX];       // data[0] = L field as sent, data[1] = C, data[2..3] = M, ...
};

enum RadioResult : int8_t {
  RR_OK = 0, RR_TOO_SHORT = -1, RR_BAD_3OF6 = -2, RR_BAD_CRC = -3, RR_BAD_LEN = -4,
};
// raw = bytes the radio delivered right after the 0x543D sync word (T1: 3-of-6 chips, C1: 0x54CD/0x543D +
// frame). Decides the mode, checks every block CRC and fills f.
RadioResult fromRadio(const uint8_t *raw, size_t rawLen, Frame &f);
// S1 (868.3 MHz, 32.768 kchip/s Manchester): raw = chips after the 0x7696 sync word.
RadioResult fromRadioS1(const uint8_t *raw, size_t rawLen, Frame &f);
// Manchester chips -> bytes (chip pair 10 = 1 / 01 = 0, or the inverse); stops at the first invalid pair.
size_t decodeManchester(const uint8_t *in, size_t inLen, uint8_t *out, size_t outMax, bool invert);

// ---- link / transport layer -----------------------------------------------------------------------------
struct Header {
  // link layer (always present)
  uint8_t  c = 0;
  uint16_t mfr = 0;            // manufacturer (3 letters packed, see mfrCode)
  uint32_t id = 0;             // 4-byte BCD id as little-endian integer -> print "%08X"
  uint8_t  ver = 0, type = 0;
  // the CI that starts the application data (after ELL / AFL / TPL were peeled off)
  uint8_t  ci = 0;
  // extended link layer (CI 0x8C / 0x8E)
  bool     hasEll = false;
  bool     ellEncrypted = false;  // CI 0x8D/0x8F: ELL AES-CTR -- not supported
  // authentication and fragmentation layer (CI 0x90)
  bool     hasAfl = false;
  bool     aflHasMcr = false;
  uint32_t aflMcr = 0;
  // transport layer (CI 0x7A short / 0x72 long)
  bool     hasTpl = false;
  bool     longTpl = false;
  uint16_t tplMfr = 0;         // long header: address of the actual meter (behind a radio converter)
  uint32_t tplId = 0;
  uint8_t  tplVer = 0, tplType = 0;
  uint8_t  acc = 0, status = 0;
  uint16_t cw = 0;             // configuration word
  uint8_t  secMode = 0;        // (cw >> 8) & 0x1F  -- 0 none, 5 AES-CBC, 7 AES-CBC + KDF, ...
  uint8_t  encBlocks = 0;      // number of encrypted 16-byte blocks (mode 5/7)
  // application data inside the frame
  uint16_t apOff = 0, apLen = 0;
  // the meter's own identity (long TPL wins over the link-layer address)
  uint16_t meterMfr() const { return longTpl ? tplMfr : mfr; }
  uint32_t meterId() const { return longTpl ? tplId : id; }
  uint8_t  meterVer() const { return longTpl ? tplVer : ver; }
  uint8_t  meterType() const { return longTpl ? tplType : type; }
};

bool parseHeader(const Frame &f, Header &h);   // false = frame too short / malformed

void mfrCode(uint16_t mfr, char out[4]);       // 0x2DAE -> "MIN" etc.
const char *deviceTypeName(uint8_t type);      // OMS device type ("heat cost allocator", ...)
const char *deviceTypeKey(uint8_t type);       // short machine-friendly key ("hca", "smoke", ...)

// ---- security -------------------------------------------------------------------------------------------
enum DecryptResult : int8_t {
  DR_PLAIN = 0,          // nothing was encrypted -> out = application data as is
  DR_OK = 1,             // decrypted and the 0x2F2F check bytes matched
  DR_NO_KEY = -1,        // encrypted, no key given
  DR_BAD_KEY = -2,       // decrypted but the 0x2F2F check failed -> wrong key
  DR_UNSUPPORTED = -3,   // security mode / ELL encryption not implemented
  DR_MALFORMED = -4,
};
const char *decryptResultName(int8_t r);
// Writes the cleartext application data (records) to out (outMax >= h.apLen). key may be nullptr.
DecryptResult decrypt(const Frame &f, const Header &h, const uint8_t *key, uint8_t *out, size_t outMax,
                      size_t &outLen);

void aesCmac(const uint8_t key[16], const uint8_t *msg, size_t len, uint8_t mac[16]);   // RFC 4493

// ---- application layer (DIF / VIF records) -------------------------------------------------------------
enum Quantity : uint8_t {
  Q_UNKNOWN = 0, Q_ENERGY, Q_VOLUME, Q_MASS, Q_ON_TIME, Q_OPERATING_TIME, Q_POWER, Q_VOLUME_FLOW,
  Q_MASS_FLOW, Q_FLOW_TEMP, Q_RETURN_TEMP, Q_TEMP_DIFF, Q_EXTERNAL_TEMP, Q_PRESSURE, Q_DATE, Q_DATETIME,
  Q_HCA, Q_AVG_DURATION, Q_ACTUALITY_DURATION, Q_FABRICATION_NO, Q_ENHANCED_ID, Q_BUS_ADDRESS,
  Q_ACCESS_NO, Q_MEDIUM, Q_MANUFACTURER, Q_PARAMETER_SET, Q_MODEL_VERSION, Q_HW_VERSION, Q_FW_VERSION,
  Q_SW_VERSION, Q_CUSTOMER_LOCATION, Q_CUSTOMER, Q_ERROR_FLAGS, Q_DIGITAL_OUTPUT, Q_DIGITAL_INPUT,
  Q_DIMENSIONLESS, Q_VOLTAGE, Q_CURRENT, Q_RESET_COUNTER, Q_CUMULATION_COUNTER, Q_BATTERY_DAYS,
  Q_HUMIDITY, Q_MFR_SPECIFIC, Q_ROOM_TEMP, Q_RADIATOR_TEMP, Q_BATTERY_YEARS, Q_TX_PERIOD, Q_STATUS, Q_DUST, Q_CONSUMPTION, Q_COUNT_
};
struct QuantityInfo {
  const char *key;           // JSON / MQTT key stem ("energy", "hca", ...)
  const char *unit;          // unit of Record::value ("kWh", "m³", "" ...)
  const char *haClass;       // Home Assistant device_class (nullptr = none)
  const char *haState;       // Home Assistant state_class (nullptr = none)
};
const QuantityInfo &quantityInfo(uint8_t q);

enum ValueKind : uint8_t { VK_NUMBER = 0, VK_DATE, VK_DATETIME, VK_HEX, VK_TEXT, VK_STRING };
struct Record {
  uint8_t  dif = 0, vif = 0, vifeTable = 0;   // vifeTable: 0 primary, 0xFB / 0xFD extension tables
  uint8_t  function = 0;                      // 0 instantaneous, 1 maximum, 2 minimum, 3 value during error
  uint32_t storage = 0;
  uint8_t  tariff = 0;
  uint16_t subunit = 0;
  uint8_t  quantity = Q_UNKNOWN;
  uint8_t  kind = VK_NUMBER;
  double   value = 0;                         // scaled to quantityInfo(quantity).unit
  uint16_t year = 0; uint8_t month = 0, day = 0, hour = 0, minute = 0;   // VK_DATE / VK_DATETIME
  const uint8_t *raw = nullptr; uint8_t rawLen = 0;                       // the data field bytes
  uint8_t  vife0 = 0;                         // first VIFE after the VIF (manufacturer-specific sub-codes)
  const char *label = nullptr;                // human-readable name override (else derived from quantity/storage)
  const char *text = nullptr;                 // VK_STRING value
  const char *key = nullptr;                  // key override (else recordKey())
};
// Stable key for a record, e.g. "energy", "hca_s1", "volume_max_s2_t1". Writes <= 31 chars + NUL.
void recordKey(const Record &r, char *out, size_t outMax);

typedef void (*RecordSink)(const Record &r, void *ctx);
// Walks the records of a cleartext application payload; returns the number of records found, or -1 if
// it stopped at a malformed record (the ones before it were still reported).
int parseRecords(const uint8_t *p, size_t n, RecordSink sink, void *ctx);

// Manufacturer-specific telegram formats that don't use DIF/VIF records:
//   * Diehl / Sappel IZAR water meters (SAP, DME, HYD; PRIOS scrambling with the public default keys)
//   * Techem heat cost allocators (FHKV data III) and water meters (MK Radio 3/4), unencrypted CI 0xA0/0xA2
// Returns the number of records reported, -1 if the format matched but the data didn't validate, or -2 if no
// manufacturer decoder applies (then use parseRecords on the application data instead).
int decodeManufacturer(const Frame &f, const Header &h, RecordSink sink, void *ctx);
const char *manufacturerDecoderName(const Frame &f, const Header &h);   // "izar", "techem_hca", ... or nullptr

// One-stop interpretation of a telegram's (decrypted) application data -- what the UI and MQTT use:
//   * manufacturer formats above, else DIF/VIF records
//   * Qundis walk-by blobs (0DFF5F) expanded into readings, Ei Electronics smoke detector head status
//   * smoke detector records labelled (alarm / removal / test button), OMS status byte as plain text
//   * derived "consumption since due date" from the current and due-date readings
int interpret(const Frame &f, const Header &h, const uint8_t *ap, size_t apLen, RecordSink sink, void *ctx);
const char *statusText(uint8_t st);   // OMS TPL status byte -> "OK" / "Batterie schwach" / ...

}   // namespace wmb
