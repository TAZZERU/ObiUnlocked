// lorawan_core.h -- LoRaWAN 1.0.x uplink header parsing, MIC check and FRMPayload decryption with the
// device's own session keys (NwkSKey / AppSKey, e.g. from the TTN console), plus Cayenne LPP decoding.
// Portable (AES via the wmbus_core platform hook), host-tested by tests/test_radio_cores.py.
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace lw {

struct Uplink {
  uint8_t  mtype = 0;            // 2 unconfirmed, 4 confirmed data up; 0 join request
  uint32_t devaddr = 0;
  uint8_t  fctrl = 0;
  uint16_t fcnt16 = 0;
  bool     hasPort = false;
  uint8_t  fport = 0;
  uint8_t  payOff = 0, payLen = 0;   // FRMPayload inside the PHYPayload
  uint8_t  devEui[8] = {0};        // join request only (little-endian as sent)
};
bool parse(const uint8_t *phy, size_t n, Uplink &u);   // false = not an uplink we understand

// MIC over the whole PHYPayload (minus the 4 MIC bytes) with NwkSKey, uplink direction.
bool micOk(const uint8_t nwkSKey[16], const uint8_t *phy, size_t n, uint32_t devaddr, uint32_t fcnt32);
// FRMPayload en-/decryption (AES-CTR style, symmetric). dir 0 = uplink.
void crypt(const uint8_t key[16], uint32_t devaddr, uint32_t fcnt32, uint8_t dir, uint8_t *data, size_t n);

// Verify + decrypt one uplink. fcntHigh = upper 16 bits of the frame counter seen so far (the counter's
// upper half is not transmitted); fcntHigh and fcntHigh+1 are tried. Returns the payload length, or -1 if
// the MIC didn't match (wrong keys / not this device). fcnt32Out receives the full counter that matched.
int decodeUplink(const uint8_t *phy, size_t n, const Uplink &u, const uint8_t nwkSKey[16],
                 const uint8_t appSKey[16], uint16_t fcntHigh, uint8_t *out, size_t outMax, uint32_t &fcnt32Out);

// Cayenne LPP: [channel][type][data...]...  Calls sink once per value (multi-axis types: one call per axis).
typedef void (*LppSink)(const char *key, const char *label, double value, const char *unit, const char *devClass,
                        void *ctx);
int parseCayenne(const uint8_t *p, size_t n, LppSink sink, void *ctx);   // values reported, -1 = not LPP

}   // namespace lw
