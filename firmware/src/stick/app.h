// Claude Talkie stick - interfaces between the firmware modules.
//
//   audio.cpp  mic capture (M5Unified), pre-roll, ADPCM encoding into a clip
//              store in PSRAM. Runs its own FreeRTOS task.
//   link.cpp   ESP-NOW link to the receiver: pairing, streaming the clip,
//              resend on request, END/ack, status messages. Own task.
//   ui.cpp     screen drawing and beeps.
//   main.cpp   buttons, the push-to-talk state machine, power management.
#pragma once

#include <Arduino.h>

#include <atomic>

#include "talkie_config.h"
#include "talkie_protocol.h"

namespace app {

constexpr int kFrameSamples = 320;  // 20 ms mic buffers
constexpr int kNumFrames = 8;       // ring of mic buffers (160 ms of slack)
constexpr int kSpp = TK_SAMPLES_PER_PACKET;
constexpr int kAudioDataBytes = (kSpp + 1) / 2;
constexpr int kAudioPktBytes = (int)sizeof(tk_audio_hdr_t) + kAudioDataBytes;
constexpr uint32_t kMaxPackets = ((uint32_t)TK_MAX_CLIP_SECONDS * TK_SAMPLE_RATE + kSpp - 1) / kSpp;
static_assert(kAudioPktBytes <= TK_MAX_PAYLOAD, "audio packet larger than ESP-NOW allows");
static_assert(kMaxPackets < 65000, "clip too long for 16-bit sequence numbers");

// ------------------------------------------------------------------ audio --

bool audio_init();        // configure mic, allocate the clip store, start the task
uint32_t clip_capacity_seconds();
void mic_start();         // idempotent
void mic_stop();          // idempotent
bool mic_running();
bool mic_ready();         // codec warmed up and delivering real audio
void mic_watchdog();      // main loop: restart capture if buffers stopped arriving
uint8_t mic_level();      // 0..255, recent peak (perceptual scale)

void rec_begin(uint16_t clip);     // start encoding (pre-roll included)
void rec_stop();                   // stop; the audio task flushes the last packet
bool rec_flushed(uint16_t clip);   // stop processed for this clip
bool rec_full(uint16_t clip);       // this clip hit the length limit
uint32_t rec_samples();            // samples encoded so far
// Packets of `clip` completed so far (0 if the store holds a different clip).
uint32_t rec_packets(uint16_t clip);
// Copy stored packet `seq` of `clip` into `out` (kAudioPktBytes); returns its
// length, or 0 if it isn't available (not encoded yet, or the store moved on).
size_t rec_copy_packet(uint16_t clip, uint32_t seq, uint8_t* out);

// ------------------------------------------------------------------- link --

enum class EvType : uint8_t { ClipAck, ClipFailed, Status, Paired };

struct Event {
  EvType type;
  uint16_t clip;
  uint8_t a;   // ClipAck: status   Status: stage
  uint8_t b;   // Status: flags
  uint16_t c;  // ClipAck: received
  uint16_t d;  // ClipAck: total
  char text[TK_STATUS_TEXT_MAX + 1];
};

struct LinkInfo {
  bool radio_up;
  bool linked;         // receiver answered recently
  bool host_alive;     // Mac listener running (as reported by the receiver)
  bool have_receiver;  // paired at some point
  bool lr;             // long range active
  int8_t rssi;
  uint32_t tx_sent;    // packets of the current clip sent
  uint32_t tx_total;   // packets of the current clip (0 until known)
  uint32_t tx_activity;  // bumps on every packet delivered or resend request (stall detection)
};

bool link_init();
void link_want_radio(bool on);
void link_clip_start(uint16_t clip, uint8_t target);
void link_clip_end(uint16_t clip, uint16_t total_packets, uint32_t total_samples, uint8_t target,
                   uint8_t reason);
void link_clip_cancel(uint16_t clip);
void link_clip_retry(uint16_t clip);
void link_clip_abort(uint16_t clip);
void link_forget_receiver();
void link_set_battery(uint8_t pct, uint8_t flags);
bool link_poll_event(Event* ev);
LinkInfo link_info();

// --------------------------------------------------------------------- ui --

enum class Mode : uint8_t {
  Boot,
  Idle,
  Rec,         // holding the button
  RecPending,  // short tap: waiting to see if a second tap locks recording
  RecLocked,   // double-tapped: recording hands-free
  Finishing,   // button released, last audio being encoded
  Sending,     // streaming / waiting for the Mac to confirm
  Waiting,     // delivered; Mac is transcribing and routing
  Result,      // showing the outcome
  Failed,      // could not deliver; A retries, B discards
  Message,     // short notice
};

enum class Beep : uint8_t { Start, Sent, Ok, Error };

struct UiModel {
  Mode mode;
  uint8_t target;
  uint32_t rec_ms;
  uint8_t level;
  bool mic_ready;
  LinkInfo link;
  int battery;  // -1 unknown
  bool charging;
  bool usb;
  bool ok;
  uint32_t anim;  // ms timestamp for spinners
  const char* line1;
  const char* line2;
};

void ui_init();
void ui_splash(const char* l1, const char* l2);
void ui_render(const UiModel& m);
void ui_screen(bool on);
void ui_beep(Beep b);

}  // namespace app
