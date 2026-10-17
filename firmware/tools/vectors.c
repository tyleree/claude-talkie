// Host-side test helper (built and run by mac/tests/test_firmware_compat.py). Uses the firmware's own
// headers so the Python side can check it decodes the stick's ADPCM bit-exactly
// and that both sides agree on packet layouts.
//
//   vectors sizes          -> prints "name size" for every protocol struct
//   vectors adpcm IN OUT   -> encodes raw int16 PCM from IN into packets in OUT:
//                             per packet: int16 predictor, u8 index, u16 n, bytes
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "talkie_adpcm.h"
#include "talkie_config.h"
#include "talkie_protocol.h"

#define SIZE(t) printf("%s %zu\n", #t, sizeof(t))

int main(int argc, char** argv) {
  if (argc >= 2 && strcmp(argv[1], "sizes") == 0) {
    SIZE(tk_hdr_t);
    SIZE(tk_hello_t);
    SIZE(tk_hello_ack_t);
    SIZE(tk_ping_t);
    SIZE(tk_pong_t);
    SIZE(tk_start_t);
    SIZE(tk_audio_hdr_t);
    SIZE(tk_end_t);
    SIZE(tk_nack_hdr_t);
    SIZE(tk_clip_ack_t);
    SIZE(tk_status_hdr_t);
    printf("samples_per_packet %d\n", TK_SAMPLES_PER_PACKET);
    return 0;
  }
  if (argc >= 4 && strcmp(argv[1], "adpcm") == 0) {
    FILE* in = fopen(argv[2], "rb");
    FILE* out = fopen(argv[3], "wb");
    if (!in || !out) return 1;
    int16_t buf[TK_SAMPLES_PER_PACKET];
    tk_adpcm_state_t st = {0, 0};
    size_t n;
    while ((n = fread(buf, sizeof(int16_t), TK_SAMPLES_PER_PACKET, in)) > 0) {
      uint8_t data[(TK_SAMPLES_PER_PACKET + 1) / 2];
      memset(data, 0, sizeof(data));
      int16_t pred = st.predictor;
      uint8_t idx = st.index;
      for (size_t i = 0; i < n; ++i) {
        uint8_t nib = tk_adpcm_encode(&st, buf[i]);
        if (i & 1)
          data[i >> 1] |= (uint8_t)(nib << 4);
        else
          data[i >> 1] = nib;
      }
      uint16_t n16 = (uint16_t)n;
      fwrite(&pred, 2, 1, out);
      fwrite(&idx, 1, 1, out);
      fwrite(&n16, 2, 1, out);
      fwrite(data, 1, (n + 1) / 2, out);
    }
    fclose(in);
    fclose(out);
    return 0;
  }
  fprintf(stderr, "usage: vectors sizes | vectors adpcm IN OUT\n");
  return 2;
}
