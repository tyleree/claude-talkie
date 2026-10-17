// Claude Talkie receiver - Seeed XIAO ESP32-C3 plugged into the Mac.
//
// A deliberately dumb bridge: it pairs with sticks that know the shared
// secret, answers their keepalives, and shuttles packets between the radio
// and the Mac listener over USB serial as text lines:
//
//   to Mac:   R <stick-mac> <rssi> <base64 packet>   packet from a stick
//             I receiver mac=.. ch=.. lr=.. fw=..     info (on boot and on request)
//             E <event>                               pairing events
//             X <stick-mac> <reason>                  could not deliver to a stick
//   from Mac: S <stick-mac> <base64 packet>           send to a stick
//             H                                       heartbeat (listener alive)
//             I                                       print info
//             F                                       forget all paired sticks
#include <Arduino.h>
#include <Preferences.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <mbedtls/base64.h>
#include <stddef.h>

#include "talkie_config.h"
#include "talkie_crypto.h"
#include "talkie_protocol.h"
#include "talkie_radio.h"

// Sniff RSSI of stick frames in promiscuous mode (only needed on ESP-IDF 4,
// whose ESP-NOW receive callback does not report RSSI).
#define TK_RX_RSSI_SNIFF 1

static constexpr int kMaxSticks = 4;
static constexpr size_t kLineMax = 512;

struct RxItem {
  uint8_t mac[6];
  int8_t rssi;
  uint8_t len;
  uint8_t data[TK_MAX_PAYLOAD];
};

static QueueHandle_t s_rxq;
static SemaphoreHandle_t s_tx_done;
static volatile bool s_tx_ok = false;
static tk_keys_t s_keys;
static tk_radio_status_t s_radio;
static uint8_t s_my_mac[6];
static uint8_t s_sticks[kMaxSticks][6];
static int s_nsticks = 0;
static volatile int8_t s_stick_rssi[kMaxSticks];
static uint32_t s_host_last_ms = 0;
static bool s_host_seen = false;
static uint32_t s_rx_count = 0, s_rx_dropped = 0, s_tx_fail = 0;
static Preferences s_prefs;

static char s_line[kLineMax];
static size_t s_line_len = 0;

// --------------------------------------------------------------- helpers ---

static bool host_alive() { return s_host_seen && (millis() - s_host_last_ms) < TK_HOST_ALIVE_MS; }

static int find_stick(const uint8_t mac[6]) {
  for (int i = 0; i < s_nsticks; ++i)
    if (memcmp(s_sticks[i], mac, 6) == 0) return i;
  return -1;
}

static void save_sticks() {
  s_prefs.putBytes("sticks", s_sticks, (size_t)s_nsticks * 6);
}

static void load_sticks() {
  size_t n = s_prefs.getBytesLength("sticks");
  if (n % 6 != 0 || n > sizeof(s_sticks)) n = 0;
  if (n) s_prefs.getBytes("sticks", s_sticks, n);
  s_nsticks = (int)(n / 6);
}

static void remember_stick(const uint8_t mac[6]) {
  int i = find_stick(mac);
  if (i < 0) {
    if (s_nsticks == kMaxSticks) {  // drop the oldest
      esp_now_del_peer(s_sticks[0]);
      memmove(s_sticks[0], s_sticks[1], (kMaxSticks - 1) * 6);
      for (int k = 0; k < kMaxSticks - 1; ++k) s_stick_rssi[k] = s_stick_rssi[k + 1];
      s_nsticks--;
    }
    memcpy(s_sticks[s_nsticks], mac, 6);
    s_stick_rssi[s_nsticks] = 0;
    s_nsticks++;
    save_sticks();
    char m[13];
    tk_mac_to_str(mac, m);
    Serial.printf("E paired %s\n", m);
  }
  tk_add_peer(mac, &s_keys);
}

static void forget_sticks() {
  for (int i = 0; i < s_nsticks; ++i) esp_now_del_peer(s_sticks[i]);
  s_nsticks = 0;
  s_prefs.remove("sticks");
  Serial.println("E forgot all sticks");
}

static bool parse_mac(const char* s, uint8_t out[6]) {
  for (int i = 0; i < 6; ++i) {
    unsigned v;
    char buf[3] = {s[i * 2], s[i * 2 + 1], 0};
    if (!isxdigit((unsigned char)buf[0]) || !isxdigit((unsigned char)buf[1])) return false;
    v = strtoul(buf, nullptr, 16);
    out[i] = (uint8_t)v;
  }
  return true;
}

static void print_info() {
  char m[13];
  tk_mac_to_str(s_my_mac, m);
  Serial.printf("I receiver mac=%s ch=%d lr=%d fw=%d.%d proto=%d sticks=%d rx=%lu drop=%lu txfail=%lu",
                m, TK_WIFI_CHANNEL, s_radio.lr ? 1 : 0, TK_FW_VERSION >> 8, TK_FW_VERSION & 0xFF,
                TK_PROTO_VERSION, s_nsticks, (unsigned long)s_rx_count, (unsigned long)s_rx_dropped,
                (unsigned long)s_tx_fail);
  if (!s_radio.ok || s_radio.err) Serial.printf(" err=%s:%d", s_radio.where, s_radio.err);
  Serial.print('\n');
}

// Send one packet and wait for the MAC-layer acknowledgement. Each attempt
// that goes unanswered blocks the loop for ~60 ms, so keep `attempts` low for
// anything the stick will ask for again anyway.
static bool send_packet(const uint8_t mac[6], const void* data, size_t len, int attempts = 3) {
  for (int attempt = 0; attempt < attempts; ++attempt) {
    xSemaphoreTake(s_tx_done, 0);
    esp_err_t e = esp_now_send(mac, (const uint8_t*)data, len);
    if (e == ESP_ERR_ESPNOW_NO_MEM) {
      vTaskDelay(pdMS_TO_TICKS(2));
      continue;
    }
    if (e != ESP_OK) return false;
    if (xSemaphoreTake(s_tx_done, pdMS_TO_TICKS(60)) == pdTRUE && s_tx_ok) return true;
    vTaskDelay(pdMS_TO_TICKS(3));
  }
  s_tx_fail++;
  return false;
}

// ------------------------------------------------------------- callbacks ---

static void on_rx(const uint8_t* mac, const uint8_t* data, int len, int8_t rssi) {
  if (len <= 0 || len > TK_MAX_PAYLOAD) return;
  RxItem it;
  memcpy(it.mac, mac, 6);
  it.rssi = rssi;
  it.len = (uint8_t)len;
  memcpy(it.data, data, len);
  if (xQueueSend(s_rxq, &it, 0) != pdTRUE) s_rx_dropped++;
}

static void on_tx(bool ok) {
  s_tx_ok = ok;
  xSemaphoreGive(s_tx_done);
}

#if TK_RX_RSSI_SNIFF && ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(5, 0, 0)
static void on_promiscuous(void* buf, wifi_promiscuous_pkt_type_t type) {
  if (type != WIFI_PKT_MGMT) return;  // ESP-NOW frames are action frames
  const wifi_promiscuous_pkt_t* p = (const wifi_promiscuous_pkt_t*)buf;
  if (p->rx_ctrl.sig_len < 24) return;
  const uint8_t* src = p->payload + 10;  // 802.11 addr2
  for (int i = 0; i < s_nsticks; ++i)
    if (memcmp(s_sticks[i], src, 6) == 0) s_stick_rssi[i] = (int8_t)p->rx_ctrl.rssi;
}
#endif

// --------------------------------------------------------------- packets ---

static void handle_hello(const RxItem& it) {
  if (it.len != sizeof(tk_hello_t)) return;
  const tk_hello_t* m = (const tk_hello_t*)it.data;
  if (m->proto != TK_PROTO_VERSION || m->role != TK_ROLE_STICK) {
    Serial.printf("L hello with protocol %d (want %d) ignored\n", m->proto, TK_PROTO_VERSION);
    return;
  }
  if (!tk_hmac8_check(&s_keys, m, offsetof(tk_hello_t, hmac), it.mac, m->hmac)) {
    char s[13];
    tk_mac_to_str(it.mac, s);
    Serial.printf("L hello from %s failed authentication (different secret?)\n", s);
    return;
  }
  remember_stick(it.mac);

  tk_hello_ack_t a;
  memset(&a, 0, sizeof(a));
  tk_fill_hdr(&a.h, TK_HELLO_ACK, 0);
  a.proto = TK_PROTO_VERSION;
  a.role = TK_ROLE_RECEIVER;
  a.fw = TK_FW_VERSION;
  memcpy(a.nonce, m->nonce, sizeof(a.nonce));
  memcpy(a.stick_mac, it.mac, 6);
  a.channel = TK_WIFI_CHANNEL;
  a.host_alive = host_alive() ? 1 : 0;
  tk_hmac8(&s_keys, &a, offsetof(tk_hello_ack_t, hmac), s_my_mac, a.hmac);
  send_packet(tk_broadcast_mac, &a, sizeof(a));
}

static void forward_to_host(const RxItem& it, int8_t rssi) {
  char m[13];
  tk_mac_to_str(it.mac, m);
  unsigned char b64[((TK_MAX_PAYLOAD + 2) / 3) * 4 + 4];
  size_t olen = 0;
  if (mbedtls_base64_encode(b64, sizeof(b64), &olen, it.data, it.len) != 0) return;
  b64[olen] = 0;
  Serial.printf("R %s %d %s\n", m, (int)rssi, (const char*)b64);
}

static void handle_rx(const RxItem& it) {
  if (it.len < sizeof(tk_hdr_t)) return;
  const tk_hdr_t* h = (const tk_hdr_t*)it.data;
  if (h->magic != TK_MAGIC) return;
  s_rx_count++;

  if (h->type == TK_HELLO) {
    handle_hello(it);
    return;
  }
  int idx = find_stick(it.mac);
  if (idx < 0) return;  // not paired (cannot normally happen: frames are encrypted)
  int8_t rssi = it.rssi ? it.rssi : s_stick_rssi[idx];

  if (h->type == TK_PING) {
    tk_pong_t p;
    tk_fill_hdr(&p.h, TK_PONG, 0);
    p.host_alive = host_alive() ? 1 : 0;
    p.rssi = rssi;
    send_packet(it.mac, &p, sizeof(p), 1);  // the stick pings again in a few seconds
  }
  forward_to_host(it, rssi);
}

// ------------------------------------------------------------ host lines ---

static void handle_line(char* line) {
  size_t n = strlen(line);
  while (n && (line[n - 1] == '\r' || line[n - 1] == ' ')) line[--n] = 0;
  if (!n) return;
  s_host_last_ms = millis();
  s_host_seen = true;

  switch (line[0]) {
    case 'H':
      return;
    case 'I':
      print_info();
      return;
    case 'F':
      forget_sticks();
      return;
    case 'S': {
      // S <12 hex mac> <base64>
      if (n < 16 || line[1] != ' ' || line[14] != ' ') return;
      uint8_t mac[6];
      if (!parse_mac(line + 2, mac)) return;
      uint8_t pkt[TK_MAX_PAYLOAD + 4];
      size_t plen = 0;
      if (mbedtls_base64_decode(pkt, sizeof(pkt), &plen, (const unsigned char*)line + 15, n - 15) != 0 ||
          plen < sizeof(tk_hdr_t) || plen > TK_MAX_PAYLOAD)
        return;
      if (find_stick(mac) < 0) {
        Serial.printf("X %.12s unknown-stick\n", line + 2);
        return;
      }
      if (!send_packet(mac, pkt, plen)) Serial.printf("X %.12s no-ack\n", line + 2);
      return;
    }
    default:
      return;
  }
}

static void read_host() {
  static bool discarding = false;  // inside an overlong line: drop it up to its newline
  while (Serial.available() > 0) {
    int c = Serial.read();
    if (c < 0) break;
    if (c == '\n') {
      s_line[s_line_len] = 0;
      if (!discarding) handle_line(s_line);
      s_line_len = 0;
      discarding = false;
    } else if (discarding) {
      continue;
    } else if (s_line_len < kLineMax - 1) {
      s_line[s_line_len++] = (char)c;
    } else {
      s_line_len = 0;
      discarding = true;
    }
  }
}

// ----------------------------------------------------------------- setup ---

void setup() {
  // Lines can be ~350 bytes; the 256-byte defaults would truncate them.
  Serial.setTxBufferSize(4096);
  Serial.setRxBufferSize(2048);
  Serial.begin(115200);
  // Wait at most 5 ms for the Mac to drain the buffer; with no listener
  // attached, output is simply dropped and nothing blocks.
  Serial.setTxTimeoutMs(5);
  delay(50);

  s_rxq = xQueueCreate(24, sizeof(RxItem));
  s_tx_done = xSemaphoreCreateBinary();
  tk_keys_derive(&s_keys);
  s_prefs.begin("talkie", false);
  load_sticks();

  tk_rx_handler = on_rx;
  tk_tx_handler = on_tx;
  s_radio = tk_radio_start(&s_keys);
  esp_wifi_get_mac(WIFI_IF_STA, s_my_mac);
  for (int i = 0; i < s_nsticks; ++i) tk_add_peer(s_sticks[i], &s_keys);

#if TK_RX_RSSI_SNIFF && ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(5, 0, 0)
  if (s_radio.ok) {
    wifi_promiscuous_filter_t f;
    f.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT;
    esp_wifi_set_promiscuous_filter(&f);
    esp_wifi_set_promiscuous_rx_cb(on_promiscuous);
    esp_wifi_set_promiscuous(true);
  }
#endif
  print_info();
}

void loop() {
  static uint32_t last_err_report = 0;
  RxItem it;
  int budget = 16;
  while (budget-- > 0 && xQueueReceive(s_rxq, &it, 0) == pdTRUE) handle_rx(it);
  read_host();
  if (!s_radio.ok && millis() - last_err_report > 5000) {
    last_err_report = millis();
    print_info();
  }
  if (uxQueueMessagesWaiting(s_rxq) == 0) vTaskDelay(1);
}
