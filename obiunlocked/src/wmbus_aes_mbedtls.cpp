// wmbus_aes_mbedtls.cpp -- device implementation of the wmbus_core AES hook (mbedtls, HW-accelerated on ESP32).
// The host tests link their own OpenSSL-backed version instead (tests/wmbus_native.cpp).
#include "wmbus_core.h"

#include <mbedtls/aes.h>

namespace wmb {

void aes128EncryptBlock(const uint8_t key[16], const uint8_t in[16], uint8_t out[16]) {
  mbedtls_aes_context ctx;
  mbedtls_aes_init(&ctx);
  mbedtls_aes_setkey_enc(&ctx, key, 128);
  mbedtls_aes_crypt_ecb(&ctx, MBEDTLS_AES_ENCRYPT, in, out);
  mbedtls_aes_free(&ctx);
}

void aes128DecryptBlock(const uint8_t key[16], const uint8_t in[16], uint8_t out[16]) {
  mbedtls_aes_context ctx;
  mbedtls_aes_init(&ctx);
  mbedtls_aes_setkey_dec(&ctx, key, 128);
  mbedtls_aes_crypt_ecb(&ctx, MBEDTLS_AES_DECRYPT, in, out);
  mbedtls_aes_free(&ctx);
}

}   // namespace wmb
