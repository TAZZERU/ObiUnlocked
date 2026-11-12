// unl.cpp -- ObiUnlocked: device lists for wM-Bus / weather sensors / LoRaWAN, web API and MQTT + Home
// Assistant output. See include/unl.h for the design and threading rules.
#include "unl.h"

#include <PubSubClient.h>
#include <Preferences.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <time.h>

#include "lorawan_core.h"
#include "web_gzip.h"
#include "wmbus_core.h"
#include "wx_core.h"

#ifndef FW_VERSION
#  if __has_include("fw_version.h")
#    include "fw_version.h"
#  endif
#endif
#ifndef FW_VERSION
#  define FW_VERSION "0.0.0-dev"
#endif

// ============================================================================ limits
static const int WM_MAX = 80;            // wM-Bus devices listed (metadata ~90 B each)
static const int FC_N = 24;              // telegram cache slots (selected devices are pinned)
static const int FC_LEN = 160;           // cached telegram bytes (CRCs stripped)
static const int WX_MAX = 16;            // weather sensors
static const int LW_MAX = 24;            // LoRaWAN devices
static const int LW_PHY = 64;            // kept PHYPayload bytes (for decryption on demand)
static const int LOG_N = 24, LOG_HEX = 40;
static const int RXQ_LEN = 6;
static const int UNL_NAME_MAX = 25;
static const int MAIN_LEN = 28;

// ============================================================================ radio-side state
static portMUX_TYPE g_cfgMux = portMUX_INITIALIZER_UNLOCKED;
static UnlRadioCfg g_cfg;
static uint16_t g_availHours = 26;

struct RxItem { uint8_t listener, len, sf; int8_t snrQ4; int16_t rssi; uint32_t freqKHz, ms; uint8_t data[255]; };
static QueueHandle_t g_rxq = nullptr;

struct Stats {
  volatile uint32_t slotMs[1 + UL_COUNT] = {0};
  volatile uint32_t switches = 0, switchErrors = 0, defers = 0, queueDrops = 0, obiFrames = 0;
  volatile uint16_t obiLastOffset = 0, obiMaxOffset = 0;
  uint32_t rx[UL_COUNT] = {0}, ok[UL_COUNT] = {0}, crc = 0;
};
static Stats g_st;

void unl_radio_cfg(UnlRadioCfg &out) { portENTER_CRITICAL(&g_cfgMux); out = g_cfg; portEXIT_CRITICAL(&g_cfgMux); }

void unl_rx_push(uint8_t listener, const uint8_t *data, size_t len, int16_t rssi, int8_t snrQ4, uint8_t sf,
                 uint32_t freqKHz) {
  if (!g_rxq || !data || !len || listener >= UL_COUNT) return;
  static RxItem it;   // LoRa task only
  it.listener = listener; it.len = (uint8_t)(len > 255 ? 255 : len); it.sf = sf; it.snrQ4 = snrQ4; it.rssi = rssi;
  it.freqKHz = freqKHz; it.ms = millis();
  memcpy(it.data, data, it.len);
  if (xQueueSend(g_rxq, &it, 0) != pdTRUE) g_st.queueDrops++;
}
void unl_note_slot(uint8_t slot, uint32_t ms) { if (slot <= UL_COUNT) g_st.slotMs[slot] += ms; }
void unl_note_switch(bool ok) { g_st.switches++; if (!ok) g_st.switchErrors++; }
void unl_note_defer() { g_st.defers++; }
void unl_note_obi_rx(uint16_t ms) {
  g_st.obiFrames++; g_st.obiLastOffset = ms;
  if (ms > g_st.obiMaxOffset) g_st.obiMaxOffset = ms;
}

// ============================================================================ device tables (web task only)
struct DevBase {
  bool used = false, selected = false, drop = false, discDone = false;
  int8_t avail = -1;               // last published availability; -2 = remove from HA in the next MQTT pass
  int16_t rssi = 0;
  uint32_t lastMs = 0, count = 0, seq = 0, pubSeq = 0, discHash = 0;
  char name[UNL_NAME_MAX] = {0};
  char main[MAIN_LEN] = {0};       // headline value for the device list ("104.505 m³", "21.5 °C · 45 %")
};
struct WmDev : DevBase {
  uint16_t mfr = 0; uint32_t id = 0;
  uint8_t ver = 0, type = 0, mode = 0, ci = 0, secMode = 0, status = 0, cache = 0xFF;
  int8_t dec = 0;
  bool keySet = false; uint8_t key[16] = {0};
};
struct WxDev : DevBase { uint8_t proto = 0; uint32_t id = 0; wx::Reading r; };
struct LwDev : DevBase {
  bool join = false, keysSet = false, hasPort = false;
  uint8_t eui[8] = {0}; uint32_t devaddr = 0;
  uint8_t mtype = 0, fport = 0, plen = 0, sf = 0, phyLen = 0;
  int8_t snrQ4 = 0; uint16_t fcnt16 = 0, fcntHigh = 0; uint32_t freqKHz = 0;
  uint8_t nwk[16] = {0}, app[16] = {0};
  uint8_t phy[LW_PHY];
};
static WmDev g_wm[WM_MAX];
static WxDev g_wx[WX_MAX];
static LwDev g_lw[LW_MAX];

struct FrameSlot { uint8_t owner = 0xFF, mode = 0, len = 0; uint32_t used = 0; uint8_t data[FC_LEN]; };
static FrameSlot g_fc[FC_N];

struct LogEntry { uint32_t seq = 0, ms = 0; uint8_t listener = 0, mode = 0, len = 0, hexLen = 0; int8_t result = 0;
                  int16_t rssi = 0; char who[20] = {0}; uint8_t hex[LOG_HEX]; };
static LogEntry g_log[LOG_N];
static int g_logHead = 0;
static uint32_t g_logSeq = 0;
static bool g_mqttDirty = false;

static void logAdd(uint8_t l, uint8_t mode, int8_t res, int16_t rssi, const char *who, const uint8_t *p, size_t n) {
  LogEntry &e = g_log[g_logHead];
  g_logHead = (g_logHead + 1) % LOG_N;
  e.seq = ++g_logSeq; e.ms = millis(); e.listener = l; e.mode = mode; e.result = res; e.rssi = rssi;
  e.len = (uint8_t)(n > 255 ? 255 : n);
  strlcpy(e.who, who ? who : "", sizeof e.who);
  e.hexLen = (uint8_t)(n > LOG_HEX ? LOG_HEX : n);
  memcpy(e.hex, p, e.hexLen);
}

// ============================================================================ helpers
static String hexStr(const uint8_t *p, size_t n) {
  static const char *H = "0123456789ABCDEF";
  String s; s.reserve(n * 2);
  for (size_t i = 0; i < n; i++) { s += H[p[i] >> 4]; s += H[p[i] & 15]; }
  return s;
}
static bool parseHex(const String &in, uint8_t *out, size_t n) {
  String t = in;
  t.replace(" ", ""); t.replace(":", ""); t.replace("-", "");
  if (t.length() != n * 2) return false;
  for (size_t i = 0; i < n; i++) {
    char b[3] = { t[2 * i], t[2 * i + 1], 0 }, *end;
    out[i] = (uint8_t)strtoul(b, &end, 16);
    if (*end) return false;
  }
  return true;
}
static String jstr(const char *s) {
  String o = "\"";
  for (; *s; s++) {
    uint8_t c = (uint8_t)*s;
    if (c == '"' || c == '\\') { o += '\\'; o += (char)c; }
    else if (c < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); o += b; }
    else o += (char)c;
  }
  return o + "\"";
}
static String num(double v, int prec = 10) {
  if (isnan(v) || isinf(v)) return "null";
  char b[24];
  snprintf(b, sizeof b, "%.*g", prec, v);
  return b;
}
static bool timeOk() { return time(nullptr) > 1700000000; }
static String isoAgo(uint32_t ms) {
  if (!timeOk() || !ms) return "";
  time_t t = time(nullptr) - (time_t)((millis() - ms) / 1000);
  struct tm g; gmtime_r(&t, &g);
  char b[24]; strftime(b, sizeof b, "%Y-%m-%dT%H:%M:%SZ", &g);
  return b;
}
static void wmKey(uint16_t mfr, uint32_t id, char out[13]) { char m[4]; wmb::mfrCode(mfr, m); snprintf(out, 13, "%s%08X", m, (unsigned)id); }
static void wxKey(const WxDev &d, char out[24]) {
  static const char *P[] = { "wx", "lacrosse", "bresser5", "bresser6", "bresser7" };
  snprintf(out, 24, "%s-%X", P[d.proto < 5 ? d.proto : 0], (unsigned)d.id);
}
static void lwKey(const LwDev &d, char out[18]) {
  if (!d.join) { snprintf(out, 18, "%08X", (unsigned)d.devaddr); return; }
  char *o = out; *o++ = 'J';
  for (int i = 7; i >= 0; i--) o += sprintf(o, "%02X", d.eui[i]);
}
static const char *mfrName(uint16_t m) {
  char c[4]; wmb::mfrCode(m, c);
  static const char *T[][2] = { {"MIN", "Minol"}, {"ZRI", "Zenner"}, {"QDS", "Qundis"}, {"TCH", "Techem"}, {"IST", "ista"},
    {"BRA", "Brunata"}, {"KAM", "Kamstrup"}, {"DME", "Diehl Metering"}, {"SAP", "Sappel (Diehl)"}, {"HYD", "Diehl Hydrometer"},
    {"EIE", "Ei Electronics"}, {"ELS", "Elster"}, {"SON", "Sontex"}, {"APA", "Apator"}, {"ITW", "Itron"} };
  for (auto &t : T) if (!strcmp(t[0], c)) return t[1];
  return nullptr;
}
static const char *typeDe(uint8_t t) {
  switch (t) {
    case 0x02: return "Stromzähler";          case 0x03: return "Gaszähler";
    case 0x04: case 0x0C: return "Wärmezähler"; case 0x0D: return "Wärme-/Kältezähler";
    case 0x0A: case 0x0B: return "Kältezähler"; case 0x06: case 0x15: return "Warmwasserzähler";
    case 0x07: return "Wasserzähler";         case 0x16: return "Kaltwasserzähler";
    case 0x08: case 0x80: return "Heizkostenverteiler"; case 0x1A: return "Rauchmelder";
    case 0x1B: return "Raumsensor";           case 0x1C: return "Gasmelder";
    case 0x62: return "Warmwasserzähler";     case 0x72: return "Kaltwasserzähler";
    case 0x31: case 0x32: case 0x33: case 0x36: case 0x37: return "Funkwandler / Repeater";
    default: return wmb::deviceTypeName(t);
  }
}
static const char *lwMtype(uint8_t m) {
  static const char *N[8] = { "Join-Request", "Join-Accept", "Uplink", "Downlink", "Uplink (bestätigt)",
                              "Downlink (bestätigt)", "Rejoin", "Proprietär" };
  return N[m & 7];
}

// ============================================================================ values (one model for UI + MQTT)
struct Val {
  const char *key; String label; String json;   // json = value as JSON literal
  const char *unit, *dc, *sc; bool diag, bin;
  double num; bool isNum;
};
typedef void (*ValFn)(const Val &v, void *ctx);

static String recordLabel(const wmb::Record &r) {
  if (r.label) return r.label;
  static const char *DE[wmb::Q_COUNT_] = {
    "Wert", "Energie", "Volumen", "Masse", "Einschaltdauer", "Betriebsdauer", "Leistung", "Durchfluss", "Massenstrom",
    "Vorlauftemperatur", "Rücklauftemperatur", "Temperaturdifferenz", "Außentemperatur", "Druck", "Datum",
    "Uhrzeit des Zählers", "Verbrauchseinheiten", "Mittelungsdauer", "Aktualität", "Fabrikationsnummer",
    "Erweiterte ID", "Busadresse", "Zugriffsnummer", "Medium", "Hersteller", "Parametersatz", "Modell/Version",
    "HW-Version", "FW-Version", "SW-Version", "Kundenstandort", "Kunde", "Fehlerflags", "Digitalausgang",
    "Digitaleingang", "Dimensionslos", "Spannung", "Strom", "Reset-Zähler", "Kumulationszähler",
    "Batterie-Restlaufzeit", "Luftfeuchte", "Herstellerdaten", "Raumtemperatur", "Heizkörpertemperatur",
    "Batterie-Restlaufzeit", "Sendeintervall", "Status", "Verschmutzung", "Verbrauch" };
  bool cur = !r.storage && !r.function && !r.tariff && !r.subunit;
  String s;
  if (cur && (r.quantity == wmb::Q_VOLUME || r.quantity == wmb::Q_ENERGY)) s = "Zählerstand";
  else if (cur && r.quantity == wmb::Q_HCA) s = "Verbrauchseinheiten aktuell";
  else if (r.quantity == wmb::Q_DATE && r.storage == 1 && !r.function) return "Stichtag";
  else s = r.quantity < wmb::Q_COUNT_ ? DE[r.quantity] : "Wert";
  if (r.quantity == wmb::Q_UNKNOWN) {
    char b[12];
    snprintf(b, sizeof b, " %s%02X", r.vifeTable == 0xFD ? "FD" : r.vifeTable == 0xFB ? "FB" : "VIF ", r.vif);
    s += b;
  }
  static const char *FN[4] = { "", " max", " min", " (bei Fehler)" };
  s += FN[r.function & 3];
  if (r.storage == 1) s += " am Stichtag";
  else if (r.storage) s += " [Speicher " + String(r.storage) + "]";
  if (r.tariff) s += " Tarif " + String(r.tariff);
  if (r.subunit) s += " Einheit " + String(r.subunit);
  return s;
}

static String recordJson(const wmb::Record &r) {
  char b[24];
  switch (r.kind) {
    case wmb::VK_NUMBER: return num(r.value);
    case wmb::VK_DATE: snprintf(b, sizeof b, "\"%04u-%02u-%02u\"", r.year, r.month, r.day); return b;
    case wmb::VK_DATETIME:
      snprintf(b, sizeof b, "\"%04u-%02u-%02uT%02u:%02u\"", r.year, r.month, r.day, r.hour, r.minute); return b;
    case wmb::VK_STRING: return jstr(r.text ? r.text : "");
    case wmb::VK_TEXT: {
      char t[64];
      size_t n = r.rawLen < sizeof t - 1 ? r.rawLen : sizeof t - 1;
      for (size_t i = 0; i < n; i++) { char c = (char)r.raw[r.rawLen - 1 - i]; t[i] = (c >= 32 && c < 127) ? c : '?'; }
      t[n] = 0;
      return jstr(t);
    }
    default: return "\"" + hexStr(r.raw, r.rawLen) + "\"";
  }
}

static const char *volumeClassFor(uint8_t type) {
  switch (type) {
    case 0x03: return "gas";
    case 0x04: case 0x06: case 0x07: case 0x0C: case 0x0D: case 0x15: case 0x16: case 0x17: case 0x62: case 0x72: return "water";
    default: return "volume";
  }
}

struct RecAdapt { ValFn fn; void *ctx; uint8_t type; int n; char keys[40][32]; };
static void recToVal(const wmb::Record &r, void *c) {
  RecAdapt &a = *(RecAdapt *)c;
  if (a.n >= 40) return;
  char key[32];
  wmb::recordKey(r, key, sizeof key);
  int dup = 1;
  for (int i = 0; i < a.n; i++) {
    size_t kl = strlen(key);
    if (!strncmp(a.keys[i], key, kl) && (!a.keys[i][kl] || (a.keys[i][kl] == '_' && a.keys[i][kl + 1] >= '2' &&
                                                           a.keys[i][kl + 1] <= '9' && !a.keys[i][kl + 2]))) dup++;
  }
  if (dup > 1) { size_t kl = strlen(key); if (kl < 29) snprintf(key + kl, 32 - kl, "_%d", dup > 9 ? 9 : dup); }
  strlcpy(a.keys[a.n], key, 32);
  const wmb::QuantityInfo &qi = wmb::quantityInfo(r.quantity);
  Val v;
  v.key = a.keys[a.n++];
  v.label = recordLabel(r);
  v.json = recordJson(r);
  v.isNum = r.kind == wmb::VK_NUMBER; v.num = r.value;
  v.unit = v.isNum ? qi.unit : "";
  v.dc = qi.haClass; v.sc = v.isNum ? qi.haState : nullptr;
  if (r.quantity == wmb::Q_VOLUME) v.dc = volumeClassFor(a.type);
  if (r.kind == wmb::VK_DATETIME || r.kind == wmb::VK_STRING || r.kind == wmb::VK_TEXT || r.kind == wmb::VK_HEX) v.dc = nullptr;
  if (v.isNum && (r.storage || r.function)) {   // historic / max / min values are no running totals
    bool meterClass = v.dc && (!strcmp(v.dc, "water") || !strcmp(v.dc, "gas") || !strcmp(v.dc, "energy") || !strcmp(v.dc, "volume"));
    v.sc = meterClass ? "total" : "measurement";
  }
  v.diag = (r.quantity >= wmb::Q_FABRICATION_NO && r.quantity <= wmb::Q_CUSTOMER) || r.quantity == wmb::Q_TX_PERIOD ||
           r.quantity == wmb::Q_DATETIME || r.kind == wmb::VK_HEX;
  v.bin = false;
  a.fn(v, a.ctx);
}

// Rebuild a device's latest telegram from the cache, decrypt + interpret it. -100 = telegram not cached.
static int wmValues(const WmDev &d, ValFn fn, void *ctx, String *frameHex = nullptr, String *plainHex = nullptr) {
  if (d.cache >= FC_N || g_fc[d.cache].owner != (uint8_t)(&d - g_wm)) return -100;
  const FrameSlot &s = g_fc[d.cache];
  static wmb::Frame f;
  f.mode = s.mode; f.len = s.len;
  memcpy(f.data, s.data, s.len);
  if (frameHex) *frameHex = hexStr(s.data, s.len);
  wmb::Header h;
  if (!wmb::parseHeader(f, h)) return wmb::DR_MALFORMED;
  static uint8_t ap[wmb::FRAME_MAX];
  size_t apLen = 0;
  int dr = wmb::decrypt(f, h, d.keySet ? d.key : nullptr, ap, sizeof ap, apLen);
  if (plainHex && dr == wmb::DR_OK) *plainHex = hexStr(ap, apLen);
  if (dr == wmb::DR_OK || dr == wmb::DR_PLAIN) {
    static RecAdapt a;
    a.fn = fn; a.ctx = ctx; a.type = h.meterType(); a.n = 0;
    wmb::interpret(f, h, ap, apLen, recToVal, &a);
  }
  return dr;
}

static void wxValues(const WxDev &d, ValFn fn, void *ctx) {
  const wx::Reading &r = d.r;
  auto emit = [&](const char *k, const char *l, double v, const char *u, const char *dc, const char *sc, int prec) {
    Val x; x.key = k; x.label = l; x.json = num(v, prec); x.unit = u; x.dc = dc; x.sc = sc; x.diag = false; x.bin = false;
    x.num = v; x.isNum = true; fn(x, ctx);
  };
  if (r.hasTemp) emit("temperature", "Temperatur", r.tempC, "°C", "temperature", "measurement", 4);
  if (r.hasHum) emit("humidity", "Luftfeuchte", r.hum, "%", "humidity", "measurement", 3);
  if (r.hasWind) {
    emit("wind_speed", "Wind (Mittel)", r.windAvg, "m/s", "wind_speed", "measurement", 4);
    emit("wind_gust", "Windböe", r.windGust, "m/s", "wind_speed", "measurement", 4);
    emit("wind_direction", "Windrichtung", r.windDir, "°", nullptr, "measurement", 4);
  }
  if (r.hasRain) emit("rain", "Regen gesamt", r.rainMm, "mm", "precipitation", "total_increasing", 6);
  if (r.hasUv) emit("uv", "UV-Index", r.uv, "", nullptr, "measurement", 3);
  if (r.hasLight) emit("illuminance", "Helligkeit", r.lux, "lx", "illuminance", "measurement", 7);
  if (r.hasBatt) {
    Val b; b.key = "battery_low"; b.label = "Batterie schwach"; b.json = r.battLow ? "true" : "false";
    b.unit = ""; b.dc = "battery"; b.sc = nullptr; b.diag = true; b.bin = true; b.isNum = false; b.num = 0;
    fn(b, ctx);
  }
}

struct LppCtx { ValFn fn; void *ctx; int n; char keys[16][24]; };
static void lppToVal(const char *key, const char *label, double value, const char *unit, const char *dc, void *c) {
  LppCtx &l = *(LppCtx *)c;
  if (l.n >= 16) return;
  strlcpy(l.keys[l.n], key, 24);
  Val v; v.key = l.keys[l.n++]; v.label = label; v.json = num(value, 8); v.unit = unit; v.dc = dc;
  v.sc = "measurement"; v.diag = false; v.bin = false; v.isNum = true; v.num = value;
  if (dc && !strcmp(dc, "energy")) v.sc = "total_increasing";
  l.fn(v, l.ctx);
}
// Returns: payload length (>=0) if decrypted, -1 wrong keys, -2 no keys / nothing to decrypt
static int lwValues(LwDev &d, ValFn fn, void *ctx, String *plainHex = nullptr) {
  if (!d.keysSet || !d.phyLen || d.join) return -2;
  lw::Uplink u;
  if (!lw::parse(d.phy, d.phyLen, u)) return -2;
  uint8_t out[LW_PHY];
  uint32_t f32 = 0;
  int n = lw::decodeUplink(d.phy, d.phyLen, u, d.nwk, d.app, d.fcntHigh, out, sizeof out, f32);
  if (n < 0) return -1;
  d.fcntHigh = (uint16_t)(f32 >> 16);
  if (plainHex) *plainHex = hexStr(out, n);
  static LppCtx l;
  l.fn = fn; l.ctx = ctx; l.n = 0;
  if (n > 0 && lw::parseCayenne(out, n, lppToVal, &l) < 0) {
    Val v; v.key = "payload"; v.label = "Nutzdaten (hex)"; v.json = "\"" + hexStr(out, n) + "\"";
    v.unit = ""; v.dc = nullptr; v.sc = nullptr; v.diag = false; v.bin = false; v.isNum = false; v.num = 0;
    fn(v, ctx);
  }
  return n;
}

// headline value for the device list: meter reading / temperature / status
struct MainPick { String s; int prio; };
static void pickMain(const Val &v, void *c) {
  MainPick &m = *(MainPick *)c;
  int p = 9;
  if (!strcmp(v.key, "volume") || !strcmp(v.key, "energy") || !strcmp(v.key, "hca") || !strcmp(v.key, "temperature")) p = 0;
  else if (!strcmp(v.key, "head_status")) p = 1;
  else if (!strcmp(v.key, "humidity")) p = 2;
  else if (!strcmp(v.key, "status")) p = 3;
  else if (v.isNum && !v.diag) p = 5;
  if (p == 2 && m.prio == 0) { m.s += " · " + num(v.num, 3) + " %"; m.prio = -1; return; }   // "21.5 °C · 45 %"
  if (p >= m.prio || m.prio < 0) return;
  m.prio = p;
  if (v.isNum) m.s = num(v.num, 7) + (v.unit[0] ? String(" ") + v.unit : String(""));
  else { m.s = v.json; m.s.replace("\"", ""); }
}
static void setMain(DevBase &d, const MainPick &m) { strlcpy(d.main, m.s.c_str(), sizeof d.main); }

// ============================================================================ persistence (NVS ns "unl")
struct __attribute__((packed)) WmP { uint16_t mfr; uint32_t id; uint8_t flags; uint8_t key[16]; char name[UNL_NAME_MAX]; };
struct __attribute__((packed)) WxP { uint8_t proto; uint32_t id; uint8_t flags; char name[UNL_NAME_MAX]; };
struct __attribute__((packed)) LwP { uint8_t join; uint8_t eui[8]; uint32_t devaddr; uint8_t flags; uint8_t nwk[16], app[16]; char name[UNL_NAME_MAX]; };

static void saveCfg() {
  UnlRadioCfg c; unl_radio_cfg(c);
  Preferences p; p.begin("unl", false);
  p.putBool("obi", c.obiOn); p.putUChar("lst", c.listen); p.putUShort("win", c.obiWindowMs);
  p.putUChar("lwch", c.lwChMask); p.putUChar("lwsfm", c.lwSfMask); p.putUShort("avh", g_availHours);
  p.end();
}

template <typename P, typename D, int N, typename Keep, typename Fill>
static void saveTable(Preferences &p, const char *k, D (&tab)[N], Keep keep, Fill fill) {
  P *buf = (P *)malloc(sizeof(P) * N);   // temporary: no static RAM for something written a few times a day
  if (!buf) return;
  int n = 0;
  for (auto &d : tab) if (d.used && keep(d)) { memset(&buf[n], 0, sizeof(P)); fill(d, buf[n++]); }
  if (n) p.putBytes(k, buf, n * sizeof(P)); else p.remove(k);
  free(buf);
}

static void saveDevices() {
  Preferences p; p.begin("unl", false);
  saveTable<WmP>(p, "wm", g_wm, [](const WmDev &d) { return d.selected || d.keySet || d.name[0]; },
                 [](const WmDev &d, WmP &x) { x.mfr = d.mfr; x.id = d.id; x.flags = (d.selected ? 1 : 0) | (d.keySet ? 2 : 0);
                                              memcpy(x.key, d.key, 16); strlcpy(x.name, d.name, sizeof x.name); });
  saveTable<WxP>(p, "wx", g_wx, [](const WxDev &d) { return d.selected || d.name[0]; },
                 [](const WxDev &d, WxP &x) { x.proto = d.proto; x.id = d.id; x.flags = d.selected ? 1 : 0; strlcpy(x.name, d.name, sizeof x.name); });
  saveTable<LwP>(p, "lw", g_lw, [](const LwDev &d) { return d.selected || d.keysSet || d.name[0]; },
                 [](const LwDev &d, LwP &x) { x.join = d.join; memcpy(x.eui, d.eui, 8); x.devaddr = d.devaddr;
                                              x.flags = (d.selected ? 1 : 0) | (d.keysSet ? 2 : 0);
                                              memcpy(x.nwk, d.nwk, 16); memcpy(x.app, d.app, 16); strlcpy(x.name, d.name, sizeof x.name); });
  p.end();
}

template <typename P, typename D, int N, typename Load>
static size_t loadTable(Preferences &p, const char *k, D (&tab)[N], Load load) {
  size_t len = p.getBytesLength(k);
  if (!len || len % sizeof(P)) return 0;   // missing, or written by an older layout -> start clean
  P *buf = (P *)malloc(len);
  if (!buf) return 0;
  size_t n = p.getBytes(k, buf, len) / sizeof(P);
  for (size_t i = 0; i < n && i < (size_t)N; i++) { tab[i] = D(); tab[i].used = true; load(buf[i], tab[i]); }
  free(buf);
  return n;
}

void unl_setup() {
  if (!g_rxq) g_rxq = xQueueCreate(RXQ_LEN, sizeof(RxItem));
  Preferences p; p.begin("unl", true);
  UnlRadioCfg c;
  c.obiOn = p.getBool("obi", true);
  c.listen = p.getUChar("lst", c.listen);
  c.obiWindowMs = p.getUShort("win", 300);
  c.lwChMask = p.getUChar("lwch", 0x07);
  c.lwSfMask = p.getUChar("lwsfm", 0x01);
  g_availHours = p.getUShort("avh", 26);
  if (c.obiWindowMs < 100 || c.obiWindowMs > 900) c.obiWindowMs = 300;
  if (!c.lwChMask) c.lwChMask = 0x07;
  if (!(c.lwSfMask & 0x3F)) c.lwSfMask = 0x01;
  if (!g_availHours) g_availHours = 26;
  size_t nw = loadTable<WmP>(p, "wm", g_wm, [](const WmP &x, WmDev &d) {
    d.mfr = x.mfr; d.id = x.id; d.selected = x.flags & 1; d.keySet = x.flags & 2;
    memcpy(d.key, x.key, 16); strlcpy(d.name, x.name, sizeof d.name); });
  size_t nx = loadTable<WxP>(p, "wx", g_wx, [](const WxP &x, WxDev &d) {
    d.proto = x.proto; d.id = x.id; d.selected = x.flags & 1; strlcpy(d.name, x.name, sizeof d.name); });
  size_t nl = loadTable<LwP>(p, "lw", g_lw, [](const LwP &x, LwDev &d) {
    d.join = x.join; memcpy(d.eui, x.eui, 8); d.devaddr = x.devaddr; d.selected = x.flags & 1; d.keysSet = x.flags & 2;
    memcpy(d.nwk, x.nwk, 16); memcpy(d.app, x.app, 16); strlcpy(d.name, x.name, sizeof d.name); });
  p.end();
  portENTER_CRITICAL(&g_cfgMux); g_cfg = c; portEXIT_CRITICAL(&g_cfgMux);
  Serial.printf("[unl] OBI %s, listeners 0x%02X, window %u ms, known devices %u/%u/%u\n", c.obiOn ? "on" : "off",
                c.listen, c.obiWindowMs, (unsigned)nw, (unsigned)nx, (unsigned)nl);
}

void unl_factory_reset() { Preferences p; p.begin("unl", false); p.clear(); p.end(); }

// ============================================================================ frame processing
template <typename D, int N>
static D *alloc(D (&tab)[N]) {   // free slot, else the longest-unheard device nobody cares about
  D *slot = nullptr;
  for (auto &d : tab) if (!d.used) { slot = &d; break; }
  if (!slot)
    for (auto &d : tab)
      if (!d.selected && !d.name[0] && (!slot || (int32_t)(d.lastMs - slot->lastMs) < 0)) slot = &d;
  if (slot) { *slot = D(); slot->used = true; }
  return slot;
}

static uint8_t cacheFrame(WmDev &d, const wmb::Frame &f) {
  if (f.len > FC_LEN) return 0xFF;
  uint8_t idx = (uint8_t)(&d - g_wm);
  int pick = -1;
  if (d.cache < FC_N && g_fc[d.cache].owner == idx) pick = d.cache;
  for (int i = 0; pick < 0 && i < FC_N; i++) if (g_fc[i].owner == 0xFF) pick = i;
  if (pick < 0)                                    // evict the least recently used unpinned slot
    for (int i = 0; i < FC_N; i++) {
      uint8_t o = g_fc[i].owner;
      if (o < WM_MAX && g_wm[o].selected) continue;
      if (pick < 0 || (int32_t)(g_fc[i].used - g_fc[pick].used) < 0) pick = i;
    }
  if (pick < 0) return 0xFF;                       // every slot pinned by a selected device
  FrameSlot &s = g_fc[pick];
  if (s.owner < WM_MAX && s.owner != idx) g_wm[s.owner].cache = 0xFF;
  s.owner = idx; s.mode = f.mode; s.used = millis(); s.len = (uint8_t)f.len;
  memcpy(s.data, f.data, s.len);
  return (uint8_t)pick;
}

static void processWmbus(const RxItem &it) {
  static wmb::Frame f;
  int rr = it.listener == UL_WMB_S1 ? wmb::fromRadioS1(it.data, it.len, f) : wmb::fromRadio(it.data, it.len, f);
  if (rr != wmb::RR_OK) {
    if (rr == wmb::RR_BAD_CRC) { g_st.crc++; logAdd(it.listener, f.mode, (int8_t)rr, it.rssi, "CRC-Fehler", it.data, it.len); }
    return;
  }
  wmb::Header h;
  if (!wmb::parseHeader(f, h)) return;
  g_st.ok[it.listener]++;
  char key[13];
  wmKey(h.meterMfr(), h.meterId(), key);
  logAdd(it.listener, f.mode, 0, it.rssi, key, f.data, f.len);
  WmDev *d = nullptr;
  for (auto &x : g_wm) if (x.used && x.mfr == h.meterMfr() && x.id == h.meterId()) { d = &x; break; }
  if (!d && (d = alloc(g_wm))) { d->mfr = h.meterMfr(); d->id = h.meterId(); }
  if (!d) return;
  d->ver = h.meterVer(); d->type = h.meterType(); d->mode = f.mode; d->ci = h.ci; d->secMode = h.secMode;
  d->status = h.status; d->rssi = it.rssi; d->lastMs = it.ms; d->count++; d->seq++;
  d->cache = cacheFrame(*d, f);
  MainPick m{ "", 99 };
  int dr = wmValues(*d, pickMain, &m);
  d->dec = (int8_t)(dr == -100 ? wmb::DR_MALFORMED : dr);
  if (m.prio == 99 && dr == wmb::DR_NO_KEY) m.s = "verschlüsselt";
  setMain(*d, m);
}

static void processWeather(const RxItem &it) {
  wx::Reading r;
  bool ok = it.listener == UL_WX_LACR ? wx::decodeLaCrosse(it.data, it.len, r) : wx::decodeBresser(it.data, it.len, r);
  if (!ok) return;
  g_st.ok[it.listener]++;
  WxDev *d = nullptr;
  for (auto &x : g_wx) if (x.used && x.proto == r.proto && x.id == r.id) { d = &x; break; }
  if (!d && (d = alloc(g_wx))) { d->proto = r.proto; d->id = r.id; }
  char key[24] = "?";
  if (d) wxKey(*d, key);
  logAdd(it.listener, r.proto, 0, it.rssi, key, it.data, it.len);
  if (!d) return;
  d->r = r; d->rssi = it.rssi; d->lastMs = it.ms; d->count++; d->seq++;
  MainPick m{ "", 99 };
  wxValues(*d, pickMain, &m);
  setMain(*d, m);
}

static void processLorawan(const RxItem &it) {
  lw::Uplink u;
  if (!lw::parse(it.data, it.len, u)) { logAdd(UL_LORAWAN, it.sf, -1, it.rssi, lwMtype(it.data[0] >> 5), it.data, it.len); return; }
  g_st.ok[UL_LORAWAN]++;
  bool join = u.mtype == 0;
  LwDev *d = nullptr;
  for (auto &x : g_lw) if (x.used && x.join == join && (join ? !memcmp(x.eui, u.devEui, 8) : x.devaddr == u.devaddr)) { d = &x; break; }
  if (!d && (d = alloc(g_lw))) { d->join = join; memcpy(d->eui, u.devEui, 8); d->devaddr = u.devaddr; }
  char key[18] = "?";
  if (d) lwKey(*d, key);
  logAdd(UL_LORAWAN, it.sf, 0, it.rssi, key, it.data, it.len);
  if (!d) return;
  if (u.fcnt16 < d->fcnt16 && d->count && !d->keysSet) d->fcntHigh++;   // best guess without keys
  d->mtype = u.mtype; d->fcnt16 = u.fcnt16; d->hasPort = u.hasPort; d->fport = u.fport; d->plen = u.payLen;
  d->rssi = it.rssi; d->snrQ4 = it.snrQ4; d->sf = it.sf; d->freqKHz = it.freqKHz;
  d->phyLen = (uint8_t)(it.len > LW_PHY ? 0 : it.len);
  if (d->phyLen) memcpy(d->phy, it.data, d->phyLen);
  d->lastMs = it.ms; d->count++; d->seq++;
  MainPick m{ "", 99 };
  int n = lwValues(*d, pickMain, &m);
  if (m.prio == 99) m.s = n == -1 ? String("Schlüssel falsch") : join ? String("Join-Request") : "FCnt " + String(u.fcnt16);
  setMain(*d, m);
}

void unl_service() {
  if (!g_rxq) return;
  static RxItem it;
  for (int i = 0; i < RXQ_LEN && xQueueReceive(g_rxq, &it, 0) == pdTRUE; i++) {
    g_st.rx[it.listener]++;
    switch (it.listener) {
      case UL_WMB_TC: case UL_WMB_S1: processWmbus(it); break;
      case UL_WX_BRES: case UL_WX_LACR: processWeather(it); break;
      case UL_LORAWAN: processLorawan(it); break;
    }
  }
}

// ============================================================================ web API
static WebServer *g_srv = nullptr;

static String cfgJson() {
  UnlRadioCfg c; unl_radio_cfg(c);
  return String("{\"obi\":") + (c.obiOn ? "true" : "false") + ",\"listen\":" + c.listen + ",\"win\":" + c.obiWindowMs +
         ",\"lwch\":" + c.lwChMask + ",\"lwsf\":" + c.lwSfMask + ",\"avh\":" + g_availHours + "}";
}
static String statsJson() {
  String s = "{\"ms\":[";
  for (int i = 0; i <= UL_COUNT; i++) { if (i) s += ","; s += String(g_st.slotMs[i]); }
  s += "],\"rx\":[";
  for (int i = 0; i < UL_COUNT; i++) { if (i) s += ","; s += String(g_st.rx[i]); }
  s += "],\"ok\":[";
  for (int i = 0; i < UL_COUNT; i++) { if (i) s += ","; s += String(g_st.ok[i]); }
  return s + "],\"crc\":" + g_st.crc + ",\"switches\":" + g_st.switches + ",\"switch_err\":" + g_st.switchErrors +
         ",\"defers\":" + g_st.defers + ",\"qdrop\":" + g_st.queueDrops + ",\"obi_frames\":" + g_st.obiFrames +
         ",\"obi_last_off\":" + g_st.obiLastOffset + ",\"obi_max_off\":" + g_st.obiMaxOffset + "}";
}
static String baseJson(const DevBase &d, const char *key) {
  return String("{\"k\":\"") + key + "\",\"name\":" + jstr(d.name) + ",\"main\":" + jstr(d.main) + ",\"rssi\":" + d.rssi +
         ",\"count\":" + d.count + ",\"age\":" + (d.lastMs ? String((millis() - d.lastMs) / 1000) : String("null")) +
         ",\"sel\":" + (d.selected ? "true" : "false");
}
static String wmJson(const WmDev &d) {
  char key[13], m[4]; wmKey(d.mfr, d.id, key); wmb::mfrCode(d.mfr, m);
  char idb[9]; snprintf(idb, sizeof idb, "%08X", (unsigned)d.id);
  const char *mn = mfrName(d.mfr);
  return baseJson(d, key) + ",\"mfr\":\"" + m + "\",\"mfrn\":" + jstr(mn ? mn : "") + ",\"id\":\"" + idb +
         "\",\"type\":" + d.type + ",\"tname\":" + jstr(typeDe(d.type)) + ",\"ver\":" + d.ver + ",\"mode\":\"" +
         wmb::modeName(d.mode) + "\",\"ci\":" + d.ci + ",\"sec\":" + d.secMode + ",\"dec\":\"" +
         (d.lastMs ? wmb::decryptResultName(d.dec) : "") + "\",\"key\":" + (d.keySet ? "true" : "false") +
         ",\"status\":" + d.status + ",\"stxt\":" + jstr(wmb::statusText(d.status)) + "}";
}
static String wxJson(const WxDev &d) {
  char key[24]; wxKey(d, key);
  return baseJson(d, key) + ",\"proto\":" + jstr(wx::protoName(d.proto)) + ",\"id\":\"" + String(d.id, HEX) +
         "\",\"ch\":" + d.r.channel + ",\"exp\":" + (d.proto == wx::P_BRESSER6 || d.proto == wx::P_BRESSER7 ? "true" : "false") + "}";
}
static String lwJson(const LwDev &d) {
  char key[18]; lwKey(d, key);
  return baseJson(d, key) + ",\"join\":" + (d.join ? "true" : "false") + ",\"mtype\":" + jstr(lwMtype(d.mtype)) +
         ",\"fcnt\":" + ((uint32_t)d.fcntHigh << 16 | d.fcnt16) + ",\"fport\":" + (d.hasPort ? String(d.fport) : String("null")) +
         ",\"plen\":" + d.plen + ",\"snr\":" + num(d.snrQ4 / 4.0, 4) + ",\"sf\":" + d.sf + ",\"freq\":" +
         num(d.freqKHz / 1000.0, 7) + ",\"keys\":" + (d.keysSet ? "true" : "false") + "}";
}

template <typename D, int N, typename J>
static void streamTable(String &chunk, D (&tab)[N], J json) {
  bool first = true;
  for (auto &d : tab) {
    if (!d.used) continue;
    if (!first) chunk += ",";
    first = false;
    chunk += json(d);
    if (chunk.length() > 1200) { g_srv->sendContent(chunk); chunk = ""; }
  }
}
static void handleList() {
  WebServer &s = *g_srv;
  s.setContentLength(CONTENT_LENGTH_UNKNOWN);
  s.send(200, "application/json", "");
  String chunk = "{\"cfg\":" + cfgJson() + ",\"stats\":" + statsJson() + ",\"wm\":[";
  streamTable(chunk, g_wm, wmJson);
  chunk += "],\"wx\":[";
  streamTable(chunk, g_wx, wxJson);
  chunk += "],\"lw\":[";
  streamTable(chunk, g_lw, lwJson);
  chunk += "]}";
  s.sendContent(chunk);
  s.sendContent("");
}

template <typename D, int N, typename K>
static D *byKey(D (&tab)[N], const String &k, K keyFn) {
  for (auto &d : tab) { if (!d.used) continue; char key[24]; keyFn(d, key); if (k.equalsIgnoreCase(key)) return &d; }
  return nullptr;
}
static WmDev *wmBy(const String &k) { return byKey(g_wm, k, [](const WmDev &d, char *o) { wmKey(d.mfr, d.id, o); }); }
static WxDev *wxBy(const String &k) { return byKey(g_wx, k, [](const WxDev &d, char *o) { wxKey(d, o); }); }
static LwDev *lwBy(const String &k) { return byKey(g_lw, k, [](const LwDev &d, char *o) { lwKey(d, o); }); }

struct ListCtx { String *out; bool first; };
static void valToDetail(const Val &v, void *c) {
  ListCtx &x = *(ListCtx *)c;
  if (!x.first) *x.out += ",";
  x.first = false;
  *x.out += String("{\"k\":\"") + v.key + "\",\"l\":" + jstr(v.label.c_str()) + ",\"v\":" + v.json + ",\"u\":" +
            jstr(v.unit ? v.unit : "") + (v.diag ? ",\"d\":1" : "") + "}";
}

static void handleDetail() {
  WebServer &s = *g_srv;
  String t = s.arg("t"), k = s.arg("k"), out;
  ListCtx x{ &out, true };
  if (t == "wm") {
    WmDev *d = wmBy(k);
    if (!d) { s.send(404, "application/json", "{\"ok\":false}"); return; }
    String frame, plain;
    out = "{\"dev\":" + wmJson(*d) + ",\"vals\":[";
    int dr = wmValues(*d, valToDetail, &x, &frame, &plain);
    out += "],\"frame\":\"" + frame + "\",\"plain\":\"" + plain + "\",\"cached\":" + (dr == -100 ? "false" : "true");
    const char *md = nullptr;
    if (dr != -100) {   // name the manufacturer decoder that was used, if any
      static wmb::Frame f;
      const FrameSlot &fs = g_fc[d->cache];
      f.len = fs.len; f.mode = fs.mode; memcpy(f.data, fs.data, f.len);
      wmb::Header h;
      if (wmb::parseHeader(f, h)) md = wmb::manufacturerDecoderName(f, h);
    }
    out += String(",\"decoder\":") + jstr(md ? md : "") + "}";
  } else if (t == "wx") {
    WxDev *d = wxBy(k);
    if (!d) { s.send(404, "application/json", "{\"ok\":false}"); return; }
    out = "{\"dev\":" + wxJson(*d) + ",\"vals\":[";
    wxValues(*d, valToDetail, &x);
    out += "]}";
  } else if (t == "lw") {
    LwDev *d = lwBy(k);
    if (!d) { s.send(404, "application/json", "{\"ok\":false}"); return; }
    String plain;
    out = "{\"dev\":" + lwJson(*d) + ",\"vals\":[";
    int n = lwValues(*d, valToDetail, &x, &plain);
    out += "],\"phy\":\"" + hexStr(d->phy, d->phyLen) + "\",\"plain\":\"" + plain + "\",\"dec\":" + n + "}";
  } else { s.send(400, "application/json", "{\"ok\":false}"); return; }
  s.send(200, "application/json", out);
}

static bool argOn(WebServer &s, const char *n) { return s.arg(n) == "1" || s.arg(n) == "true"; }
static void applyCommon(WebServer &s, DevBase &d) {
  if (s.hasArg("name")) { strlcpy(d.name, s.arg("name").c_str(), sizeof d.name); d.discDone = false; d.seq++; }
  if (s.hasArg("sel")) {
    bool on = argOn(s, "sel");
    if (on != d.selected) { d.selected = on; d.discDone = false; d.avail = on ? -1 : -2; d.seq++; }
  }
}
static bool applyForget(WebServer &s, DevBase &d) {
  if (!s.hasArg("forget")) return false;
  bool wasSel = d.selected;
  d.selected = false; d.name[0] = 0;
  if (wasSel) { d.avail = -2; d.drop = true; } else d.used = false;
  return true;
}

static void handleSet() {
  WebServer &s = *g_srv;
  String t = s.arg("t"), k = s.arg("k"), err;
  DevBase *base = nullptr;
  if (t == "wm") {
    WmDev *d = wmBy(k);
    if (d && !applyForget(s, *d)) {
      if (s.hasArg("key")) {
        String v = s.arg("key");
        if (!v.length()) { d->keySet = false; memset(d->key, 0, 16); }
        else if (parseHex(v, d->key, 16)) d->keySet = true;
        else err = "AES-Schlüssel: 32 Hex-Zeichen";
        d->discDone = false; d->seq++;
        MainPick m{ "", 99 };
        int dr = wmValues(*d, pickMain, &m);
        if (dr != -100) { d->dec = (int8_t)dr; if (m.prio == 99 && dr == wmb::DR_NO_KEY) m.s = "verschlüsselt"; setMain(*d, m); }
      }
      applyCommon(s, *d);
    } else if (d) { d->keySet = false; }
    base = d;
  } else if (t == "wx") {
    WxDev *d = wxBy(k);
    if (d && !applyForget(s, *d)) applyCommon(s, *d);
    base = d;
  } else if (t == "lw") {
    LwDev *d = lwBy(k);
    if (d && !applyForget(s, *d)) {
      if (s.hasArg("nwk") || s.hasArg("app")) {
        String n = s.arg("nwk"), a = s.arg("app");
        if (!n.length() && !a.length()) d->keysSet = false;
        else if (parseHex(n, d->nwk, 16) && parseHex(a, d->app, 16)) { d->keysSet = true; d->fcntHigh = 0; }
        else err = "NwkSKey und AppSKey: je 32 Hex-Zeichen";
        d->discDone = false; d->seq++;
        MainPick m{ "", 99 };
        int r = lwValues(*d, pickMain, &m);
        if (m.prio == 99) m.s = r == -1 ? String("Schlüssel falsch") : "FCnt " + String(d->fcnt16);
        setMain(*d, m);
      }
      applyCommon(s, *d);
    } else if (d) { d->keysSet = false; }
    base = d;
  }
  if (!base) { s.send(404, "application/json", "{\"ok\":false,\"err\":\"unbekanntes Gerät\"}"); return; }
  g_mqttDirty = true;
  saveDevices();
  if (err.length()) s.send(400, "application/json", "{\"ok\":false,\"err\":" + jstr(err.c_str()) + "}");
  else s.send(200, "application/json", "{\"ok\":true}");
}

static void handleLog() {
  WebServer &s = *g_srv;
  uint32_t since = s.arg("since").toInt();
  String out = "{\"seq\":" + String(g_logSeq) + ",\"now\":" + millis() + ",\"e\":[";
  bool first = true;
  for (int i = 0; i < LOG_N; i++) {
    const LogEntry &e = g_log[(g_logHead + i) % LOG_N];
    if (!e.seq || e.seq <= since) continue;
    if (!first) out += ",";
    first = false;
    out += String("{\"s\":") + e.seq + ",\"t\":" + e.ms + ",\"l\":" + e.listener + ",\"m\":" + e.mode + ",\"r\":" + e.result +
           ",\"rssi\":" + e.rssi + ",\"len\":" + e.len + ",\"who\":" + jstr(e.who) + ",\"b\":\"" + hexStr(e.hex, e.hexLen) + "\"}";
  }
  s.send(200, "application/json", out + "]}");
}

static void handleCfg() {
  WebServer &s = *g_srv;
  UnlRadioCfg c; unl_radio_cfg(c);
  if (s.hasArg("obi")) c.obiOn = argOn(s, "obi");
  if (s.hasArg("listen")) c.listen = (uint8_t)(s.arg("listen").toInt() & ((1 << UL_COUNT) - 1));
  if (s.hasArg("win")) { long w = s.arg("win").toInt(); if (w >= 100 && w <= 900) c.obiWindowMs = (uint16_t)w; }
  if (s.hasArg("lwch")) { long m = s.arg("lwch").toInt() & 0xFF; if (m) c.lwChMask = (uint8_t)m; }
  if (s.hasArg("lwsf")) { long m = s.arg("lwsf").toInt() & 0x3F; if (m) c.lwSfMask = (uint8_t)m; }
  if (s.hasArg("avh")) { long h = s.arg("avh").toInt(); if (h >= 1 && h <= 720) g_availHours = (uint16_t)h; }
  if (!c.obiOn && !c.listen) c.obiOn = true;            // the radio has to do *something*
  if (c.obiOn && !(c.lwSfMask & 0x0F)) c.lwSfMask |= 1; // SF11/12 frames don't fit into the OBI idle time
  portENTER_CRITICAL(&g_cfgMux); g_cfg = c; portEXIT_CRITICAL(&g_cfgMux);
  saveCfg();
  g_mqttDirty = true;
  s.send(200, "application/json", "{\"ok\":true,\"cfg\":" + cfgJson() + "}");
}

static void handleStatsReset() {
  for (int i = 0; i <= UL_COUNT; i++) g_st.slotMs[i] = 0;
  for (int i = 0; i < UL_COUNT; i++) { g_st.rx[i] = 0; g_st.ok[i] = 0; }
  g_st.crc = 0; g_st.obiMaxOffset = 0;
  g_srv->send(200, "application/json", "{\"ok\":true}");
}

static void handleMetersRedirect() { g_srv->sendHeader("Location", "/#funk"); g_srv->send(302, "text/plain", ""); }

void unl_register_routes(WebServer &srv, WebServer::THandlerFunction (*guard)(WebServer::THandlerFunction)) {
  g_srv = &srv;
  srv.on("/meters", HTTP_GET, guard(handleMetersRedirect));   // old bookmark -> dashboard tab
  srv.on("/api/unl", HTTP_GET, guard(handleList));
  srv.on("/api/unl/dev", HTTP_GET, guard(handleDetail));
  srv.on("/api/unl/dev", HTTP_POST, guard(handleSet));
  srv.on("/api/unl/log", HTTP_GET, guard(handleLog));
  srv.on("/api/unl/cfg", HTTP_POST, guard(handleCfg));
  srv.on("/api/unl/stats/reset", HTTP_POST, guard(handleStatsReset));
}

// ============================================================================ MQTT / Home Assistant
static bool publishBig(PubSubClient &m, const String &topic, const String &payload, bool retain, uint32_t &cnt) {
  if (!m.beginPublish(topic.c_str(), payload.length(), retain)) return false;   // streams past PubSubClient's buffer
  m.write((const uint8_t *)payload.c_str(), payload.length());
  bool ok = m.endPublish();
  if (ok) cnt++;
  return ok;
}

static String haComp(const String &uid, const char *key, const String &name, const String &tpl, const char *unit,
                     const char *dc, const char *sc, bool diag, bool bin = false) {
  String s = String("\"") + key + "\":{\"p\":\"" + (bin ? "binary_sensor" : "sensor") + "\",\"uniq_id\":\"" + uid + "_" + key +
             "\",\"name\":" + jstr(name.c_str()) + ",\"val_tpl\":" + jstr(tpl.c_str());
  if (unit && unit[0]) s += ",\"unit_of_meas\":" + jstr(unit);
  if (dc) s += String(",\"dev_cla\":\"") + dc + "\"";
  if (sc) s += String(",\"stat_cla\":\"") + sc + "\"";
  if (diag) s += ",\"ent_cat\":\"diagnostic\"";
  return s + "}";
}

struct PubCtx { String cmps, vals; String uid; uint32_t hash; bool first; };
static void valToPub(const Val &v, void *c) {
  PubCtx &p = *(PubCtx *)c;
  for (const char *k = v.key; *k; k++) p.hash = p.hash * 31 + (uint8_t)*k;
  if (!p.first) p.vals += ",";
  p.first = false;
  p.vals += String("\"") + v.key + "\":" + v.json;
  String tpl = v.bin ? String("{{ 'ON' if value_json.v.") + v.key + " else 'OFF' }}" : String("{{ value_json.v.") + v.key + " }}";
  p.cmps += "," + haComp(p.uid, v.key, v.label, tpl, v.unit, v.dc, v.sc, v.diag, v.bin);
}

// Shared publish logic: removal, availability, discovery (when the value set / name changed), state.
static void publishDev(PubSubClient &m, DevBase &d, const String &uid, const String &st, const String &devName,
                       const char *mf, const char *model, const String &gwAvt, const String &gwUid, uint32_t &cnt,
                       const String &extra, void (*values)(DevBase &, PubCtx &)) {
  String cfgTopic = "homeassistant/device/" + uid + "/config";
  if (d.avail == -2) {
    publishBig(m, cfgTopic, "", true, cnt);
    publishBig(m, st, "", true, cnt);
    publishBig(m, st + "/status", "", true, cnt);
    d.avail = -1; d.discDone = false;
    if (d.drop) d.used = false;
    return;
  }
  if (!d.selected) return;
  bool online = d.lastMs && (millis() - d.lastMs) < (uint32_t)g_availHours * 3600000UL;
  if (d.avail != (online ? 1 : 0)) { publishBig(m, st + "/status", online ? "online" : "offline", true, cnt); d.avail = online ? 1 : 0; }
  if (d.seq == d.pubSeq && d.discDone) return;
  if (!d.lastMs) { d.discDone = true; d.pubSeq = d.seq; return; }   // nothing heard since boot: announce once values exist
  PubCtx p;
  p.uid = uid; p.hash = 0; p.first = true;
  values(d, p);
  for (const char *c = d.name; *c; c++) p.hash = p.hash * 31 + (uint8_t)*c;
  if (!d.discDone || p.hash != d.discHash) {
    String body = String("{\"dev\":{\"ids\":[\"") + uid + "\"],\"name\":" + jstr(devName.c_str()) + ",\"mf\":" + jstr(mf) +
                  ",\"mdl\":" + jstr(model) + ",\"via_device\":\"" + gwUid + "\"},\"o\":{\"name\":\"ObiUnlocked\",\"sw\":\"" +
                  FW_VERSION + "\",\"url\":\"https://forgejo.tzru.de/tzru/ObiUnlocked\"},\"stat_t\":\"" + st +
                  "\",\"avty\":[{\"t\":\"" + gwAvt + "\"},{\"t\":\"" + st + "/status\"}],\"avty_mode\":\"all\",\"cmps\":{" +
                  haComp(uid, "rssi", "Signalstärke", "{{ value_json.rssi }}", "dBm", "signal_strength", "measurement", true) +
                  (timeOk() ? "," + haComp(uid, "last_seen", "Zuletzt gesehen", "{{ value_json.last_seen }}", "", "timestamp", nullptr, true) : String("")) +
                  p.cmps + "}}";
    if (publishBig(m, cfgTopic, body, true, cnt)) { d.discDone = true; d.discHash = p.hash; }
  }
  String last = isoAgo(d.lastMs);
  String body = String("{\"rssi\":") + d.rssi + ",\"count\":" + d.count + (last.length() ? ",\"last_seen\":\"" + last + "\"" : String("")) +
                extra + ",\"v\":{" + p.vals + "}}";
  if (publishBig(m, st, body, true, cnt)) d.pubSeq = d.seq;
}

static void wmVals(DevBase &b, PubCtx &p) { wmValues(static_cast<WmDev &>(b), valToPub, &p); }
static void wxVals(DevBase &b, PubCtx &p) { wxValues(static_cast<WxDev &>(b), valToPub, &p); }
static void lwVals(DevBase &b, PubCtx &p) { lwValues(static_cast<LwDev &>(b), valToPub, &p); }

void unl_mqtt_reset() {
  for (auto &d : g_wm) { d.discDone = false; if (d.avail != -2) d.avail = -1; d.pubSeq = d.seq - 1; }
  for (auto &d : g_wx) { d.discDone = false; if (d.avail != -2) d.avail = -1; d.pubSeq = d.seq - 1; }
  for (auto &d : g_lw) { d.discDone = false; if (d.avail != -2) d.avail = -1; d.pubSeq = d.seq - 1; }
}

void unl_mqtt_service(PubSubClient &m, const String &base, const String &gwAvt, const String &gwUid, uint32_t &cnt) {
  static int wi = 0, xi = 0, li = 0;
  static uint32_t lastSweep = 0;
  int budget = 3;   // a few devices per pass so a burst never stalls the web task
  bool sweep = g_mqttDirty || millis() - lastSweep > 30000;
  if (sweep) { g_mqttDirty = false; lastSweep = millis(); }
  auto needs = [&](DevBase &d) {
    return d.used && (d.selected || d.avail == -2) && (d.avail == -2 || d.seq != d.pubSeq || !d.discDone || sweep);
  };
  for (int n = 0; n < WM_MAX && budget > 0 && m.connected(); n++, wi = (wi + 1) % WM_MAX) {
    WmDev &d = g_wm[wi];
    if (!needs(d)) continue;
    char key[13], mf[4]; wmKey(d.mfr, d.id, key); wmb::mfrCode(d.mfr, mf);
    const char *mn = mfrName(d.mfr);
    char idb[9]; snprintf(idb, sizeof idb, "%08X", (unsigned)d.id);
    String extra = String(",\"id\":\"") + idb + "\",\"mfr\":\"" + mf + "\",\"type\":" + jstr(typeDe(d.type)) + ",\"mode\":\"" +
                   wmb::modeName(d.mode) + "\",\"decryption\":\"" + wmb::decryptResultName(d.dec) + "\"";
    publishDev(m, d, String("obiu_wm_") + key, base + "/wmbus/" + key,
               d.name[0] ? String(d.name) : String(typeDe(d.type)) + " " + idb, mn ? mn : mf, typeDe(d.type), gwAvt, gwUid,
               cnt, extra, wmVals);
    budget--;
  }
  for (int n = 0; n < WX_MAX && budget > 0 && m.connected(); n++, xi = (xi + 1) % WX_MAX) {
    WxDev &d = g_wx[xi];
    if (!needs(d)) continue;
    char key[24]; wxKey(d, key);
    publishDev(m, d, String("obiu_wx_") + key, base + "/weather/" + key, d.name[0] ? String(d.name) : String(key),
               wx::protoName(d.proto), "Wettersensor", gwAvt, gwUid, cnt, String(",\"channel\":") + d.r.channel, wxVals);
    budget--;
  }
  for (int n = 0; n < LW_MAX && budget > 0 && m.connected(); n++, li = (li + 1) % LW_MAX) {
    LwDev &d = g_lw[li];
    if (!needs(d)) continue;
    char key[18]; lwKey(d, key);
    String extra = String(",\"fcnt\":") + ((uint32_t)d.fcntHigh << 16 | d.fcnt16) + ",\"fport\":" +
                   (d.hasPort ? String(d.fport) : String("null")) + ",\"snr\":" + num(d.snrQ4 / 4.0, 4) + ",\"sf\":" + d.sf +
                   ",\"freq\":" + num(d.freqKHz / 1000.0, 7);
    publishDev(m, d, String("obiu_lw_") + key, base + "/lorawan/" + key, d.name[0] ? String(d.name) : String("LoRaWAN ") + key,
               "LoRaWAN", d.join ? "Join-Request (DevEUI)" : "Uplink (DevAddr)", gwAvt, gwUid, cnt, extra, lwVals);
    budget--;
  }
}
