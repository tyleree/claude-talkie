// 4-bit IMA ADPCM (the classic DVI/IMA variant used by WAV files).
// Header-only so the exact same code runs on the stick and in host tests
// (firmware/test/host). mac/talkie/adpcm.py is the matching decoder.
#pragma once

#include <stdint.h>

typedef struct {
  int16_t predictor;
  uint8_t index;  // 0..88
} tk_adpcm_state_t;

static const int16_t tk_adpcm_steps[89] = {
    7,     8,     9,     10,    11,    12,    13,    14,    16,    17,    19,    21,    23,
    25,    28,    31,    34,    37,    41,    45,    50,    55,    60,    66,    73,    80,
    88,    97,    107,   118,   130,   143,   157,   173,   190,   209,   230,   253,   279,
    307,   337,   371,   408,   449,   494,   544,   598,   658,   724,   796,   876,   963,
    1060,  1166,  1282,  1411,  1552,  1707,  1878,  2066,  2272,  2499,  2749,  3024,  3327,
    3660,  4026,  4428,  4871,  5358,  5894,  6484,  7132,  7845,  8630,  9493,  10442, 11487,
    12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767};

static const int8_t tk_adpcm_index_adjust[16] = {-1, -1, -1, -1, 2, 4, 6, 8,
                                                 -1, -1, -1, -1, 2, 4, 6, 8};

static inline void tk_adpcm_advance(tk_adpcm_state_t* st, uint8_t nibble, int32_t diffq) {
  int32_t p = st->predictor;
  p += (nibble & 8) ? -diffq : diffq;
  if (p > 32767) p = 32767;
  if (p < -32768) p = -32768;
  st->predictor = (int16_t)p;
  int32_t idx = (int32_t)st->index + tk_adpcm_index_adjust[nibble & 15];
  if (idx < 0) idx = 0;
  if (idx > 88) idx = 88;
  st->index = (uint8_t)idx;
}

// Encode one sample, updating the state exactly as a decoder will.
static inline uint8_t tk_adpcm_encode(tk_adpcm_state_t* st, int16_t sample) {
  int32_t step = tk_adpcm_steps[st->index];
  int32_t diff = (int32_t)sample - st->predictor;
  uint8_t nibble = 0;
  if (diff < 0) {
    nibble = 8;
    diff = -diff;
  }
  int32_t diffq = step >> 3;
  if (diff >= step) {
    nibble |= 4;
    diff -= step;
    diffq += step;
  }
  step >>= 1;
  if (diff >= step) {
    nibble |= 2;
    diff -= step;
    diffq += step;
  }
  step >>= 1;
  if (diff >= step) {
    nibble |= 1;
    diffq += step;
  }
  tk_adpcm_advance(st, nibble, diffq);
  return nibble;
}

static inline int16_t tk_adpcm_decode(tk_adpcm_state_t* st, uint8_t nibble) {
  int32_t step = tk_adpcm_steps[st->index];
  int32_t diffq = step >> 3;
  if (nibble & 4) diffq += step;
  if (nibble & 2) diffq += step >> 1;
  if (nibble & 1) diffq += step >> 2;
  tk_adpcm_advance(st, nibble, diffq);
  return st->predictor;
}
