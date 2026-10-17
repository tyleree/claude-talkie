// Key derivation and message authentication for Claude Talkie.
//
// Both devices are built with the same random secret (include/talkie_secret.h,
// generated on first build by scripts/gen_secret.py). From it we derive:
//   - the ESP-NOW PMK and LMK, so stick <-> receiver traffic is encrypted
//     (AES-CCMP) and nobody else can inject or overhear audio, and
//   - an HMAC key that authenticates the unencrypted pairing broadcasts, so
//     the stick only pairs with your receiver and vice versa.
#pragma once

#include <stdint.h>
#include <string.h>

#include "mbedtls/md.h"

#if __has_include("talkie_secret.h")
#include "talkie_secret.h"
#else
#error "include/talkie_secret.h is missing: build with PlatformIO from the firmware folder (scripts/gen_secret.py creates it)."
#endif

typedef struct {
  uint8_t pmk[16];
  uint8_t lmk[16];
  uint8_t hmac_key[32];
} tk_keys_t;

static inline void tk_sha256_labeled(const char* label, const char* secret, uint8_t out[32]) {
  const mbedtls_md_info_t* info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  mbedtls_md_context_t ctx;
  mbedtls_md_init(&ctx);
  mbedtls_md_setup(&ctx, info, 0);
  mbedtls_md_starts(&ctx);
  mbedtls_md_update(&ctx, (const unsigned char*)label, strlen(label));
  mbedtls_md_update(&ctx, (const unsigned char*)secret, strlen(secret));
  mbedtls_md_finish(&ctx, out);
  mbedtls_md_free(&ctx);
}

static inline void tk_keys_derive(tk_keys_t* k) {
  uint8_t h[32];
  tk_sha256_labeled("talkie-pmk:", TK_SECRET, h);
  memcpy(k->pmk, h, 16);
  tk_sha256_labeled("talkie-lmk:", TK_SECRET, h);
  memcpy(k->lmk, h, 16);
  tk_sha256_labeled("talkie-hmac:", TK_SECRET, k->hmac_key);
}

// HMAC-SHA256(key, data || sender_mac) truncated to 8 bytes. Including the
// sender's MAC stops a captured packet being replayed from another radio.
static inline void tk_hmac8(const tk_keys_t* k, const void* data, size_t len,
                            const uint8_t sender_mac[6], uint8_t out[8]) {
  const mbedtls_md_info_t* info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  mbedtls_md_context_t ctx;
  uint8_t full[32];
  mbedtls_md_init(&ctx);
  mbedtls_md_setup(&ctx, info, 1);
  mbedtls_md_hmac_starts(&ctx, k->hmac_key, sizeof(k->hmac_key));
  mbedtls_md_hmac_update(&ctx, (const unsigned char*)data, len);
  mbedtls_md_hmac_update(&ctx, sender_mac, 6);
  mbedtls_md_hmac_finish(&ctx, full);
  mbedtls_md_free(&ctx);
  memcpy(out, full, 8);
}

static inline bool tk_hmac8_check(const tk_keys_t* k, const void* data, size_t len,
                                  const uint8_t sender_mac[6], const uint8_t expected[8]) {
  uint8_t got[8];
  tk_hmac8(k, data, len, sender_mac, got);
  uint8_t diff = 0;
  for (int i = 0; i < 8; ++i) diff |= (uint8_t)(got[i] ^ expected[i]);
  return diff == 0;
}
