// Host driver for wx_core + lorawan_core (see tests/test_radio_cores.py). AES comes from OpenSSL here.
//   radio_native wx <lacrosse|bresser> <hex>
//   radio_native lw <phy-hex> <nwkskey-hex> <appskey-hex> <fcnt-high>
#include <openssl/aes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vector>

#include "lorawan_core.h"
#include "wmbus_core.h"
#include "wx_core.h"

namespace wmb {
void aes128EncryptBlock(const uint8_t key[16], const uint8_t in[16], uint8_t out[16]) {
  AES_KEY k; AES_set_encrypt_key(key, 128, &k); AES_encrypt(in, out, &k);
}
void aes128DecryptBlock(const uint8_t key[16], const uint8_t in[16], uint8_t out[16]) {
  AES_KEY k; AES_set_decrypt_key(key, 128, &k); AES_decrypt(in, out, &k);
}
}   // namespace wmb

static std::vector<uint8_t> unhex(const char *s) {
  std::vector<uint8_t> v;
  for (size_t i = 0; s[i] && s[i + 1]; i += 2) { char b[3] = { s[i], s[i + 1], 0 }; v.push_back((uint8_t)strtoul(b, nullptr, 16)); }
  return v;
}
static void lpp(const char *key, const char *, double v, const char *unit, const char *, void *) {
  printf("lpp %s=%.6g %s\n", key, v, unit);
}

int main(int argc, char **argv) {
  if (argc >= 4 && !strcmp(argv[1], "wx")) {
    std::vector<uint8_t> d = unhex(argv[3]);
    wx::Reading r;
    bool ok = !strcmp(argv[2], "lacrosse") ? wx::decodeLaCrosse(d.data(), d.size(), r) : wx::decodeBresser(d.data(), d.size(), r);
    printf("ok=%d proto=%u id=%u ch=%u\n", ok, r.proto, (unsigned)r.id, r.channel);
    if (!ok) return 0;
    if (r.hasTemp) printf("temp=%.1f\n", r.tempC);
    if (r.hasHum) printf("hum=%u\n", r.hum);
    if (r.hasWind) printf("wavg=%.1f\nwgust=%.1f\nwdir=%.1f\n", r.windAvg, r.windGust, r.windDir);
    if (r.hasRain) printf("rain=%.1f\n", r.rainMm);
    if (r.hasUv) printf("uv=%.1f\n", r.uv);
    if (r.hasLight) printf("lux=%u\n", (unsigned)r.lux);
    if (r.hasBatt) printf("battlow=%d\n", r.battLow);
    printf("newbatt=%d\n", r.newBatt);
    return 0;
  }
  if (argc >= 6 && !strcmp(argv[1], "lw")) {
    std::vector<uint8_t> phy = unhex(argv[2]), nwk = unhex(argv[3]), app = unhex(argv[4]);
    lw::Uplink u;
    if (!lw::parse(phy.data(), phy.size(), u)) { printf("parse=0\n"); return 0; }
    printf("parse=1 mtype=%u devaddr=%08x fcnt16=%u port=%d\n", u.mtype, (unsigned)u.devaddr, u.fcnt16, u.hasPort ? u.fport : -1);
    uint8_t out[255];
    uint32_t f32 = 0;
    int n = lw::decodeUplink(phy.data(), phy.size(), u, nwk.data(), app.data(), (uint16_t)atoi(argv[5]), out, sizeof out, f32);
    printf("len=%d fcnt32=%u\n", n, (unsigned)f32);
    if (n > 0) {
      printf("payload=");
      for (int i = 0; i < n; i++) printf("%02x", out[i]);
      printf("\n");
      int c = lw::parseCayenne(out, n, lpp, nullptr);
      printf("lppcount=%d\n", c);
    }
    return 0;
  }
  fprintf(stderr, "usage\n");
  return 2;
}
