// Host driver for wmbus_core (see tests/test_wmbus.py). AES comes from OpenSSL here.
//   wmbus_native radio <radio-hex> [key-hex]   decode bytes as delivered after the 0x543D sync word
//   wmbus_native cmac <key-hex> <msg-hex>      AES-CMAC (RFC 4493)
#include <openssl/aes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>
#include <vector>

#include "wmbus_core.h"

namespace wmb {
void aes128EncryptBlock(const uint8_t key[16], const uint8_t in[16], uint8_t out[16]) {
  AES_KEY k;
  AES_set_encrypt_key(key, 128, &k);
  AES_encrypt(in, out, &k);
}
void aes128DecryptBlock(const uint8_t key[16], const uint8_t in[16], uint8_t out[16]) {
  AES_KEY k;
  AES_set_decrypt_key(key, 128, &k);
  AES_decrypt(in, out, &k);
}
}   // namespace wmb

static std::vector<uint8_t> unhex(const char *s) {
  std::vector<uint8_t> v;
  for (size_t i = 0; s[i] && s[i + 1]; i += 2) {
    char b[3] = { s[i], s[i + 1], 0 };
    v.push_back((uint8_t)strtoul(b, nullptr, 16));
  }
  return v;
}

static void printRecord(const wmb::Record &r, void *) {
  char key[32];
  wmb::recordKey(r, key, sizeof key);
  printf("rec %s kind=%u ", key, r.kind);
  if (r.kind == wmb::VK_NUMBER) printf("value=%.10g unit=%s\n", r.value, wmb::quantityInfo(r.quantity).unit);
  else if (r.kind == wmb::VK_DATE) printf("date=%04u-%02u-%02u\n", r.year, r.month, r.day);
  else if (r.kind == wmb::VK_DATETIME)
    printf("datetime=%04u-%02u-%02uT%02u:%02u\n", r.year, r.month, r.day, r.hour, r.minute);
  else {
    printf("hex=");
    for (int i = 0; i < r.rawLen; i++) printf("%02x", r.raw[i]);
    printf("\n");
  }
}

int main(int argc, char **argv) {
  if (argc >= 4 && !strcmp(argv[1], "cmac")) {
    std::vector<uint8_t> key = unhex(argv[2]), msg = unhex(argv[3]);
    uint8_t mac[16];
    wmb::aesCmac(key.data(), msg.data(), msg.size(), mac);
    for (int i = 0; i < 16; i++) printf("%02x", mac[i]);
    printf("\n");
    return 0;
  }
  if (argc >= 3 && !strcmp(argv[1], "radio")) {
    std::vector<uint8_t> raw = unhex(argv[2]);
    std::vector<uint8_t> key = argc >= 4 ? unhex(argv[3]) : std::vector<uint8_t>();
    wmb::Frame f;
    int rr = wmb::fromRadio(raw.data(), raw.size(), f);
    printf("rr=%d mode=%s len=%u\n", rr, wmb::modeName(f.mode), f.len);
    if (rr != wmb::RR_OK) return 0;
    wmb::Header h;
    if (!wmb::parseHeader(f, h)) { printf("header=bad\n"); return 0; }
    char m[4];
    wmb::mfrCode(h.meterMfr(), m);
    printf("mfr=%s id=%08x ver=%u type=%u typekey=%s ci=%02x acc=%u status=%02x sec=%u blocks=%u\n", m,
           h.meterId(), h.meterVer(), h.meterType(), wmb::deviceTypeKey(h.meterType()), h.ci, h.acc,
           h.status, h.secMode, h.encBlocks);
    uint8_t ap[wmb::FRAME_MAX];
    size_t apLen;
    int dr = wmb::decrypt(f, h, key.size() == 16 ? key.data() : nullptr, ap, sizeof ap, apLen);
    printf("decrypt=%s\n", wmb::decryptResultName(dr));
    if (dr == wmb::DR_OK || dr == wmb::DR_PLAIN) printf("records=%d\n", wmb::parseRecords(ap, apLen, printRecord, nullptr));
    return 0;
  }
  fprintf(stderr, "usage: %s radio <hex> [key] | cmac <key> <msg>\n", argv[0]);
  return 2;
}
