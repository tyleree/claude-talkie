// Claude Talkie wire protocol (version 1).
//
// Every radio packet is at most 250 bytes (the ESP-NOW limit), little-endian,
// and starts with tk_hdr_t. The receiver dongle forwards stick packets to the
// Mac verbatim (base64 over USB serial), so mac/talkie/protocol.py mirrors
// these layouts exactly. Change one, change both, and bump TK_PROTO_VERSION.
#pragma once

#include <stdint.h>
#include <stddef.h>

#define TK_MAGIC 0xC7
#define TK_PROTO_VERSION 1
#define TK_MAX_PAYLOAD 250

// Packet types
#define TK_HELLO 0x01      // stick -> broadcast   pairing / link check (HMAC)
#define TK_HELLO_ACK 0x02  // receiver -> broadcast reply to HELLO (HMAC)
#define TK_PING 0x03       // stick -> receiver    keepalive (encrypted)
#define TK_PONG 0x04       // receiver -> stick    keepalive reply
#define TK_START 0x10      // stick -> Mac         a clip begins
#define TK_AUDIO 0x11      // stick -> Mac         one ADPCM packet
#define TK_END 0x12        // stick -> Mac         clip complete (retried until acked)
#define TK_CANCEL 0x13     // stick -> Mac         clip abandoned
#define TK_NACK 0x20       // Mac -> stick         please resend these sequence numbers
#define TK_CLIP_ACK 0x21   // Mac -> stick         clip delivered
#define TK_STATUS 0x22     // Mac -> stick         progress / result text

// Roles in HELLO / HELLO_ACK
#define TK_ROLE_STICK 1
#define TK_ROLE_RECEIVER 2

// Clip targets (which button was used)
#define TK_TARGET_CURRENT 1  // button A: the agent you're looking at
#define TK_TARGET_NEW 2      // button B: a new background agent

// Codec ids
#define TK_CODEC_IMA_ADPCM 1

// END reasons
#define TK_END_RELEASED 0
#define TK_END_MAX_LENGTH 1
#define TK_END_LATCHED 2

// CLIP_ACK status
#define TK_ACK_COMPLETE 0
#define TK_ACK_PARTIAL 1   // some packets never arrived; gaps were filled with silence
#define TK_ACK_UNKNOWN 2   // Mac has no record of this clip (e.g. listener restarted)
#define TK_ACK_CANCELLED 3

// STATUS stages
#define TK_STAGE_RECEIVED 1
#define TK_STAGE_TRANSCRIBING 2
#define TK_STAGE_ROUTING 3
#define TK_STAGE_DONE 4   // text: "summary\ntranscript"
#define TK_STAGE_ERROR 5  // text: "reason\ndetail"
#define TK_STAGE_INFO 6

// STATUS flags
#define TK_STATUS_SOUND 0x01

// HELLO flags
#define TK_HELLO_FLAG_LR 0x01

#if defined(__cplusplus)
#define TK_STATIC_ASSERT(c, m) static_assert(c, m)
#else
#define TK_STATIC_ASSERT(c, m) _Static_assert(c, m)
#endif

#pragma pack(push, 1)

typedef struct {
  uint8_t magic;   // TK_MAGIC
  uint8_t type;    // TK_*
  uint16_t clip;   // clip id, 0 when not about a clip
} tk_hdr_t;

typedef struct {
  tk_hdr_t h;
  uint8_t proto;     // TK_PROTO_VERSION
  uint8_t role;      // TK_ROLE_STICK
  uint16_t fw;       // TK_FW_VERSION
  uint8_t nonce[8];  // fresh random value per HELLO
  uint8_t flags;     // TK_HELLO_FLAG_*
  uint8_t reserved;
  uint8_t hmac[8];   // HMAC-SHA256(key, packet-without-hmac || sender MAC)[0..8]
} tk_hello_t;

typedef struct {
  tk_hdr_t h;
  uint8_t proto;
  uint8_t role;          // TK_ROLE_RECEIVER
  uint16_t fw;
  uint8_t nonce[8];      // echo of the HELLO nonce
  uint8_t stick_mac[6];  // who this answer is for
  uint8_t channel;
  uint8_t host_alive;    // 1 if the Mac listener is talking to the receiver
  uint8_t hmac[8];
} tk_hello_ack_t;

typedef struct {
  tk_hdr_t h;
  uint8_t battery;  // 0-100, 255 = unknown
  uint8_t flags;    // bit0 charging, bit1 on USB power
} tk_ping_t;

typedef struct {
  tk_hdr_t h;
  uint8_t host_alive;
  int8_t rssi;      // stick signal as heard by the receiver (dBm), 0 = unknown
} tk_pong_t;

typedef struct {
  tk_hdr_t h;
  uint8_t target;               // TK_TARGET_*
  uint8_t codec;                // TK_CODEC_IMA_ADPCM
  uint16_t sample_rate;         // Hz
  uint16_t samples_per_packet;
  uint8_t battery;              // 0-100, 255 = unknown
  uint8_t flags;
} tk_start_t;

// Followed by (nsamples + 1) / 2 bytes of 4-bit IMA ADPCM, low nibble first.
// predictor/step_index are the decoder state *before* the first nibble, so
// every packet decodes on its own and a lost packet costs only its 28 ms.
typedef struct {
  tk_hdr_t h;
  uint16_t seq;        // 0-based packet number within the clip
  int16_t predictor;
  uint8_t step_index;  // 0..88
  uint8_t reserved;
  uint16_t nsamples;
} tk_audio_hdr_t;

typedef struct {
  tk_hdr_t h;
  uint16_t total_packets;
  uint32_t total_samples;
  uint8_t target;
  uint8_t reason;  // TK_END_*
} tk_end_t;

typedef struct {
  tk_hdr_t h;
} tk_cancel_t;

// Followed by `count` uint16 sequence numbers.
typedef struct {
  tk_hdr_t h;
  uint8_t count;
  uint8_t reserved;
} tk_nack_hdr_t;

typedef struct {
  tk_hdr_t h;
  uint8_t status;  // TK_ACK_*
  uint8_t reserved;
  uint16_t received;
  uint16_t total;
} tk_clip_ack_t;

// Followed by UTF-8 text (not NUL terminated); length = packet length - 6.
typedef struct {
  tk_hdr_t h;
  uint8_t stage;  // TK_STAGE_*
  uint8_t flags;  // TK_STATUS_*
} tk_status_hdr_t;

#pragma pack(pop)

#define TK_NACK_MAX ((TK_MAX_PAYLOAD - sizeof(tk_nack_hdr_t)) / 2)
#define TK_STATUS_TEXT_MAX (TK_MAX_PAYLOAD - sizeof(tk_status_hdr_t))

TK_STATIC_ASSERT(sizeof(tk_hdr_t) == 4, "hdr");
TK_STATIC_ASSERT(sizeof(tk_hello_t) == 26, "hello");
TK_STATIC_ASSERT(sizeof(tk_hello_ack_t) == 32, "hello_ack");
TK_STATIC_ASSERT(sizeof(tk_ping_t) == 6, "ping");
TK_STATIC_ASSERT(sizeof(tk_pong_t) == 6, "pong");
TK_STATIC_ASSERT(sizeof(tk_start_t) == 12, "start");
TK_STATIC_ASSERT(sizeof(tk_audio_hdr_t) == 12, "audio");
TK_STATIC_ASSERT(sizeof(tk_end_t) == 12, "end");
TK_STATIC_ASSERT(sizeof(tk_nack_hdr_t) == 6, "nack");
TK_STATIC_ASSERT(sizeof(tk_clip_ack_t) == 10, "clip_ack");
TK_STATIC_ASSERT(sizeof(tk_status_hdr_t) == 6, "status");
