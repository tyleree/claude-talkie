// ESP-NOW link from the stick to the receiver dongle.
//
// One task owns the radio. It turns Wi-Fi on only when wanted (it costs far
// more power than everything else), finds the receiver with an authenticated
// HELLO broadcast, then streams the current clip: START, every AUDIO packet
// as soon as it is encoded, any packets the Mac asks for again (NACK), and
// END until the Mac acknowledges the clip. Status messages from the Mac are
// passed to the UI as events.
#include <Preferences.h>
#include <esp_mac.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <stddef.h>

#include "app.h"
#include "talkie_crypto.h"
#include "talkie_radio.h"

namespace app {
namespace {

enum class CmdType : uint8_t { Radio, Start, End, Cancel, Retry, Abort, Forget };

struct Cmd {
  CmdType type;
  bool on;
  uint8_t target;
  uint8_t reason;
  uint16_t clip;
  uint16_t total_packets;
  uint32_t total_samples;
};

struct RxItem {
  uint8_t mac[6];
  int8_t rssi;
  uint8_t len;
  uint8_t data[TK_MAX_PAYLOAD];
};

QueueHandle_t s_cmdq, s_evq, s_rxq;
SemaphoreHandle_t s_tx_done;
volatile bool s_tx_ok = false;
TaskHandle_t s_task = nullptr;

tk_keys_t s_keys;
Preferences s_prefs;
uint8_t s_my_mac[6];
uint8_t s_peer[6];
bool s_have_peer = false;

// link-task state
bool s_want = false;
bool s_up = false;
bool s_linked = false;
tk_radio_status_t s_rs = {false, false, 0, ""};
uint32_t s_radio_retry_ms = 0;
uint32_t s_last_hello = 0, s_last_ping = 0;
uint8_t s_nonce[8], s_prev_nonce[8];
int s_unanswered_pings = 0;
int s_fail_streak = 0;

std::atomic<bool> a_up{false}, a_linked{false}, a_host{false}, a_have{false}, a_lr{false};
std::atomic<int8_t> a_rssi{0};
std::atomic<uint32_t> a_sent{0}, a_total{0}, a_acts{0};
std::atomic<uint8_t> a_battery{255}, a_bflags{0};

struct TxClip {
  bool active = false;
  uint16_t clip = 0;
  uint8_t target = 0;
  bool start_sent = false;
  uint32_t next = 0;
  uint8_t pkt_fails = 0;
  bool end_known = false;
  uint16_t total = 0;
  uint32_t samples = 0;
  uint8_t reason = 0;
  uint32_t end_first = 0, end_last = 0;
  uint8_t restarts = 0;
  uint16_t resend[256];
  uint8_t rs_head = 0, rs_tail = 0;  // ring indexes wrap at 256

  void restart() {
    start_sent = false;
    next = 0;
    pkt_fails = 0;
    end_first = end_last = 0;
    rs_head = rs_tail = 0;
  }
} s_tx;

void post(const Event& ev) { xQueueSend(s_evq, &ev, 0); }

void on_rx(const uint8_t* mac, const uint8_t* data, int len, int8_t rssi) {
  if (len < (int)sizeof(tk_hdr_t) || len > TK_MAX_PAYLOAD) return;
  RxItem it;
  memcpy(it.mac, mac, 6);
  it.rssi = rssi;
  it.len = (uint8_t)len;
  memcpy(it.data, data, len);
  xQueueSend(s_rxq, &it, 0);
  if (s_task) xTaskNotifyGive(s_task);
}

void on_tx(bool ok) {
  s_tx_ok = ok;
  xSemaphoreGive(s_tx_done);
}

// Send and wait for the MAC-layer acknowledgement (broadcasts always "succeed").
bool tx_raw(const uint8_t* mac, const void* data, size_t len) {
  for (int attempt = 0; attempt < 3; ++attempt) {
    xSemaphoreTake(s_tx_done, 0);
    esp_err_t e = esp_now_send(mac, (const uint8_t*)data, len);
    if (e == ESP_ERR_ESPNOW_NO_MEM) {
      vTaskDelay(pdMS_TO_TICKS(2));
      continue;
    }
    if (e != ESP_OK) return false;
    if (xSemaphoreTake(s_tx_done, pdMS_TO_TICKS(60)) == pdTRUE && s_tx_ok) return true;
    vTaskDelay(pdMS_TO_TICKS(2));
  }
  return false;
}

bool tx_peer(const void* data, size_t len) {
  if (!s_have_peer) return false;
  bool ok = tx_raw(s_peer, data, len);
  if (ok) {
    s_fail_streak = 0;
  } else if (++s_fail_streak >= 6) {
    s_linked = false;  // receiver gone; go back to HELLO
    s_fail_streak = 0;
  }
  return ok;
}

void save_peer() { s_prefs.putBytes("rx", s_peer, 6); }

void radio_on() {
  s_rs = tk_radio_start(&s_keys);
  a_lr = s_rs.lr;
  if (!s_rs.ok) {
    Serial.printf("link: radio start failed at %s (%d)\n", s_rs.where, s_rs.err);
    tk_radio_stop();
    s_radio_retry_ms = millis() + 2000;
    return;
  }
  if (s_rs.err) Serial.printf("link: radio warning at %s (%d)\n", s_rs.where, s_rs.err);
  if (s_have_peer) tk_add_peer(s_peer, &s_keys);
  RxItem junk;
  while (xQueueReceive(s_rxq, &junk, 0) == pdTRUE) {
  }
  s_up = true;
  s_linked = false;
  s_last_hello = 0;
  s_unanswered_pings = 0;
  s_fail_streak = 0;
}

void radio_off() {
  tk_radio_stop();
  s_up = false;
  s_linked = false;
}

void send_hello(uint32_t now) {
  memcpy(s_prev_nonce, s_nonce, 8);
  esp_fill_random(s_nonce, 8);
  tk_hello_t m;
  memset(&m, 0, sizeof(m));
  tk_fill_hdr(&m.h, TK_HELLO, 0);
  m.proto = TK_PROTO_VERSION;
  m.role = TK_ROLE_STICK;
  m.fw = TK_FW_VERSION;
  memcpy(m.nonce, s_nonce, 8);
  m.flags = s_rs.lr ? TK_HELLO_FLAG_LR : 0;
  tk_hmac8(&s_keys, &m, offsetof(tk_hello_t, hmac), s_my_mac, m.hmac);
  tx_raw(tk_broadcast_mac, &m, sizeof(m));
  s_last_hello = now;
}

void send_ping(uint32_t now) {
  s_last_ping = now;
  if (s_unanswered_pings >= 3) {
    s_linked = false;
    s_unanswered_pings = 0;
    return;
  }
  tk_ping_t m;
  tk_fill_hdr(&m.h, TK_PING, 0);
  m.battery = a_battery.load();
  m.flags = a_bflags.load();
  tx_peer(&m, sizeof(m));
  s_unanswered_pings++;
}

void handle_hello_ack(const RxItem& it) {
  if (it.len != sizeof(tk_hello_ack_t)) return;
  const tk_hello_ack_t* m = (const tk_hello_ack_t*)it.data;
  if (m->proto != TK_PROTO_VERSION || m->role != TK_ROLE_RECEIVER) return;
  if (memcmp(m->stick_mac, s_my_mac, 6) != 0) return;
  if (memcmp(m->nonce, s_nonce, 8) != 0 && memcmp(m->nonce, s_prev_nonce, 8) != 0) return;
  if (!tk_hmac8_check(&s_keys, m, offsetof(tk_hello_ack_t, hmac), it.mac, m->hmac)) return;

  if (!s_have_peer || memcmp(s_peer, it.mac, 6) != 0) {
    if (s_have_peer) esp_now_del_peer(s_peer);
    memcpy(s_peer, it.mac, 6);
    s_have_peer = true;
    a_have = true;
    save_peer();
    Event ev = {};
    ev.type = EvType::Paired;
    post(ev);
  }
  tk_add_peer(s_peer, &s_keys);
  s_linked = true;
  s_unanswered_pings = 0;
  s_fail_streak = 0;
  a_host = m->host_alive != 0;
}

void handle_rx(const RxItem& it) {
  const tk_hdr_t* h = (const tk_hdr_t*)it.data;
  if (h->magic != TK_MAGIC) return;
  if (h->type == TK_HELLO_ACK) {
    handle_hello_ack(it);
    return;
  }
  if (!s_have_peer || memcmp(it.mac, s_peer, 6) != 0) return;
  s_linked = true;  // any authenticated (encrypted) packet proves the link

  switch (h->type) {
    case TK_PONG: {
      if (it.len < sizeof(tk_pong_t)) return;
      const tk_pong_t* p = (const tk_pong_t*)it.data;
      s_unanswered_pings = 0;
      a_host = p->host_alive != 0;
      a_rssi = p->rssi;
      return;
    }
    case TK_NACK: {
      if (it.len < sizeof(tk_nack_hdr_t)) return;
      const tk_nack_hdr_t* n = (const tk_nack_hdr_t*)it.data;
      if (!s_tx.active || n->h.clip != s_tx.clip) return;
      size_t count = n->count;
      if (sizeof(tk_nack_hdr_t) + count * 2 > it.len) count = (it.len - sizeof(tk_nack_hdr_t)) / 2;
      const uint8_t* q = it.data + sizeof(tk_nack_hdr_t);
      for (size_t i = 0; i < count; ++i) {
        uint16_t seq = (uint16_t)(q[i * 2] | (q[i * 2 + 1] << 8));
        if (seq >= rec_packets(s_tx.clip)) continue;
        if ((uint8_t)(s_tx.rs_head + 1) == s_tx.rs_tail) break;  // ring full
        s_tx.resend[s_tx.rs_head++] = seq;
      }
      // The Mac is alive and repairing the clip: give it time before giving up.
      if (s_tx.end_first) s_tx.end_first = millis();
      a_acts++;
      return;
    }
    case TK_CLIP_ACK: {
      if (it.len < sizeof(tk_clip_ack_t)) return;
      const tk_clip_ack_t* a = (const tk_clip_ack_t*)it.data;
      if (s_tx.active && a->h.clip == s_tx.clip && a->status == TK_ACK_UNKNOWN && s_tx.end_known &&
          s_tx.restarts < 1) {
        // The Mac lost this clip (listener restarted?): send it all again once.
        s_tx.restarts++;
        s_tx.restart();
        return;
      }
      if (s_tx.active && a->h.clip == s_tx.clip) s_tx.active = false;
      Event ev = {};
      ev.type = EvType::ClipAck;
      ev.clip = a->h.clip;
      ev.a = a->status;
      ev.c = a->received;
      ev.d = a->total;
      post(ev);
      return;
    }
    case TK_STATUS: {
      if (it.len < sizeof(tk_status_hdr_t)) return;
      const tk_status_hdr_t* s = (const tk_status_hdr_t*)it.data;
      Event ev = {};
      ev.type = EvType::Status;
      ev.clip = s->h.clip;
      ev.a = s->stage;
      ev.b = s->flags;
      size_t tl = it.len - sizeof(tk_status_hdr_t);
      if (tl > TK_STATUS_TEXT_MAX) tl = TK_STATUS_TEXT_MAX;
      memcpy(ev.text, it.data + sizeof(tk_status_hdr_t), tl);
      ev.text[tl] = 0;
      post(ev);
      return;
    }
    default:
      return;
  }
}

void handle_cmd(const Cmd& c) {
  switch (c.type) {
    case CmdType::Radio:
      s_want = c.on;
      break;
    case CmdType::Start:
      s_tx.active = true;
      s_tx.clip = c.clip;
      s_tx.target = c.target;
      s_tx.end_known = false;
      s_tx.total = 0;
      s_tx.samples = 0;
      s_tx.restarts = 0;
      s_tx.restart();
      a_sent = 0;
      a_total = 0;
      break;
    case CmdType::End:
      if (c.clip != s_tx.clip) break;
      s_tx.end_known = true;
      s_tx.total = c.total_packets;
      s_tx.samples = c.total_samples;
      s_tx.target = c.target;
      s_tx.reason = c.reason;
      a_total = c.total_packets;
      break;
    case CmdType::Cancel:
      if (c.clip == s_tx.clip && s_tx.active) {
        s_tx.active = false;
        if (s_up && s_linked) {
          tk_cancel_t m;
          tk_fill_hdr(&m.h, TK_CANCEL, c.clip);
          tx_peer(&m, sizeof(m));
        }
      }
      break;
    case CmdType::Retry:
      if (c.clip == s_tx.clip && s_tx.end_known) {
        s_tx.active = true;
        s_tx.restarts = 0;
        s_tx.restart();
        a_sent = 0;
      }
      break;
    case CmdType::Abort:
      if (c.clip == s_tx.clip) s_tx.active = false;
      break;
    case CmdType::Forget:
      if (s_have_peer && s_up) esp_now_del_peer(s_peer);
      s_have_peer = false;
      a_have = false;
      s_linked = false;
      s_prefs.remove("rx");
      break;
  }
}

// Push the current clip forward by one packet. Returns true if more work is
// queued right now (so the task should not sleep).
bool pump_clip(uint32_t now) {
  if (!s_tx.active) return false;
  if (!s_tx.start_sent) {
    tk_start_t m;
    tk_fill_hdr(&m.h, TK_START, s_tx.clip);
    m.target = s_tx.target;
    m.codec = TK_CODEC_IMA_ADPCM;
    m.sample_rate = TK_SAMPLE_RATE;
    m.samples_per_packet = kSpp;
    m.battery = a_battery.load();
    m.flags = a_bflags.load();
    if (tx_peer(&m, sizeof(m))) s_tx.start_sent = true;
    return s_linked;
  }
  uint8_t pkt[kAudioPktBytes];
  if (s_tx.rs_head != s_tx.rs_tail) {
    uint16_t seq = s_tx.resend[s_tx.rs_tail++];
    size_t len = rec_copy_packet(s_tx.clip, seq, pkt);
    if (len && tx_peer(pkt, len)) a_acts++;
    return true;
  }
  uint32_t avail = rec_packets(s_tx.clip);  // 0 until the recorder has started this clip
  if (s_tx.next < avail) {
    size_t len = rec_copy_packet(s_tx.clip, s_tx.next, pkt);
    if (!len) return false;  // the store moved on to a newer clip
    const bool ok = tx_peer(pkt, len);
    if (ok) a_acts++;
    if (ok || ++s_tx.pkt_fails >= 3) {  // give up on it; the Mac will NACK
      s_tx.next++;
      s_tx.pkt_fails = 0;
    }
    a_sent = s_tx.next;
    return s_linked;
  }
  if (s_tx.end_known && s_tx.next >= s_tx.total) {
    if (s_tx.end_first == 0 || now - s_tx.end_last >= TK_END_RETRY_MS) {
      if (s_tx.end_first == 0) s_tx.end_first = now;
      tk_end_t m;
      tk_fill_hdr(&m.h, TK_END, s_tx.clip);
      m.total_packets = s_tx.total;
      m.total_samples = s_tx.samples;
      m.target = s_tx.target;
      m.reason = s_tx.reason;
      tx_peer(&m, sizeof(m));
      s_tx.end_last = now;
    }
    if (now - s_tx.end_first > TK_END_TIMEOUT_MS) {
      s_tx.active = false;
      Event ev = {};
      ev.type = EvType::ClipFailed;
      ev.clip = s_tx.clip;
      post(ev);
    }
  }
  return false;
}

void link_task(void*) {
  for (;;) {
    uint32_t now = millis();
    Cmd c;
    while (xQueueReceive(s_cmdq, &c, 0) == pdTRUE) handle_cmd(c);

    bool need = s_want || s_tx.active;
    if (need && !s_up && (int32_t)(now - s_radio_retry_ms) >= 0) radio_on();
    if (!need && s_up) radio_off();

    RxItem it;
    int budget = 8;
    while (s_up && budget-- > 0 && xQueueReceive(s_rxq, &it, 0) == pdTRUE) handle_rx(it);

    bool busy = false;
    if (s_up) {
      if (!s_linked) {
        if (now - s_last_hello >= TK_HELLO_INTERVAL_MS) send_hello(now);
      } else {
        busy = pump_clip(now);
        if (!busy && !s_tx.active && now - s_last_ping >= TK_PING_INTERVAL_MS) send_ping(now);
      }
    }

    a_up = s_up;
    a_linked = s_up && s_linked;
    if (busy)
      taskYIELD();
    else
      ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(10));
  }
}

void send_cmd(const Cmd& c) {
  xQueueSend(s_cmdq, &c, pdMS_TO_TICKS(50));
  if (s_task) xTaskNotifyGive(s_task);
}

}  // namespace

bool link_init() {
  s_cmdq = xQueueCreate(16, sizeof(Cmd));
  s_evq = xQueueCreate(8, sizeof(Event));
  s_rxq = xQueueCreate(16, sizeof(RxItem));
  s_tx_done = xSemaphoreCreateBinary();
  tk_keys_derive(&s_keys);
  esp_read_mac(s_my_mac, ESP_MAC_WIFI_STA);
  esp_fill_random(s_nonce, 8);
  memcpy(s_prev_nonce, s_nonce, 8);
  s_prefs.begin("talkie", false);
  s_have_peer = s_prefs.getBytes("rx", s_peer, 6) == 6;
  a_have = s_have_peer;
  tk_rx_handler = on_rx;
  tk_tx_handler = on_tx;
  return xTaskCreatePinnedToCore(link_task, "talkie_link", 6144, nullptr, 2, &s_task, 1) == pdPASS;
}

void link_want_radio(bool on) {
  static bool requested = false;  // only ever called from the main loop
  static bool first = true;
  if (!first && requested == on) return;
  first = false;
  requested = on;
  Cmd c = {};
  c.type = CmdType::Radio;
  c.on = on;
  send_cmd(c);
}

void link_clip_start(uint16_t clip, uint8_t target) {
  Cmd c = {};
  c.type = CmdType::Start;
  c.clip = clip;
  c.target = target;
  send_cmd(c);
}

void link_clip_end(uint16_t clip, uint16_t total_packets, uint32_t total_samples, uint8_t target,
                   uint8_t reason) {
  Cmd c = {};
  c.type = CmdType::End;
  c.clip = clip;
  c.total_packets = total_packets;
  c.total_samples = total_samples;
  c.target = target;
  c.reason = reason;
  send_cmd(c);
}

void link_clip_cancel(uint16_t clip) {
  Cmd c = {};
  c.type = CmdType::Cancel;
  c.clip = clip;
  send_cmd(c);
}

void link_clip_retry(uint16_t clip) {
  Cmd c = {};
  c.type = CmdType::Retry;
  c.clip = clip;
  send_cmd(c);
}

void link_clip_abort(uint16_t clip) {
  Cmd c = {};
  c.type = CmdType::Abort;
  c.clip = clip;
  send_cmd(c);
}

void link_forget_receiver() {
  Cmd c = {};
  c.type = CmdType::Forget;
  send_cmd(c);
}

void link_set_battery(uint8_t pct, uint8_t flags) {
  a_battery = pct;
  a_bflags = flags;
}

bool link_poll_event(Event* ev) { return xQueueReceive(s_evq, ev, 0) == pdTRUE; }

LinkInfo link_info() {
  LinkInfo i;
  i.radio_up = a_up.load();
  i.linked = a_linked.load();
  i.host_alive = a_host.load();
  i.have_receiver = a_have.load();
  i.lr = a_lr.load();
  i.rssi = a_rssi.load();
  i.tx_sent = a_sent.load();
  i.tx_total = a_total.load();
  i.tx_activity = a_acts.load();
  return i;
}

}  // namespace app
