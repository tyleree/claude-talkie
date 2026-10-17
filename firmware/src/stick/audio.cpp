// Mic capture, pre-roll and ADPCM encoding for the stick.
//
// M5Unified's mic task fills 20 ms buffers and calls mic_release_cb for each
// one; we immediately queue another buffer so capture never gaps, and hand
// the filled one to audio_task. When a clip is recording, audio_task encodes
// straight into the clip store (PSRAM), one complete radio packet at a time.
// When idle it keeps the last TK_PREROLL_MS of audio for the next clip.
#include <M5Unified.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <math.h>

#include "app.h"
#include "talkie_adpcm.h"

namespace app {
namespace {

constexpr int kPreroll = TK_PREROLL_MS * TK_SAMPLE_RATE / 1000;
constexpr uint32_t kWarmupMaxMs = 1600;  // ES8311 can output zeros for ~1 s

int16_t s_frames[kNumFrames][kFrameSamples];
QueueHandle_t s_audio_q = nullptr;
volatile int s_next_frame = 0;
std::atomic<bool> s_running{false};
std::atomic<bool> s_ready{false};
std::atomic<uint8_t> s_level{0};
std::atomic<uint32_t> s_started_ms{0};
std::atomic<bool> s_preroll_reset{false};
std::atomic<uint32_t> s_last_frame_ms{0};
volatile uint32_t s_overruns = 0;

uint8_t* s_store = nullptr;
uint32_t s_capacity = 0;  // packets
std::atomic<uint32_t> s_published{0};
std::atomic<uint32_t> s_total_samples{0};
std::atomic<uint16_t> s_store_clip{0};  // which clip the store currently holds

std::atomic<bool> s_rec_req{false};
std::atomic<uint16_t> s_rec_clip{0};
std::atomic<uint16_t> s_flushed_clip{0};
std::atomic<uint16_t> s_full_clip{0};  // clip that filled the store (ids are never 0)

int16_t s_preroll[kPreroll > 0 ? kPreroll : 1];
int s_pr_head = 0;
int s_pr_count = 0;

struct Encoder {
  tk_adpcm_state_t st = {0, 0};
  uint32_t seq = 0;
  uint16_t n = 0;
  uint8_t* pkt = nullptr;
  uint32_t total = 0;
  uint16_t clip = 0;
  bool full = false;

  void reset(uint16_t c) {
    st.predictor = 0;
    st.index = 0;
    seq = 0;
    n = 0;
    pkt = nullptr;
    total = 0;
    clip = c;
    full = false;
  }

  void open_packet() {
    if (seq >= s_capacity) {
      full = true;
      return;
    }
    pkt = s_store + seq * kAudioPktBytes;
    tk_audio_hdr_t* h = (tk_audio_hdr_t*)pkt;
    h->h.magic = TK_MAGIC;
    h->h.type = TK_AUDIO;
    h->h.clip = clip;
    h->seq = (uint16_t)seq;
    h->predictor = st.predictor;
    h->step_index = st.index;
    h->reserved = 0;
    h->nsamples = 0;
    n = 0;
  }

  void close_packet() {
    ((tk_audio_hdr_t*)pkt)->nsamples = n;
    seq++;
    s_published.store(seq, std::memory_order_release);
    pkt = nullptr;
    n = 0;
  }

  void push(const int16_t* s, int count) {
    for (int i = 0; i < count; ++i) {
      if (!pkt) {
        open_packet();
        if (full) return;
      }
      uint8_t nib = tk_adpcm_encode(&st, s[i]);
      uint8_t* data = pkt + sizeof(tk_audio_hdr_t);
      if ((n & 1) == 0)
        data[n >> 1] = nib;
      else
        data[n >> 1] |= (uint8_t)(nib << 4);
      n++;
      total++;
      if (n == kSpp) close_packet();
    }
  }

  void flush() {
    if (pkt && n) close_packet();
    pkt = nullptr;
  }
};

Encoder s_enc;
bool s_recording = false;
uint16_t s_cur_clip = 0;

void preroll_push(const int16_t* s, int count) {
  if (kPreroll <= 0) return;
  for (int i = 0; i < count; ++i) {
    s_preroll[s_pr_head] = s[i];
    s_pr_head = (s_pr_head + 1) % kPreroll;
    if (s_pr_count < kPreroll) s_pr_count++;
  }
}

void preroll_drain_into_encoder() {
  if (kPreroll <= 0) return;
  int start = (s_pr_head - s_pr_count + kPreroll) % kPreroll;
  int done = 0;
  while (done < s_pr_count) {
    int pos = (start + done) % kPreroll;
    int chunk = s_pr_count - done;
    if (chunk > kPreroll - pos) chunk = kPreroll - pos;
    s_enc.push(&s_preroll[pos], chunk);
    done += chunk;
  }
  s_pr_count = 0;
}

void handle_transitions() {
  bool want = s_rec_req.load(std::memory_order_acquire);
  uint16_t want_clip = s_rec_clip.load(std::memory_order_acquire);
  if (s_recording && (!want || want_clip != s_cur_clip)) {
    s_enc.flush();
    s_total_samples.store(s_enc.total, std::memory_order_release);
    s_recording = false;
    s_flushed_clip.store(s_cur_clip, std::memory_order_release);
  }
  if (!s_recording && want) {
    s_recording = true;
    s_cur_clip = want_clip;
    // Invalidate the old clip before reusing its slots, then publish the new
    // id, so the link task never mixes packets of two clips.
    s_store_clip.store(0, std::memory_order_seq_cst);
    std::atomic_thread_fence(std::memory_order_seq_cst);  // no store writes before this point
    if (s_full_clip.load() == want_clip) s_full_clip.store(0);  // id reused after wrapping
    s_total_samples.store(0, std::memory_order_relaxed);
    s_published.store(0, std::memory_order_seq_cst);
    s_store_clip.store(want_clip, std::memory_order_seq_cst);
    s_enc.reset(want_clip);
    preroll_drain_into_encoder();
    s_total_samples.store(s_enc.total, std::memory_order_release);
  }
}

void audio_task(void*) {
  for (;;) {
    int idx = -1;
    bool got = xQueueReceive(s_audio_q, &idx, pdMS_TO_TICKS(30)) == pdTRUE;
    if (s_preroll_reset.exchange(false)) s_pr_count = 0;
    handle_transitions();
    if (!got || idx < 0 || idx >= kNumFrames) continue;

    const int16_t* s = s_frames[idx];
    int peak = 0;
    for (int i = 0; i < kFrameSamples; ++i) {
      int v = s[i] < 0 ? -s[i] : s[i];
      if (v > peak) peak = v;
    }
    int lvl = (int)(sqrtf((float)peak / 32768.0f) * 255.0f);
    if (lvl > 255) lvl = 255;
    uint8_t prev = s_level.load(std::memory_order_relaxed);
    s_level.store((uint8_t)(lvl > prev ? lvl : (prev * 3 + lvl) / 4), std::memory_order_relaxed);
    if (!s_ready.load(std::memory_order_relaxed)) {
      if (peak > 24 || millis() - s_started_ms.load() > kWarmupMaxMs) s_ready.store(true);
    }

    if (s_recording) {
      if (!s_enc.full) {
        s_enc.push(s, kFrameSamples);
        s_total_samples.store(s_enc.total, std::memory_order_release);
      }
      if (s_enc.full) s_full_clip.store(s_cur_clip, std::memory_order_release);
    } else {
      preroll_push(s, kFrameSamples);
    }
  }
}

// Called by M5Unified's mic task each time a buffer we queued is full.
void mic_release_cb(void*, void* data, size_t) {
  s_last_frame_ms.store(millis(), std::memory_order_relaxed);
  int idx = (int)(((int16_t*)data - &s_frames[0][0]) / kFrameSamples);
  if (idx >= 0 && idx < kNumFrames) {
    if (xQueueSend(s_audio_q, &idx, 0) != pdTRUE) s_overruns = s_overruns + 1;
  }
  int next = s_next_frame;
  s_next_frame = (next + 1) % kNumFrames;
  M5.Mic.record(s_frames[next], kFrameSamples);  // never blocks from here
}

}  // namespace

bool audio_init() {
  s_audio_q = xQueueCreate(kNumFrames, sizeof(int));

  // The clip store: every encoded packet of the current clip stays here until
  // the next clip, so the Mac can ask for any of them again.
  s_capacity = kMaxPackets;
  s_store = (uint8_t*)heap_caps_malloc((size_t)s_capacity * kAudioPktBytes,
                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!s_store) {  // PSRAM missing or not enabled: fall back to ~10 s in RAM
    s_capacity = (10 * TK_SAMPLE_RATE) / kSpp;
    s_store = (uint8_t*)malloc((size_t)s_capacity * kAudioPktBytes);
    Serial.println("audio: PSRAM unavailable, clips limited to ~10 s");
  }
  if (!s_store) return false;

  auto cfg = M5.Mic.config();
  cfg.sample_rate = TK_SAMPLE_RATE;
  cfg.magnification = TK_MIC_MAGNIFICATION;
  cfg.noise_filter_level = 0;
  cfg.task_priority = 4;
  M5.Mic.config(cfg);
  M5.Mic.setBufferReleaseCallback(nullptr, mic_release_cb);

  return xTaskCreatePinnedToCore(audio_task, "talkie_audio", 4096, nullptr, 3, nullptr, 1) == pdPASS;
}

uint32_t clip_capacity_seconds() { return (s_capacity * kSpp) / TK_SAMPLE_RATE; }

void mic_start() {
  if (s_running.load()) return;
  s_preroll_reset.store(true);
  s_ready.store(false);
  s_level.store(0);
  s_started_ms.store(millis());
  if (!M5.Mic.begin()) {
    Serial.println("audio: mic begin failed");
    return;
  }
  s_next_frame = 2;
  s_last_frame_ms.store(millis());
  M5.Mic.record(s_frames[0], kFrameSamples, TK_SAMPLE_RATE);
  M5.Mic.record(s_frames[1], kFrameSamples, TK_SAMPLE_RATE);
  s_running.store(true);
}

// The release callback keeps two buffers queued. If a record() from inside it
// is ever refused, the chain stops and the mic task sleeps with nothing to
// fill. Called from the main loop (the same context as mic_start/mic_stop, so
// it never races them): re-queue buffers, or restart the mic if that fails.
void mic_watchdog() {
  if (!s_running.load()) return;
  const uint32_t quiet = millis() - s_last_frame_ms.load(std::memory_order_relaxed);
  if (quiet < 400) return;
  if (quiet < 2000) {
    if (M5.Mic.isRecording()) return;  // buffers are queued; capture is just late
    Serial.println("audio: capture chain stopped, re-queuing buffers");
    int f = s_next_frame;
    s_next_frame = (f + 2) % kNumFrames;
    s_last_frame_ms.store(millis());
    M5.Mic.record(s_frames[f], kFrameSamples, TK_SAMPLE_RATE);
    M5.Mic.record(s_frames[(f + 1) % kNumFrames], kFrameSamples, TK_SAMPLE_RATE);
    return;
  }
  Serial.println("audio: no audio for 2 s, restarting the mic");
  mic_stop();
  mic_start();
}

void mic_stop() {
  if (!s_running.load()) return;
  s_preroll_reset.store(true);  // don't let stale audio leak into the next clip
  M5.Mic.end();
  s_running.store(false);
  s_ready.store(false);
  s_level.store(0);
}

bool mic_running() { return s_running.load(); }
bool mic_ready() { return s_running.load() && s_ready.load(); }
uint8_t mic_level() { return s_level.load(std::memory_order_relaxed); }

void rec_begin(uint16_t clip) {
  s_rec_clip.store(clip, std::memory_order_release);
  s_rec_req.store(true, std::memory_order_release);
}

void rec_stop() { s_rec_req.store(false, std::memory_order_release); }

bool rec_flushed(uint16_t clip) {
  return !s_rec_req.load(std::memory_order_acquire) &&
         s_flushed_clip.load(std::memory_order_acquire) == clip;
}

bool rec_full(uint16_t clip) { return s_full_clip.load(std::memory_order_acquire) == clip; }
uint32_t rec_samples() { return s_total_samples.load(std::memory_order_acquire); }
uint32_t rec_packets(uint16_t clip) {
  if (s_store_clip.load(std::memory_order_acquire) != clip) return 0;
  return s_published.load(std::memory_order_acquire);
}

// Copy out packet `seq` of `clip`. The audio task may start reusing the store
// for a newer clip at any moment, so check again after copying: if the store
// still holds this clip, the copy is intact. Returns the length, or 0.
size_t rec_copy_packet(uint16_t clip, uint32_t seq, uint8_t* out) {
  if (s_store_clip.load(std::memory_order_seq_cst) != clip) return 0;
  if (seq >= s_published.load(std::memory_order_acquire)) return 0;
  const uint8_t* p = s_store + seq * kAudioPktBytes;
  const tk_audio_hdr_t* h = (const tk_audio_hdr_t*)p;
  size_t len = sizeof(tk_audio_hdr_t) + (h->nsamples + 1) / 2;
  if (len > kAudioPktBytes) return 0;
  memcpy(out, p, len);
  std::atomic_thread_fence(std::memory_order_seq_cst);
  if (s_store_clip.load(std::memory_order_seq_cst) != clip) return 0;
  const tk_audio_hdr_t* c = (const tk_audio_hdr_t*)out;
  if (c->h.clip != clip || c->seq != (uint16_t)seq) return 0;
  return len;
}

}  // namespace app
