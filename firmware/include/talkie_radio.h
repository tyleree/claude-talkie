// ESP-NOW setup shared by the stick and the receiver (Arduino-ESP32 2.x/3.x).
#pragma once

#include <Arduino.h>
#include <WiFi.h>
#include <esp_idf_version.h>
#include <esp_now.h>
#include <esp_wifi.h>

#include "talkie_config.h"
#include "talkie_crypto.h"
#include "talkie_protocol.h"

typedef void (*tk_rx_handler_t)(const uint8_t* mac, const uint8_t* data, int len, int8_t rssi);
typedef void (*tk_tx_handler_t)(bool ok);

static tk_rx_handler_t tk_rx_handler = nullptr;
static tk_tx_handler_t tk_tx_handler = nullptr;

// ESP-IDF changed both callback signatures over time; adapt them here.
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
static void tk_on_esp_now_rx(const esp_now_recv_info_t* info, const uint8_t* data, int len) {
  if (tk_rx_handler) tk_rx_handler(info->src_addr, data, len, info->rx_ctrl ? (int8_t)info->rx_ctrl->rssi : 0);
}
#else
static void tk_on_esp_now_rx(const uint8_t* mac, const uint8_t* data, int len) {
  if (tk_rx_handler) tk_rx_handler(mac, data, len, 0);
}
#endif

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 5, 0)
static void tk_on_esp_now_tx(const esp_now_send_info_t* info, esp_now_send_status_t status) {
  (void)info;
  if (tk_tx_handler) tk_tx_handler(status == ESP_NOW_SEND_SUCCESS);
}
#else
static void tk_on_esp_now_tx(const uint8_t* mac, esp_now_send_status_t status) {
  (void)mac;
  if (tk_tx_handler) tk_tx_handler(status == ESP_NOW_SEND_SUCCESS);
}
#endif

static const uint8_t tk_broadcast_mac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

typedef struct {
  bool ok;       // ESP-NOW is up
  bool lr;       // long-range mode active
  int err;       // first error, 0 if none
  const char* where;
} tk_radio_status_t;

// Bring up Wi-Fi in station mode (never connecting to anything) and ESP-NOW
// on the configured channel, optionally in Long Range mode, with encryption.
static tk_radio_status_t tk_radio_start(const tk_keys_t* keys) {
  tk_radio_status_t r = {false, false, 0, ""};
  esp_err_t e;
  WiFi.persistent(false);
  if (!WiFi.mode(WIFI_STA)) {
    r.err = -1;
    r.where = "wifi mode";
    return r;
  }
  esp_wifi_set_ps(WIFI_PS_NONE);
#if TK_LONG_RANGE
  // Keep b/g/n as well so either side can still decode normal-rate frames.
  e = esp_wifi_set_protocol(WIFI_IF_STA, WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N |
                                             WIFI_PROTOCOL_LR);
  r.lr = (e == ESP_OK);
  if (e != ESP_OK && !r.err) {
    r.err = e;
    r.where = "long range";
  }
#endif
  e = esp_wifi_set_channel(TK_WIFI_CHANNEL, WIFI_SECOND_CHAN_NONE);
  if (e != ESP_OK) {  // older cores only allow this in promiscuous mode
    esp_wifi_set_promiscuous(true);
    e = esp_wifi_set_channel(TK_WIFI_CHANNEL, WIFI_SECOND_CHAN_NONE);
    esp_wifi_set_promiscuous(false);
  }
  if (e != ESP_OK && !r.err) {
    r.err = e;
    r.where = "channel";
  }
  esp_wifi_set_max_tx_power(TK_TX_POWER_QDBM);
  e = esp_now_init();
  if (e != ESP_OK) {
    r.err = e;
    r.where = "esp_now_init";
    return r;
  }
#if TK_LONG_RANGE
  if (r.lr) {
    e = esp_wifi_config_espnow_rate(WIFI_IF_STA, TK_LR_500K ? WIFI_PHY_RATE_LORA_500K : WIFI_PHY_RATE_LORA_250K);
    if (e != ESP_OK) {
      r.lr = false;
      if (!r.err) {
        r.err = e;
        r.where = "lr rate";
      }
    }
  }
#endif
  esp_now_set_pmk(keys->pmk);
  esp_now_register_recv_cb(tk_on_esp_now_rx);
  esp_now_register_send_cb(tk_on_esp_now_tx);

  esp_now_peer_info_t p;
  memset(&p, 0, sizeof(p));
  memcpy(p.peer_addr, tk_broadcast_mac, 6);
  p.channel = 0;  // current channel
  p.ifidx = WIFI_IF_STA;
  p.encrypt = false;
  esp_now_add_peer(&p);
  r.ok = true;
  return r;
}

static void tk_radio_stop(void) {
  esp_now_deinit();
  WiFi.mode(WIFI_OFF);
}

static bool tk_add_peer(const uint8_t mac[6], const tk_keys_t* keys) {
  if (esp_now_is_peer_exist(mac)) return true;
  esp_now_peer_info_t p;
  memset(&p, 0, sizeof(p));
  memcpy(p.peer_addr, mac, 6);
  p.channel = 0;
  p.ifidx = WIFI_IF_STA;
  p.encrypt = true;
  memcpy(p.lmk, keys->lmk, 16);
  return esp_now_add_peer(&p) == ESP_OK;
}

static void tk_mac_to_str(const uint8_t mac[6], char out[13]) {
  static const char* hex = "0123456789abcdef";
  for (int i = 0; i < 6; ++i) {
    out[i * 2] = hex[mac[i] >> 4];
    out[i * 2 + 1] = hex[mac[i] & 15];
  }
  out[12] = 0;
}

static void tk_fill_hdr(tk_hdr_t* h, uint8_t type, uint16_t clip) {
  h->magic = TK_MAGIC;
  h->type = type;
  h->clip = clip;
}
