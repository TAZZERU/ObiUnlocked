// unl.cpp -- ObiUnlocked: wireless M-Bus + LoRaWAN device lists, web API and MQTT/Home Assistant output.
// See include/unl.h for the overall design and the threading rules.
#include "unl.h"

#include <PubSubClient.h>
#include <Preferences.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <time.h>

#include "wmbus_core.h"

#if defined(__has_include)
#  if __has_include("web_assets.h")
#    include "web_assets.h"
#    define UNL_HAVE_PAGE 1
#  endif
#endif
#include "web_gzip.h"

#ifndef FW_VERSION
#  if __has_include("fw_version.h")
#    include "fw_version.h"
#  endif
#endif
#ifndef FW_VERSION
#  define FW_VERSION "0.0.0-dev"
#endif

// ============================================================================ limits
static const int WM_MAX = 36;            // wM-Bus devices kept in RAM (an apartment block easily has 30+)
static const int WM_FRAME_MAX = 180;     // link-layer frame bytes kept per device (CRCs stripped)
static const int LW_MAX = 24;            // LoRaWAN devices kept in RAM
static const int LOG_N = 24;             // recent-telegram log entries
static const int LOG_HEX = 48;           // bytes of each telegram kept in the log
static const int RXQ_LEN = 6;            // frames buffered between LoRa task and web task
static const int UNL_NAME_MAX = 25;

// ============================================================================ radio-side state
static portMUX_TYPE g_cfgMux = portMUX_INITIALIZER_UNLOCKED;
static UnlRadioCfg g_cfg;
static uint16_t g_availHours = 26;       // HA: a device is offline after this long without a telegram

struct RxItem {
  uint8_t kind, len, sf;
  int8_t snrQ4;
  int16_t rssi;
  uint32_t freqKHz, ms;
  uint8_t data[255];
};
static QueueHandle_t g_rxq = nullptr;

struct Stats {
  volatile uint32_t slotMs[UNL_SLOT_COUNT] = {0, 0, 0};
  volatile uint32_t switches = 0, switchErrors = 0, defers = 0, queueDrops = 0;
  volatile uint16_t obiLastOffset = 0, obiMaxOffset = 0;
  volatile uint32_t obiFrames = 0;
  uint32_t wmFrames = 0, wmOk = 0, wmCrc = 0, wm3of6 = 0, wmOther = 0;
  uint32_t lwFrames = 0, lwOk = 0;
};
static Stats g_st;

void unl_radio_cfg(UnlRadioCfg &out) {
  portENTER_CRITICAL(&g_cfgMux);
  out = g_cfg;
  portEXIT_CRITICAL(&g_cfgMux);
}

void unl_rx_push(uint8_t kind, const uint8_t *data, size_t len, int16_t rssi, int8_t snrQ4, uint8_t sf,
                 uint32_t freqKHz) {
  if (!g_rxq || !data || !len) return;
  static RxItem it;   // only ever used from the (single) LoRa task -> no need for a stack copy of 270 B
  it.kind = kind; it.len = (uint8_t)(len > 255 ? 255 : len); it.sf = sf; it.snrQ4 = snrQ4; it.rssi = rssi;
  it.freqKHz = freqKHz; it.ms = millis();
  memcpy(it.data, data, it.len);
  if (xQueueSend(g_rxq, &it, 0) != pdTRUE) g_st.queueDrops++;
}

void unl_note_slot(uint8_t slot, uint32_t ms) { if (slot < UNL_SLOT_COUNT) g_st.slotMs[slot] += ms; }
void unl_note_switch(bool ok) { g_st.switches++; if (!ok) g_st.switchErrors++; }
void unl_note_defer() { g_st.defers++; }
void unl_note_obi_rx(uint16_t ms) {
  g_st.obiFrames++;
  g_st.obiLastOffset = ms;
  if (ms > g_st.obiMaxOffset) g_st.obiMaxOffset = ms;
}

// ============================================================================ device tables (web task)
struct WmDev {
  bool used = false, selected = false, keySet = false;
  uint8_t key[16] = {0};
  uint16_t mfr = 0;
  uint32_t id = 0;
  uint8_t ver = 0, type = 0, mode = 0, ci = 0, secMode = 0, status = 0;
  int8_t dec = 0;                 // wmb::DecryptResult of the latest telegram
  int16_t rssi = 0;
  uint32_t lastMs = 0, count = 0, seq = 0;
  char name[UNL_NAME_MAX] = {0};
  uint8_t frameLen = 0;
  bool truncated = false;
  uint8_t frame[WM_FRAME_MAX];
  // MQTT bookkeeping
  uint32_t pubSeq = 0;
  bool discDone = false;
  uint32_t discHash = 0;
  int8_t avail = -1;              // last published availability; -2 = remove from HA in the next MQTT pass
  bool drop = false;              // forget the device once its HA entries are removed
};
static WmDev g_wm[WM_MAX];

struct LwDev {
  bool used = false, selected = false, join = false;   // join: identified by DevEUI from a join request
  uint8_t eui[8] = {0};
  uint32_t devaddr = 0;
  uint8_t mtype = 0, fport = 0, sf = 0, plen = 0;
  bool hasPort = false;
  uint32_t fcnt = 0, freqKHz = 0;
  int16_t rssi = 0;
  int8_t snrQ4 = 0;
  uint32_t lastMs = 0, count = 0, seq = 0;
  char name[UNL_NAME_MAX] = {0};
  uint32_t pubSeq = 0;
  bool discDone = false;
  int8_t avail = -1;
  bool drop = false;
};
static LwDev g_lw[LW_MAX];

struct LogEntry {
  uint32_t seq = 0, ms = 0;
  uint8_t kind = 0, mode = 0, len = 0, hexLen = 0;
  int8_t result = 0;              // RadioResult (wM-Bus) / 0 ok
  int16_t rssi = 0;
  char who[20] = {0};
  uint8_t hex[LOG_HEX];
};
static LogEntry g_log[LOG_N];
static int g_logHead = 0;
static uint32_t g_logSeq = 0;

static void logAdd(uint8_t kind, uint8_t mode, int8_t result, int16_t rssi, const char *who, const uint8_t *p,
                   size_t n) {
  LogEntry &e = g_log[g_logHead];
  g_logHead = (g_logHead + 1) % LOG_N;
  e.seq = ++g_logSeq; e.ms = millis(); e.kind = kind; e.mode = mode; e.result = result; e.rssi = rssi;
  e.len = (uint8_t)(n > 255 ? 255 : n);
  strlcpy(e.who, who ? who : "", sizeof e.who);
  e.hexLen = (uint8_t)(n > LOG_HEX ? LOG_HEX : n);
  memcpy(e.hex, p, e.hexLen);
}

// ---- keys / formatting
static void wmKey(uint16_t mfr, uint32_t id, char out[13]) {
  char m[4];
  wmb::mfrCode(mfr, m);
  snprintf(out, 13, "%s%08X", m, (unsigned)id);
}
static void lwKey(const LwDev &d, char out[18]) {
  if (d.join) {
    char *o = out;
    *o++ = 'J';
    for (int i = 7; i >= 0; i--) o += sprintf(o, "%02X", d.eui[i]);
  } else {
    snprintf(out, 18, "%08X", (unsigned)d.devaddr);
  }
}
static String hexStr(const uint8_t *p, size_t n) {
  static const char *H = "0123456789ABCDEF";
  String s;
  s.reserve(n * 2);
  for (size_t i = 0; i < n; i++) { s += H[p[i] >> 4]; s += H[p[i] & 15]; }
  return s;
}
static bool parseHex(const String &s, uint8_t *out, size_t n) {
  String t = s;
  t.replace(" ", ""); t.replace(":", ""); t.replace("-", "");
  if (t.length() != n * 2) return false;
  for (size_t i = 0; i < n; i++) {
    char b[3] = { t[2 * i], t[2 * i + 1], 0 };
    char *end;
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
static String num(double v) {
  if (isnan(v) || isinf(v)) return "null";
  char b[24];
  snprintf(b, sizeof b, "%.10g", v);
  return b;
}
static bool timeOk() { return time(nullptr) > 1700000000; }
static String isoAgo(uint32_t ms) {   // millis() timestamp -> ISO-8601 UTC, "" if the clock isn't set
  if (!timeOk() || !ms) return "";
  time_t t = time(nullptr) - (time_t)((millis() - ms) / 1000);
  struct tm g;
  gmtime_r(&t, &g);
  char b[24];
  strftime(b, sizeof b, "%Y-%m-%dT%H:%M:%SZ", &g);
  return b;
}
static const char *lwMtypeName(uint8_t m) {
  static const char *N[8] = { "join_request", "join_accept", "unconf_up", "unconf_down",
                              "conf_up", "conf_down", "rejoin", "proprietary" };
  return N[m & 7];
}

// ============================================================================ persistence (NVS ns "unl")
struct __attribute__((packed)) WmPersist { uint16_t mfr; uint32_t id; uint8_t flags; uint8_t key[16]; char name[UNL_NAME_MAX]; };
struct __attribute__((packed)) LwPersist { uint8_t join; uint8_t eui[8]; uint32_t devaddr; uint8_t flags; char name[UNL_NAME_MAX]; };

// one scratch buffer for load + save (both run before/on the web task, never concurrently)
static union { WmPersist wm[WM_MAX]; LwPersist lw[LW_MAX]; } g_persistBuf;

static void saveCfg() {
  UnlRadioCfg c;
  unl_radio_cfg(c);
  Preferences p;
  p.begin("unl", false);
  p.putBool("obi", c.obiOn); p.putBool("wmb", c.wmbusOn); p.putBool("lw", c.lorawanOn);
  p.putUShort("win", c.obiWindowMs); p.putULong("lwf", c.lorawanFreqKHz); p.putUChar("lwsf", c.lorawanSf);
  p.putUShort("avh", g_availHours);
  p.end();
}

static void saveDevices() {
  WmPersist *wp = g_persistBuf.wm;
  LwPersist *lp = g_persistBuf.lw;
  int nw = 0, nl = 0;
  Preferences p;
  p.begin("unl", false);
  for (auto &d : g_wm)
    if (d.used && (d.selected || d.keySet || d.name[0])) {
      WmPersist &x = wp[nw++];
      memset(&x, 0, sizeof x);
      x.mfr = d.mfr; x.id = d.id; x.flags = (d.selected ? 1 : 0) | (d.keySet ? 2 : 0);
      memcpy(x.key, d.key, 16); strlcpy(x.name, d.name, sizeof x.name);
    }
  if (nw) p.putBytes("wm", wp, nw * sizeof(WmPersist)); else p.remove("wm");
  for (auto &d : g_lw)
    if (d.used && (d.selected || d.name[0])) {
      LwPersist &x = lp[nl++];
      memset(&x, 0, sizeof x);
      x.join = d.join; memcpy(x.eui, d.eui, 8); x.devaddr = d.devaddr; x.flags = d.selected ? 1 : 0;
      strlcpy(x.name, d.name, sizeof x.name);
    }
  if (nl) p.putBytes("lw", lp, nl * sizeof(LwPersist)); else p.remove("lw");
  p.end();
}

void unl_setup() {
  if (!g_rxq) g_rxq = xQueueCreate(RXQ_LEN, sizeof(RxItem));
  Preferences p;
  p.begin("unl", true);
  UnlRadioCfg c;
  c.obiOn = p.getBool("obi", true);
  c.wmbusOn = p.getBool("wmb", true);
  c.lorawanOn = p.getBool("lw", false);
  c.obiWindowMs = p.getUShort("win", 300);
  c.lorawanFreqKHz = p.getULong("lwf", 868100);
  c.lorawanSf = p.getUChar("lwsf", 7);
  g_availHours = p.getUShort("avh", 26);
  if (c.obiWindowMs < 100 || c.obiWindowMs > 900) c.obiWindowMs = 300;
  if (c.lorawanSf < 7 || c.lorawanSf > 12) c.lorawanSf = 7;
  if (c.lorawanFreqKHz < 863000 || c.lorawanFreqKHz > 870000) c.lorawanFreqKHz = 868100;
  if (g_availHours < 1) g_availHours = 26;
  WmPersist *wp = g_persistBuf.wm;
  size_t nw = p.getBytes("wm", wp, sizeof g_persistBuf.wm) / sizeof(WmPersist);
  portENTER_CRITICAL(&g_cfgMux);
  g_cfg = c;
  portEXIT_CRITICAL(&g_cfgMux);
  for (size_t i = 0; i < nw && i < (size_t)WM_MAX; i++) {
    WmDev &d = g_wm[i];
    d = WmDev();
    d.used = true; d.mfr = wp[i].mfr; d.id = wp[i].id;
    d.selected = wp[i].flags & 1; d.keySet = wp[i].flags & 2;
    memcpy(d.key, wp[i].key, 16);
    strlcpy(d.name, wp[i].name, sizeof d.name);
  }
  LwPersist *lp = g_persistBuf.lw;   // reuses the buffer: the wM-Bus entries are already copied out
  size_t nl = p.getBytes("lw", lp, sizeof g_persistBuf.lw) / sizeof(LwPersist);
  p.end();
  for (size_t i = 0; i < nl && i < (size_t)LW_MAX; i++) {
    LwDev &d = g_lw[i];
    d = LwDev();
    d.used = true; d.join = lp[i].join; memcpy(d.eui, lp[i].eui, 8); d.devaddr = lp[i].devaddr;
    d.selected = lp[i].flags & 1;
    strlcpy(d.name, lp[i].name, sizeof d.name);
  }
  Serial.printf("[unl] wM-Bus %s, LoRaWAN %s (%.1f MHz SF%u), OBI %s, window %u ms, %u+%u known devices\n",
                c.wmbusOn ? "on" : "off", c.lorawanOn ? "on" : "off", c.lorawanFreqKHz / 1000.0, c.lorawanSf,
                c.obiOn ? "on" : "off", c.obiWindowMs, (unsigned)nw, (unsigned)nl);
}

void unl_factory_reset() {
  Preferences p;
  p.begin("unl", false);
  p.clear();
  p.end();
}

// ============================================================================ frame processing
static WmDev *wmFind(uint16_t mfr, uint32_t id) {
  for (auto &d : g_wm) if (d.used && d.mfr == mfr && d.id == id) return &d;
  return nullptr;
}
static WmDev *wmAlloc(uint16_t mfr, uint32_t id) {
  WmDev *slot = nullptr;
  for (auto &d : g_wm) if (!d.used) { slot = &d; break; }
  if (!slot) {   // evict the longest-unheard device the user doesn't care about
    for (auto &d : g_wm)
      if (!d.selected && !d.keySet && !d.name[0] && (!slot || (int32_t)(d.lastMs - slot->lastMs) < 0)) slot = &d;
  }
  if (!slot) return nullptr;
  *slot = WmDev();
  slot->used = true; slot->mfr = mfr; slot->id = id;
  return slot;
}

static void processWmbus(const RxItem &it) {
  g_st.wmFrames++;
  static wmb::Frame f;
  int rr = wmb::fromRadio(it.data, it.len, f);
  if (rr != wmb::RR_OK) {
    if (rr == wmb::RR_BAD_CRC) g_st.wmCrc++;
    else if (rr == wmb::RR_BAD_3OF6) g_st.wm3of6++;
    else g_st.wmOther++;
    // a CRC failure is almost always a real but weak/colliding telegram -- worth seeing in the log
    if (rr == wmb::RR_BAD_CRC) logAdd(UNL_RX_WMBUS, f.mode, (int8_t)rr, it.rssi, "CRC error", it.data, it.len);
    return;
  }
  wmb::Header h;
  if (!wmb::parseHeader(f, h)) { g_st.wmOther++; return; }
  g_st.wmOk++;
  WmDev *d = wmFind(h.meterMfr(), h.meterId());
  if (!d) d = wmAlloc(h.meterMfr(), h.meterId());
  char key[13];
  wmKey(h.meterMfr(), h.meterId(), key);
  logAdd(UNL_RX_WMBUS, f.mode, 0, it.rssi, key, f.data, f.len);
  if (!d) return;
  d->ver = h.meterVer(); d->type = h.meterType(); d->mode = f.mode; d->ci = h.ci;
  d->secMode = h.secMode; d->status = h.status; d->rssi = it.rssi; d->lastMs = it.ms;
  d->count++; d->seq++;
  d->truncated = f.len > WM_FRAME_MAX;
  d->frameLen = (uint8_t)(d->truncated ? WM_FRAME_MAX : f.len);
  memcpy(d->frame, f.data, d->frameLen);
  static uint8_t ap[wmb::FRAME_MAX];
  size_t apLen;
  d->dec = d->truncated ? (int8_t)wmb::DR_MALFORMED
                        : (int8_t)wmb::decrypt(f, h, d->keySet ? d->key : nullptr, ap, sizeof ap, apLen);
}

static void processLorawan(const RxItem &it) {
  g_st.lwFrames++;
  const uint8_t *p = it.data;
  size_t n = it.len;
  if (n < 1) return;
  uint8_t mtype = p[0] >> 5;
  LwDev probe;
  bool hasPort = false;
  uint8_t fport = 0, plen = 0;
  uint32_t fcnt = 0;
  if (mtype == 0 && n == 23) {                     // join request: JoinEUI DevEUI DevNonce MIC
    probe.join = true;
    memcpy(probe.eui, p + 9, 8);
  } else if ((mtype == 2 || mtype == 4) && n >= 12) {   // data uplink: DevAddr FCtrl FCnt FOpts [FPort payload] MIC
    probe.devaddr = (uint32_t)p[1] | ((uint32_t)p[2] << 8) | ((uint32_t)p[3] << 16) | ((uint32_t)p[4] << 24);
    uint8_t foptsLen = p[5] & 0x0F;
    fcnt = (uint32_t)p[6] | ((uint32_t)p[7] << 8);
    size_t hdr = 8 + foptsLen;
    if (n < hdr + 4) { logAdd(UNL_RX_LORAWAN, it.sf, -1, it.rssi, "malformed", p, n); return; }
    if (n > hdr + 4) { hasPort = true; fport = p[hdr]; plen = (uint8_t)(n - hdr - 1 - 4); }
  } else {
    logAdd(UNL_RX_LORAWAN, it.sf, -1, it.rssi, lwMtypeName(mtype), p, n);
    return;
  }
  g_st.lwOk++;
  LwDev *d = nullptr;
  for (auto &x : g_lw)
    if (x.used && x.join == probe.join &&
        (probe.join ? !memcmp(x.eui, probe.eui, 8) : x.devaddr == probe.devaddr)) { d = &x; break; }
  if (!d) {
    for (auto &x : g_lw) if (!x.used) { d = &x; break; }
    if (!d)
      for (auto &x : g_lw)
        if (!x.selected && !x.name[0] && (!d || (int32_t)(x.lastMs - d->lastMs) < 0)) d = &x;
    if (d) { *d = LwDev(); d->used = true; d->join = probe.join; memcpy(d->eui, probe.eui, 8); d->devaddr = probe.devaddr; }
  }
  char key[18];
  lwKey(probe, key);
  logAdd(UNL_RX_LORAWAN, it.sf, 0, it.rssi, key, p, n);
  if (!d) return;
  d->mtype = mtype; d->fcnt = fcnt; d->hasPort = hasPort; d->fport = fport; d->plen = plen;
  d->rssi = it.rssi; d->snrQ4 = it.snrQ4; d->sf = it.sf; d->freqKHz = it.freqKHz;
  d->lastMs = it.ms; d->count++; d->seq++;
}

void unl_service() {
  if (!g_rxq) return;
  static RxItem it;
  for (int i = 0; i < RXQ_LEN && xQueueReceive(g_rxq, &it, 0) == pdTRUE; i++) {
    if (it.kind == UNL_RX_WMBUS) processWmbus(it);
    else if (it.kind == UNL_RX_LORAWAN) processLorawan(it);
  }
}

// ============================================================================ record decoding helpers
// Human-readable (German) entity names for the stable record keys.
static String recordLabel(const wmb::Record &r) {
  static const char *DE[wmb::Q_COUNT_] = {
    "Wert", "Energie", "Volumen", "Masse", "Einschaltdauer", "Betriebsdauer", "Leistung", "Durchfluss",
    "Massenstrom", "Vorlauftemperatur", "Rücklauftemperatur", "Temperaturdifferenz", "Außentemperatur",
    "Druck", "Datum", "Zeitstempel", "Verbrauchseinheiten", "Mittelungsdauer", "Aktualität",
    "Fabrikationsnummer", "Erweiterte ID", "Busadresse", "Zugriffsnummer", "Medium", "Hersteller",
    "Parametersatz", "Modell/Version", "HW-Version", "FW-Version", "SW-Version", "Kundenstandort", "Kunde",
    "Fehlerflags", "Digitalausgang", "Digitaleingang", "Dimensionslos", "Spannung", "Strom",
    "Reset-Zähler", "Kumulationszähler", "Batterie-Restlaufzeit", "Luftfeuchte", "Herstellerdaten" };
  String s = r.quantity < wmb::Q_COUNT_ ? DE[r.quantity] : "Wert";
  if (r.quantity == wmb::Q_UNKNOWN) { char b[12]; snprintf(b, sizeof b, " %s%02X", r.vifeTable == 0xFD ? "FD" : r.vifeTable == 0xFB ? "FB" : "VIF ", r.vif); s += b; }
  static const char *FN[4] = { "", " max", " min", " (Fehler)" };
  s += FN[r.function & 3];
  if (r.storage == 1) s += " (Stichtag)";
  else if (r.storage) s += " [Speicher " + String(r.storage) + "]";
  if (r.tariff) s += " Tarif " + String(r.tariff);
  if (r.subunit) s += " Einheit " + String(r.subunit);
  return s;
}

static String recordValueJson(const wmb::Record &r) {
  char b[24];
  switch (r.kind) {
    case wmb::VK_NUMBER: return num(r.value);
    case wmb::VK_DATE: snprintf(b, sizeof b, "\"%04u-%02u-%02u\"", r.year, r.month, r.day); return b;
    case wmb::VK_DATETIME:
      snprintf(b, sizeof b, "\"%04u-%02u-%02uT%02u:%02u\"", r.year, r.month, r.day, r.hour, r.minute); return b;
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

// Collect the records of a device's latest telegram (de-duplicating keys: second "volume" -> "volume_2").
struct RecCollect {
  static const int MAX = 40;
  int n = 0;
  char keys[MAX][32];
  bool full = false;
  void (*each)(RecCollect &, const char *key, const wmb::Record &r) = nullptr;
  void *user = nullptr;
};
static void recSink(const wmb::Record &r, void *ctx) {
  RecCollect &c = *(RecCollect *)ctx;
  if (c.n >= RecCollect::MAX) { c.full = true; return; }
  char key[32];
  wmb::recordKey(r, key, sizeof key);
  int dup = 1;
  for (int i = 0; i < c.n; i++) {
    const char *k = c.keys[i];
    size_t kl = strlen(key);
    if (!strncmp(k, key, kl) && (k[kl] == 0 || (k[kl] == '_' && k[kl + 1] >= '2' && k[kl + 1] <= '9' && !k[kl + 2]))) dup++;
  }
  if (dup > 1) { size_t kl = strlen(key); if (kl < 29) snprintf(key + kl, 32 - kl, "_%d", dup > 9 ? 9 : dup); }
  strlcpy(c.keys[c.n++], key, 32);
  if (c.each) c.each(c, key, r);
}

static RecCollect g_rc;   // shared scratch for the (web-task-only) record walks below

// Re-decode a device's stored frame. Returns the DecryptResult; calls `each` for every record.
static int wmDecode(const WmDev &d, RecCollect &c, uint8_t *apOut = nullptr, size_t *apLenOut = nullptr) {
  if (!d.frameLen || d.truncated) return wmb::DR_MALFORMED;
  static wmb::Frame f;
  f.mode = d.mode;
  f.len = d.frameLen;
  memcpy(f.data, d.frame, d.frameLen);
  wmb::Header h;
  if (!wmb::parseHeader(f, h)) return wmb::DR_MALFORMED;
  static uint8_t ap[wmb::FRAME_MAX];
  size_t apLen = 0;
  int dr = wmb::decrypt(f, h, d.keySet ? d.key : nullptr, ap, sizeof ap, apLen);
  if (apOut && apLenOut) { memcpy(apOut, ap, apLen); *apLenOut = apLen; }
  if (dr == wmb::DR_OK || dr == wmb::DR_PLAIN) {
    // manufacturer-specific CI (0xA0..0xB7) carries no DIF/VIF records -> leave it to the raw view
    if (!(h.ci >= 0xA0 && h.ci <= 0xB7)) wmb::parseRecords(ap, apLen, recSink, &c);
  }
  return dr;
}

// ============================================================================ web API
static WebServer *g_srv = nullptr;

static String cfgJson() {
  UnlRadioCfg c;
  unl_radio_cfg(c);
  return String("{\"obi\":") + (c.obiOn ? "true" : "false") + ",\"wmb\":" + (c.wmbusOn ? "true" : "false") +
         ",\"lw\":" + (c.lorawanOn ? "true" : "false") + ",\"win\":" + c.obiWindowMs +
         ",\"lwf\":" + c.lorawanFreqKHz + ",\"lwsf\":" + c.lorawanSf + ",\"avh\":" + g_availHours + "}";
}

static String statsJson() {
  uint32_t tot = 0;
  for (int i = 0; i < UNL_SLOT_COUNT; i++) tot += g_st.slotMs[i];
  return String("{\"ms\":[") + g_st.slotMs[0] + "," + g_st.slotMs[1] + "," + g_st.slotMs[2] + "],\"ms_total\":" + tot +
         ",\"switches\":" + g_st.switches + ",\"switch_err\":" + g_st.switchErrors + ",\"defers\":" + g_st.defers +
         ",\"qdrop\":" + g_st.queueDrops + ",\"obi_frames\":" + g_st.obiFrames +
         ",\"obi_last_off\":" + g_st.obiLastOffset + ",\"obi_max_off\":" + g_st.obiMaxOffset +
         ",\"wm_frames\":" + g_st.wmFrames + ",\"wm_ok\":" + g_st.wmOk + ",\"wm_crc\":" + g_st.wmCrc +
         ",\"wm_noise\":" + (g_st.wm3of6 + g_st.wmOther) + ",\"lw_frames\":" + g_st.lwFrames +
         ",\"lw_ok\":" + g_st.lwOk + ",\"uptime\":" + (millis() / 1000) + "}";
}

static String wmSummaryJson(const WmDev &d) {
  char key[13];
  wmKey(d.mfr, d.id, key);
  char m[4];
  wmb::mfrCode(d.mfr, m);
  char idb[9];
  snprintf(idb, sizeof idb, "%08X", (unsigned)d.id);
  uint32_t age = d.lastMs ? (millis() - d.lastMs) / 1000 : 0;
  return String("{\"k\":\"") + key + "\",\"mfr\":\"" + m + "\",\"id\":\"" + idb + "\",\"name\":" + jstr(d.name) +
         ",\"type\":" + d.type + ",\"tname\":\"" + wmb::deviceTypeName(d.type) + "\",\"ver\":" + d.ver +
         ",\"mode\":\"" + wmb::modeName(d.mode) + "\",\"rssi\":" + d.rssi + ",\"count\":" + d.count +
         ",\"age\":" + (d.lastMs ? String(age) : String("null")) + ",\"ci\":" + d.ci + ",\"sec\":" + d.secMode +
         ",\"dec\":\"" + (d.lastMs ? wmb::decryptResultName(d.dec) : "") + "\",\"sel\":" + (d.selected ? "true" : "false") +
         ",\"key\":" + (d.keySet ? "true" : "false") + ",\"status\":" + d.status + "}";
}

static String lwSummaryJson(const LwDev &d) {
  char key[18];
  lwKey(d, key);
  uint32_t age = d.lastMs ? (millis() - d.lastMs) / 1000 : 0;
  String s = String("{\"k\":\"") + key + "\",\"name\":" + jstr(d.name) + ",\"join\":" + (d.join ? "true" : "false") +
             ",\"mtype\":\"" + lwMtypeName(d.mtype) + "\",\"fcnt\":" + d.fcnt +
             ",\"fport\":" + (d.hasPort ? String(d.fport) : String("null")) + ",\"plen\":" + d.plen +
             ",\"rssi\":" + d.rssi + ",\"snr\":" + num(d.snrQ4 / 4.0) + ",\"sf\":" + d.sf +
             ",\"freq\":" + num(d.freqKHz / 1000.0) + ",\"count\":" + d.count +
             ",\"age\":" + (d.lastMs ? String(age) : String("null")) + ",\"sel\":" + (d.selected ? "true" : "false") + "}";
  return s;
}

static void handleList() {
  WebServer &s = *g_srv;
  s.setContentLength(CONTENT_LENGTH_UNKNOWN);
  s.send(200, "application/json", "");
  String chunk = "{\"cfg\":" + cfgJson() + ",\"stats\":" + statsJson() + ",\"wm\":[";
  bool first = true;
  for (auto &d : g_wm) {
    if (!d.used) continue;
    if (!first) chunk += ",";
    first = false;
    chunk += wmSummaryJson(d);
    if (chunk.length() > 1200) { s.sendContent(chunk); chunk = ""; }
  }
  chunk += "],\"lw\":[";
  first = true;
  for (auto &d : g_lw) {
    if (!d.used) continue;
    if (!first) chunk += ",";
    first = false;
    chunk += lwSummaryJson(d);
    if (chunk.length() > 1200) { s.sendContent(chunk); chunk = ""; }
  }
  chunk += "]}";
  s.sendContent(chunk);
  s.sendContent("");
}

static WmDev *wmByKey(const String &k) {
  for (auto &d : g_wm) {
    if (!d.used) continue;
    char key[13];
    wmKey(d.mfr, d.id, key);
    if (k.equalsIgnoreCase(key)) return &d;
  }
  return nullptr;
}
static LwDev *lwByKey(const String &k) {
  for (auto &d : g_lw) {
    if (!d.used) continue;
    char key[18];
    lwKey(d, key);
    if (k.equalsIgnoreCase(key)) return &d;
  }
  return nullptr;
}

struct DetailCtx { String *out; bool first; };
static void detailEach(RecCollect &c, const char *key, const wmb::Record &r) {
  DetailCtx &x = *(DetailCtx *)c.user;
  const wmb::QuantityInfo &qi = wmb::quantityInfo(r.quantity);
  if (!x.first) *x.out += ",";
  x.first = false;
  *x.out += String("{\"k\":\"") + key + "\",\"l\":" + jstr(recordLabel(r).c_str()) + ",\"v\":" + recordValueJson(r) +
            ",\"u\":" + jstr(qi.unit) + ",\"raw\":\"" + hexStr(r.raw, r.rawLen) + "\",\"dif\":" + r.dif +
            ",\"vif\":" + r.vif + "}";
}

static void handleDetail() {
  WebServer &s = *g_srv;
  WmDev *d = wmByKey(s.arg("k"));
  if (!d) { s.send(404, "application/json", "{\"ok\":false}"); return; }
  String out = "{\"dev\":" + wmSummaryJson(*d) + ",\"frame\":\"" + hexStr(d->frame, d->frameLen) + "\"";
  RecCollect &c = g_rc;
  c = RecCollect();
  DetailCtx x{ &out, true };
  c.each = detailEach;
  c.user = &x;
  static uint8_t ap[wmb::FRAME_MAX];
  size_t apLen = 0;
  out += ",\"rec\":[";
  int dr = wmDecode(*d, c, ap, &apLen);
  out += "],\"dec\":\"" + String(d->frameLen ? wmb::decryptResultName(dr) : "") + "\"";
  if (dr == wmb::DR_OK) out += ",\"plain\":\"" + hexStr(ap, apLen) + "\"";
  out += "}";
  s.send(200, "application/json", out);
}

static void handleLog() {
  WebServer &s = *g_srv;
  uint32_t since = s.arg("since").toInt();
  String out = "{\"seq\":" + String(g_logSeq) + ",\"now\":" + millis() + ",\"e\":[";
  bool first = true;
  for (int i = 0; i < LOG_N; i++) {
    const LogEntry &e = g_log[(g_logHead + i) % LOG_N];   // oldest first
    if (!e.seq || e.seq <= since) continue;
    if (!first) out += ",";
    first = false;
    out += String("{\"s\":") + e.seq + ",\"t\":" + e.ms + ",\"kind\":" + e.kind + ",\"m\":" + e.mode +
           ",\"r\":" + e.result + ",\"rssi\":" + e.rssi + ",\"len\":" + e.len + ",\"who\":" + jstr(e.who) +
           ",\"b\":\"" + hexStr(e.hex, e.hexLen) + "\"}";
  }
  out += "]}";
  s.send(200, "application/json", out);
}

static bool g_mqttDirty = false;   // a selection / name change -> re-announce in the next MQTT pass

static void handleWmSet() {
  WebServer &s = *g_srv;
  WmDev *d = wmByKey(s.arg("k"));
  if (!d) { s.send(404, "application/json", "{\"ok\":false,\"err\":\"unknown device\"}"); return; }
  if (s.hasArg("forget")) {
    bool wasSel = d->selected;
    d->selected = false; d->keySet = false; memset(d->key, 0, 16); d->name[0] = 0;
    if (wasSel) { d->avail = -2; d->drop = true; g_mqttDirty = true; }   // clear its HA device first
    else d->used = false;
    saveDevices();
    s.send(200, "application/json", "{\"ok\":true}");
    return;
  }
  if (s.hasArg("key")) {
    String k = s.arg("key");
    if (!k.length()) { d->keySet = false; memset(d->key, 0, 16); }
    else if (parseHex(k, d->key, 16)) d->keySet = true;
    else { s.send(400, "application/json", "{\"ok\":false,\"err\":\"key must be 32 hex digits\"}"); return; }
    d->discDone = false; d->seq++;               // re-decode + re-publish with the new key
  }
  if (s.hasArg("name")) { strlcpy(d->name, s.arg("name").c_str(), sizeof d->name); d->discDone = false; d->seq++; }
  if (s.hasArg("sel")) {
    bool on = s.arg("sel") == "1" || s.arg("sel") == "true";
    if (on != d->selected) {
      d->selected = on;
      d->discDone = false;
      d->avail = on ? -1 : -2;
      d->seq++;
    }
  }
  g_mqttDirty = true;
  saveDevices();
  s.send(200, "application/json", "{\"ok\":true,\"dev\":" + wmSummaryJson(*d) + "}");
}

static void handleLwSet() {
  WebServer &s = *g_srv;
  LwDev *d = lwByKey(s.arg("k"));
  if (!d) { s.send(404, "application/json", "{\"ok\":false}"); return; }
  if (s.hasArg("forget")) {
    d->name[0] = 0;
    if (d->selected) { d->selected = false; d->avail = -2; d->drop = true; } else d->used = false;
    g_mqttDirty = true;
    saveDevices();
    s.send(200, "application/json", "{\"ok\":true}");
    return;
  }
  if (s.hasArg("name")) { strlcpy(d->name, s.arg("name").c_str(), sizeof d->name); d->discDone = false; d->seq++; }
  if (s.hasArg("sel")) {
    bool on = s.arg("sel") == "1" || s.arg("sel") == "true";
    if (on != d->selected) { d->selected = on; d->discDone = false; d->avail = on ? -1 : -2; d->seq++; }
  }
  g_mqttDirty = true;
  saveDevices();
  s.send(200, "application/json", "{\"ok\":true,\"dev\":" + lwSummaryJson(*d) + "}");
}

static void handleCfg() {
  WebServer &s = *g_srv;
  UnlRadioCfg c;
  unl_radio_cfg(c);
  auto flag = [&](const char *n, bool &v) { if (s.hasArg(n)) v = s.arg(n) == "1" || s.arg(n) == "true"; };
  flag("obi", c.obiOn);
  flag("wmb", c.wmbusOn);
  flag("lw", c.lorawanOn);
  if (s.hasArg("win")) { long w = s.arg("win").toInt(); if (w >= 100 && w <= 900) c.obiWindowMs = (uint16_t)w; }
  if (s.hasArg("lwf")) { long f = s.arg("lwf").toInt(); if (f >= 863000 && f <= 870000) c.lorawanFreqKHz = (uint32_t)f; }
  if (s.hasArg("lwsf")) { long sf = s.arg("lwsf").toInt(); if (sf >= 7 && sf <= 12) c.lorawanSf = (uint8_t)sf; }
  if (s.hasArg("avh")) { long h = s.arg("avh").toInt(); if (h >= 1 && h <= 720) g_availHours = (uint16_t)h; }
  if (!c.obiOn && !c.wmbusOn && !c.lorawanOn) c.obiOn = true;   // the radio has to do *something*
  portENTER_CRITICAL(&g_cfgMux);
  g_cfg = c;
  portEXIT_CRITICAL(&g_cfgMux);
  saveCfg();
  for (auto &d : g_wm) d.avail = d.selected ? -1 : d.avail;   // re-evaluate availability with the new timeout
  s.send(200, "application/json", "{\"ok\":true,\"cfg\":" + cfgJson() + "}");
}

static void handleStatsReset() {
  for (int i = 0; i < UNL_SLOT_COUNT; i++) g_st.slotMs[i] = 0;
  g_st.obiMaxOffset = 0;
  g_srv->send(200, "application/json", "{\"ok\":true}");
}

static void handlePage() {
#ifdef UNL_HAVE_PAGE
  sendGzipHtml(*g_srv, METERS_HTML_GZ);
#else
  g_srv->send(500, "text/plain", "meters page not embedded");
#endif
}

void unl_register_routes(WebServer &srv, WebServer::THandlerFunction (*guard)(WebServer::THandlerFunction)) {
  g_srv = &srv;
  srv.on("/meters", HTTP_GET, guard(handlePage));
  srv.on("/api/unl", HTTP_GET, guard(handleList));
  srv.on("/api/unl/wm", HTTP_GET, guard(handleDetail));
  srv.on("/api/unl/wm", HTTP_POST, guard(handleWmSet));
  srv.on("/api/unl/lw", HTTP_POST, guard(handleLwSet));
  srv.on("/api/unl/log", HTTP_GET, guard(handleLog));
  srv.on("/api/unl/cfg", HTTP_POST, guard(handleCfg));
  srv.on("/api/unl/stats/reset", HTTP_POST, guard(handleStatsReset));
}

// ============================================================================ MQTT / Home Assistant
static bool publishBig(PubSubClient &m, const String &topic, const String &payload, bool retain, uint32_t &cnt) {
  // beginPublish/endPublish streams past PubSubClient's fixed buffer -- discovery configs can be a few KB
  if (!m.beginPublish(topic.c_str(), payload.length(), retain)) return false;
  m.write((const uint8_t *)payload.c_str(), payload.length());
  bool ok = m.endPublish();
  if (ok) cnt++;
  return ok;
}

static String haDevBlock(const char *uid, const String &name, const char *mf, const char *model, const String &gwUid) {
  return String("\"dev\":{\"ids\":[\"") + uid + "\"],\"name\":" + jstr(name.c_str()) + ",\"mf\":" + jstr(mf) +
         ",\"mdl\":" + jstr(model) + ",\"via_device\":\"" + gwUid + "\"},\"o\":{\"name\":\"ObiUnlocked\",\"sw\":\"" +
         FW_VERSION + "\",\"url\":\"https://forgejo.tzru.de/tzru/ObiUnlocked\"}";
}

static String haSensor(const String &uid, const char *objKey, const String &name, const String &tpl, const char *unit,
                       const char *devClass, const char *stateClass, bool diagnostic) {
  String s = String("\"") + objKey + "\":{\"p\":\"sensor\",\"uniq_id\":\"" + uid + "_" + objKey + "\",\"name\":" +
             jstr(name.c_str()) + ",\"val_tpl\":" + jstr(tpl.c_str());
  if (unit && unit[0]) s += ",\"unit_of_meas\":" + jstr(unit);
  if (devClass) s += String(",\"dev_cla\":\"") + devClass + "\"";
  if (stateClass) s += String(",\"stat_cla\":\"") + stateClass + "\"";
  if (diagnostic) s += ",\"ent_cat\":\"diagnostic\"";
  return s + "}";
}

static const char *volumeClassFor(uint8_t type) {
  switch (type) {
    case 0x03: return "gas";
    case 0x06: case 0x07: case 0x15: case 0x16: case 0x17: case 0x04: case 0x0C: case 0x0D: return "water";
    default: return "volume";
  }
}

struct DiscCtx { String *cmps; String uid; uint8_t type; uint32_t hash; };
static void discEach(RecCollect &c, const char *key, const wmb::Record &r) {
  DiscCtx &x = *(DiscCtx *)c.user;
  for (const char *k = key; *k; k++) x.hash = x.hash * 31 + (uint8_t)*k;
  if (r.quantity == wmb::Q_MFR_SPECIFIC || r.kind == wmb::VK_HEX) return;   // raw blobs: no HA entity
  const wmb::QuantityInfo &qi = wmb::quantityInfo(r.quantity);
  const char *dc = qi.haClass;
  const char *sc = qi.haState;
  if (r.quantity == wmb::Q_VOLUME) dc = volumeClassFor(x.type);
  if (r.kind != wmb::VK_NUMBER) sc = nullptr;
  if (r.storage) sc = (r.kind == wmb::VK_NUMBER) ? "measurement" : nullptr;   // historic values are no running totals
  if (r.kind == wmb::VK_DATETIME) dc = nullptr;
  if (r.kind == wmb::VK_TEXT) { dc = nullptr; sc = nullptr; }
  bool diag = r.quantity >= wmb::Q_FABRICATION_NO && r.quantity <= wmb::Q_CUSTOMER;
  *x.cmps += "," + haSensor(x.uid, key, recordLabel(r), String("{{ value_json.v.") + key + " }}",
                            r.kind == wmb::VK_NUMBER ? qi.unit : "", dc, sc, diag);
}

static String wmStateTopic(const String &base, const char *key) { return base + "/wmbus/" + key; }
static String lwStateTopic(const String &base, const char *key) { return base + "/lorawan/" + key; }

struct StateCtx { String *out; bool first; };
static void stateEach(RecCollect &c, const char *key, const wmb::Record &r) {
  StateCtx &x = *(StateCtx *)c.user;
  if (!x.first) *x.out += ",";
  x.first = false;
  *x.out += String("\"") + key + "\":" + recordValueJson(r);
}

static void wmPublish(PubSubClient &m, WmDev &d, const String &base, const String &gwAvt, const String &gwUid,
                      uint32_t &cnt) {
  char key[13];
  wmKey(d.mfr, d.id, key);
  String st = wmStateTopic(base, key);
  String uid = String("obiu_wm_") + key;
  String cfgTopic = "homeassistant/device/" + uid + "/config";
  char mf[4];
  wmb::mfrCode(d.mfr, mf);

  if (d.avail == -2) {   // deselected / forgotten: remove the HA device and the retained topics
    publishBig(m, cfgTopic, "", true, cnt);
    publishBig(m, st, "", true, cnt);
    publishBig(m, st + "/status", "", true, cnt);
    d.avail = -1; d.discDone = false;
    if (d.drop) d.used = false;
    return;
  }
  if (!d.selected) return;

  // availability
  bool online = d.lastMs && (millis() - d.lastMs) < (uint32_t)g_availHours * 3600000UL;
  if (d.avail != (online ? 1 : 0)) {
    publishBig(m, st + "/status", online ? "online" : "offline", true, cnt);
    d.avail = online ? 1 : 0;
  }
  bool fresh = d.seq != d.pubSeq;
  if (!fresh && d.discDone) return;
  if (!d.frameLen) {
    if (!d.discDone) d.discDone = true;   // nothing heard since boot -- announce once we know the records
    return;
  }

  RecCollect &c = g_rc;
  // discovery (only when the record set / name / key changed)
  c = RecCollect();
  String cmps;
  DiscCtx dx{ &cmps, uid, d.type, 0 };
  c.each = discEach;
  c.user = &dx;
  int dr = wmDecode(d, c);
  for (const char *p = d.name; *p; p++) dx.hash = dx.hash * 31 + (uint8_t)*p;
  dx.hash = dx.hash * 31 + (uint32_t)dr;
  if (!d.discDone || dx.hash != d.discHash) {
    String devName = d.name[0] ? String(d.name) : String(mf) + " " + key + 3;
    String dev = haDevBlock(uid.c_str(), devName, mf, wmb::deviceTypeName(d.type), gwUid);
    String body = "{" + dev + ",\"stat_t\":\"" + st + "\",\"avty\":[{\"t\":\"" + gwAvt + "\"},{\"t\":\"" + st +
                  "/status\"}],\"avty_mode\":\"all\",\"cmps\":{" +
                  haSensor(uid, "rssi", "RSSI", "{{ value_json.rssi }}", "dBm", "signal_strength", "measurement", true) +
                  "," + haSensor(uid, "telegrams", "Telegramme", "{{ value_json.count }}", "", nullptr, "total_increasing", true) +
                  "," + haSensor(uid, "decryption", "Entschlüsselung", "{{ value_json.dec }}", "", nullptr, nullptr, true) +
                  (timeOk() ? "," + haSensor(uid, "last_seen", "Zuletzt gesehen", "{{ value_json.last_seen }}", "",
                                             "timestamp", nullptr, true) : String("")) +
                  cmps + "}}";
    if (publishBig(m, cfgTopic, body, true, cnt)) { d.discDone = true; d.discHash = dx.hash; }
  }
  // state
  c = RecCollect();
  String vals;
  StateCtx sx{ &vals, true };
  c.each = stateEach;
  c.user = &sx;
  dr = wmDecode(d, c);
  char idb[9];
  snprintf(idb, sizeof idb, "%08X", (unsigned)d.id);
  String last = isoAgo(d.lastMs);
  String body = String("{\"id\":\"") + idb + "\",\"mfr\":\"" + mf + "\",\"type\":\"" + wmb::deviceTypeKey(d.type) +
                "\",\"ver\":" + d.ver + ",\"mode\":\"" + wmb::modeName(d.mode) + "\",\"rssi\":" + d.rssi +
                ",\"count\":" + d.count + ",\"status\":" + d.status + ",\"dec\":\"" + wmb::decryptResultName(dr) + "\"" +
                (last.length() ? ",\"last_seen\":\"" + last + "\"" : String("")) + ",\"v\":{" + vals + "}}";
  if (publishBig(m, st, body, true, cnt)) d.pubSeq = d.seq;
}

static void lwPublish(PubSubClient &m, LwDev &d, const String &base, const String &gwAvt, const String &gwUid,
                      uint32_t &cnt) {
  char key[18];
  lwKey(d, key);
  String st = lwStateTopic(base, key);
  String uid = String("obiu_lw_") + key;
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
  if (d.avail != (online ? 1 : 0)) {
    publishBig(m, st + "/status", online ? "online" : "offline", true, cnt);
    d.avail = online ? 1 : 0;
  }
  if (!d.discDone) {
    String devName = d.name[0] ? String(d.name) : String("LoRaWAN ") + key;
    String body = "{" + haDevBlock(uid.c_str(), devName, "LoRaWAN", d.join ? "join request (DevEUI)" : "uplink (DevAddr)", gwUid) +
                  ",\"stat_t\":\"" + st + "\",\"avty\":[{\"t\":\"" + gwAvt + "\"},{\"t\":\"" + st +
                  "/status\"}],\"avty_mode\":\"all\",\"cmps\":{" +
                  haSensor(uid, "rssi", "RSSI", "{{ value_json.rssi }}", "dBm", "signal_strength", "measurement", false) +
                  "," + haSensor(uid, "snr", "SNR", "{{ value_json.snr }}", "dB", nullptr, "measurement", true) +
                  "," + haSensor(uid, "fcnt", "Frame-Zähler", "{{ value_json.fcnt }}", "", nullptr, "measurement", false) +
                  "," + haSensor(uid, "uplinks", "Empfangene Pakete", "{{ value_json.count }}", "", nullptr, "total_increasing", true) +
                  (timeOk() ? "," + haSensor(uid, "last_seen", "Zuletzt gesehen", "{{ value_json.last_seen }}", "",
                                             "timestamp", nullptr, false) : String("")) + "}}";
    if (publishBig(m, cfgTopic, body, true, cnt)) d.discDone = true;
  }
  if (d.seq == d.pubSeq || !d.lastMs) return;
  String last = isoAgo(d.lastMs);
  String body = String("{\"key\":\"") + key + "\",\"mtype\":\"" + lwMtypeName(d.mtype) + "\",\"fcnt\":" + d.fcnt +
                ",\"fport\":" + (d.hasPort ? String(d.fport) : String("null")) + ",\"len\":" + d.plen +
                ",\"rssi\":" + d.rssi + ",\"snr\":" + num(d.snrQ4 / 4.0) + ",\"sf\":" + d.sf +
                ",\"freq\":" + num(d.freqKHz / 1000.0) + ",\"count\":" + d.count +
                (last.length() ? ",\"last_seen\":\"" + last + "\"" : String("")) + "}";
  if (publishBig(m, st, body, true, cnt)) d.pubSeq = d.seq;
}

void unl_mqtt_reset() {
  for (auto &d : g_wm) { d.discDone = false; if (d.avail != -2) d.avail = -1; d.pubSeq = d.seq - 1; }
  for (auto &d : g_lw) { d.discDone = false; if (d.avail != -2) d.avail = -1; d.pubSeq = d.seq - 1; }
}

void unl_mqtt_service(PubSubClient &m, const String &base, const String &gwAvt, const String &gwUid, uint32_t &cnt) {
  // At most a few devices per pass so a burst (first connect, many selected meters) never stalls the web task.
  static int wi = 0, li = 0;
  static uint32_t lastAvailCheck = 0;
  int budget = 3;
  bool sweep = g_mqttDirty || millis() - lastAvailCheck > 30000;
  if (sweep) { g_mqttDirty = false; lastAvailCheck = millis(); }
  for (int n = 0; n < WM_MAX && budget > 0 && m.connected(); n++) {
    WmDev &d = g_wm[wi];
    wi = (wi + 1) % WM_MAX;
    if (!d.used || (!d.selected && d.avail != -2)) continue;
    bool needs = d.avail == -2 || d.seq != d.pubSeq || !d.discDone || sweep;
    if (!needs) continue;
    wmPublish(m, d, base, gwAvt, gwUid, cnt);
    budget--;
  }
  for (int n = 0; n < LW_MAX && budget > 0 && m.connected(); n++) {
    LwDev &d = g_lw[li];
    li = (li + 1) % LW_MAX;
    if (!d.used || (!d.selected && d.avail != -2)) continue;
    bool needs = d.avail == -2 || d.seq != d.pubSeq || !d.discDone || sweep;
    if (!needs) continue;
    lwPublish(m, d, base, gwAvt, gwUid, cnt);
    budget--;
  }
}
