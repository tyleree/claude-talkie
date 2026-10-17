// Claude Talkie stick - M5StickS3 push-to-talk for Claude Code.
//
// Controls
//   hold A ........ talk to the agent you're looking at (release to send)
//   hold B ........ start a new background agent (release to send)
//   double-tap .... lock recording on; tap the same button again to send
//   other button .. cancel while recording
//   hold B at boot  forget the paired receiver
//
// This file owns the push-to-talk state machine and power management.
// audio.cpp records and encodes, link.cpp talks to the receiver, ui.cpp draws.
#include <M5Unified.h>
#include <driver/gpio.h>
#include <esp_mac.h>
#include <esp_sleep.h>
#include <esp_system.h>

#include "app.h"

using namespace app;

namespace {

constexpr uint32_t kBootSplashMs = 1200;
constexpr uint32_t kMessageMs = 1800;
constexpr uint32_t kFailedTimeoutMs = 60000;
constexpr uint32_t kSendStallMs = 15000;
constexpr uint32_t kBatteryPollMs = 20000;
constexpr gpio_num_t kPinA = GPIO_NUM_11;  // KEY1 (active low)
constexpr gpio_num_t kPinB = GPIO_NUM_12;  // KEY2 (active low)

struct Buttons {
  bool a_pressed, a_released, b_pressed, b_released, a_down, b_down;
};

Mode s_mode = Mode::Boot;
uint32_t s_mode_since = 0;
uint8_t s_target = TK_TARGET_CURRENT;
uint16_t s_clip = 0;
uint32_t s_press_ms = 0;
uint32_t s_rec_start = 0;
uint32_t s_tap_deadline = 0;
bool s_lock_armed = false;
uint8_t s_end_reason = TK_END_RELEASED;
uint32_t s_until = 0;
uint32_t s_progress = 0;
uint32_t s_progress_ms = 0;
uint32_t s_last_activity = 0;
bool s_screen = true;
bool s_ok = false;
char s_line1[64] = "";
char s_line2[TK_STATUS_TEXT_MAX + 1] = "";
int s_battery = -1;
bool s_charging = false;
bool s_usb = false;
uint32_t s_battery_ms = 0;
uint32_t s_last_render = 0;
bool s_dirty = true;
char s_serial[64];
size_t s_serial_len = 0;

bool recording() { return s_mode == Mode::Rec || s_mode == Mode::RecPending || s_mode == Mode::RecLocked; }

bool busy() {
  return recording() || s_mode == Mode::Finishing || s_mode == Mode::Sending || s_mode == Mode::Waiting ||
         s_mode == Mode::Failed;
}

void set_mode(Mode m) {
  s_mode = m;
  s_mode_since = millis();
  s_dirty = true;
}

void set_lines(const char* a, const char* b) {
  strlcpy(s_line1, a ? a : "", sizeof(s_line1));
  strlcpy(s_line2, b ? b : "", sizeof(s_line2));
}

void wake() {
  s_last_activity = millis();
  if (!s_screen) {
    s_screen = true;
    ui_screen(true);
  }
  s_dirty = true;
}

void message(const char* a, const char* b, uint32_t ms = kMessageMs) {
  set_lines(a, b);
  s_until = millis() + ms;
  set_mode(Mode::Message);
}

void poll_battery(uint32_t now, bool force) {
  if (!force && now - s_battery_ms < kBatteryPollMs) return;
  s_battery_ms = now;
  int lvl = M5.Power.getBatteryLevel();
  s_battery = (lvl >= 0 && lvl <= 100) ? lvl : -1;
  s_usb = M5.Power.getVBUSVoltage() > 4000;
  s_charging = M5.Power.isCharging() == m5::Power_Class::is_charging;
  link_set_battery(s_battery < 0 ? 255 : (uint8_t)s_battery, (s_charging ? 1 : 0) | (s_usb ? 2 : 0));
}

// ------------------------------------------------------------ recording ---

void start_recording(uint8_t target) {
  wake();
  if (TK_BEEP_ON_START) ui_beep(Beep::Start);
  mic_start();
  if (++s_clip == 0) s_clip = 1;
  s_target = target;
  s_press_ms = s_rec_start = millis();
  s_lock_armed = false;
  link_want_radio(true);
  rec_begin(s_clip);
  link_clip_start(s_clip, target);
  set_lines("", "");
  set_mode(Mode::Rec);
}

void finish_recording(uint8_t reason) {
  rec_stop();
  s_end_reason = reason;
  set_mode(Mode::Finishing);
}

void cancel_recording(const char* a, const char* b) {
  rec_stop();
  link_clip_cancel(s_clip);
  message(a, b);
}

// ------------------------------------------------------------- buttons ---

Buttons read_buttons() {
  M5.update();
  Buttons b;
  b.a_pressed = M5.BtnA.wasPressed();
  b.a_released = M5.BtnA.wasReleased();
  b.b_pressed = M5.BtnB.wasPressed();
  b.b_released = M5.BtnB.wasReleased();
  b.a_down = M5.BtnA.isPressed();
  b.b_down = M5.BtnB.isPressed();
  return b;
}

void handle_buttons(const Buttons& b, uint32_t now) {
  if (b.a_pressed || b.b_pressed || b.a_released || b.b_released) wake();
  const bool mine_pressed = s_target == TK_TARGET_NEW ? b.b_pressed : b.a_pressed;
  const bool mine_released = s_target == TK_TARGET_NEW ? b.b_released : b.a_released;
  const bool other_pressed = s_target == TK_TARGET_NEW ? b.a_pressed : b.b_pressed;

  switch (s_mode) {
    case Mode::Boot:
    case Mode::Idle:
    case Mode::Waiting:
    case Mode::Result:
    case Mode::Message:
      if (b.a_pressed)
        start_recording(TK_TARGET_CURRENT);
      else if (b.b_pressed)
        start_recording(TK_TARGET_NEW);
      break;

    case Mode::Rec:
      if (other_pressed) {
        cancel_recording("Cancelled", "");
      } else if (mine_released) {
        if (now - s_press_ms >= TK_TAP_MAX_MS) {
          finish_recording(TK_END_RELEASED);
        } else {
          s_tap_deadline = now + TK_DOUBLE_TAP_MS;
          set_mode(Mode::RecPending);
        }
      }
      break;

    case Mode::RecPending:
      if (other_pressed) {
        cancel_recording("Cancelled", "");
      } else if (mine_pressed) {
        s_lock_armed = false;
        set_mode(Mode::RecLocked);
      }
      break;

    case Mode::RecLocked:
      if (other_pressed) {
        cancel_recording("Cancelled", "");
      } else if (!s_lock_armed && mine_released) {
        s_lock_armed = true;  // the second tap of the double-tap has ended
      } else if (s_lock_armed && mine_pressed) {
        finish_recording(TK_END_LATCHED);
      }
      break;

    case Mode::Failed:
      if (b.a_pressed) {
        link_want_radio(true);
        link_clip_retry(s_clip);
        s_progress = 0;
        s_progress_ms = now;
        set_mode(Mode::Sending);
      } else if (b.b_pressed) {
        message("Discarded", "");
      }
      break;

    case Mode::Finishing:
    case Mode::Sending:
      break;  // wait for delivery
  }
}

// --------------------------------------------------------------- events ---

void show_result(bool ok, const char* l1, const char* l2, bool sound) {
  wake();
  s_ok = ok;
  set_lines(l1, l2);
  s_until = millis() + TK_RESULT_SHOW_MS;
  set_mode(Mode::Result);
  if (sound && (ok ? TK_BEEP_ON_SUCCESS : TK_BEEP_ON_ERROR)) ui_beep(ok ? Beep::Ok : Beep::Error);
}

void fail_delivery() {
  set_mode(Mode::Failed);
  if (TK_BEEP_ON_ERROR) ui_beep(Beep::Error);
}

void handle_events(uint32_t now) {
  Event ev;
  while (link_poll_event(&ev)) {
    switch (ev.type) {
      case EvType::Paired:
        if (s_mode == Mode::Idle || s_mode == Mode::Boot) message("Paired", "Found the receiver.");
        s_dirty = true;
        break;

      case EvType::ClipAck:
        if (ev.clip != s_clip || (s_mode != Mode::Sending && s_mode != Mode::Failed)) break;
        if (ev.a == TK_ACK_CANCELLED) {
          set_mode(Mode::Idle);
          break;
        }
        if (ev.a == TK_ACK_UNKNOWN) {  // the Mac still doesn't have it after a full resend
          if (s_mode == Mode::Sending) fail_delivery();
          break;
        }
        set_lines(ev.a == TK_ACK_PARTIAL ? "Transcribing (some audio lost)" : "Transcribing", "");
        s_until = now + TK_RESULT_TIMEOUT_MS;
        set_mode(Mode::Waiting);
        break;

      case EvType::ClipFailed:
        if (ev.clip == s_clip && s_mode == Mode::Sending) fail_delivery();
        break;

      case EvType::Status: {
        char* nl = strchr(ev.text, '\n');
        const char* l2 = "";
        if (nl) {
          *nl = 0;
          l2 = nl + 1;
        }
        const bool final = ev.a == TK_STAGE_DONE || ev.a == TK_STAGE_ERROR;
        if (!final) {
          if (ev.clip == s_clip && (s_mode == Mode::Waiting || s_mode == Mode::Sending)) {
            set_lines(ev.text, l2);
            if (s_mode == Mode::Sending) {  // our ack got lost but the Mac has the clip
              s_until = now + TK_RESULT_TIMEOUT_MS;
              set_mode(Mode::Waiting);
            }
            s_dirty = true;
          }
          break;
        }
        if (recording() || s_mode == Mode::Finishing) break;  // never interrupt a new clip
        if ((s_mode == Mode::Sending || s_mode == Mode::Failed) && ev.clip != s_clip) break;
        show_result(ev.a == TK_STAGE_DONE, ev.text, l2, (ev.b & TK_STATUS_SOUND) != 0);
        break;
      }
    }
  }
}

// ------------------------------------------------------------------ tick ---

void tick(uint32_t now) {
  switch (s_mode) {
    case Mode::Boot:
      if (now - s_mode_since > kBootSplashMs) set_mode(Mode::Idle);
      break;

    case Mode::Rec:
    case Mode::RecLocked:
      if (rec_full(s_clip)) {
        finish_recording(TK_END_MAX_LENGTH);
        break;
      }
      s_dirty = true;
      break;

    case Mode::RecPending:
      if ((int32_t)(now - s_tap_deadline) > 0)
        cancel_recording("Hold to talk", "Or double-tap to lock recording on, then tap again to send.");
      s_dirty = true;
      break;

    case Mode::Finishing:
      if (rec_flushed(s_clip)) {
        uint32_t samples = rec_samples();
        uint32_t packets = rec_packets(s_clip);
        if (packets == 0 || samples < (uint32_t)TK_MIN_CLIP_MS * TK_SAMPLE_RATE / 1000) {
          link_clip_cancel(s_clip);
          message("Too short", "Hold the button while you talk.");
        } else {
          link_clip_end(s_clip, (uint16_t)packets, samples, s_target, s_end_reason);
          s_progress = 0;
          s_progress_ms = now;
          set_mode(Mode::Sending);
          if (TK_BEEP_ON_SEND) ui_beep(Beep::Sent);
        }
      } else if (now - s_mode_since > 3000) {  // audio task never answered
        link_clip_cancel(s_clip);
        message("Audio error", "The recorder stopped responding. Press the side button to restart.", 5000);
      }
      break;

    case Mode::Sending: {
      LinkInfo li = link_info();
      if (li.tx_activity != s_progress) {  // packets or resend requests still flowing
        s_progress = li.tx_activity;
        s_progress_ms = now;
      }
      if (now - s_progress_ms > kSendStallMs) {
        link_clip_abort(s_clip);
        fail_delivery();
      }
      s_dirty = true;
      break;
    }

    case Mode::Waiting:
      if ((int32_t)(now - s_until) > 0) {
        show_result(false, "No answer", "The Mac got the clip but never replied. Check the Talkie app's log.",
                    true);
      }
      s_dirty = true;
      break;

    case Mode::Result:
    case Mode::Message:
      if ((int32_t)(now - s_until) > 0) {
        set_lines("", "");
        set_mode(Mode::Idle);
      }
      break;

    case Mode::Failed:
      if (now - s_mode_since > kFailedTimeoutMs) set_mode(Mode::Idle);
      break;

    case Mode::Idle:
      break;
  }
}

// ----------------------------------------------------------------- power ---

void light_sleep() {
  Serial.flush();
  gpio_wakeup_enable(kPinA, GPIO_INTR_LOW_LEVEL);
  gpio_wakeup_enable(kPinB, GPIO_INTR_LOW_LEVEL);
  esp_sleep_enable_gpio_wakeup();
  esp_light_sleep_start();  // returns when a button is pressed
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_GPIO);
  gpio_wakeup_disable(kPinA);
  gpio_wakeup_disable(kPinB);
}

void manage_power(uint32_t now, const Buttons& b) {
  const uint32_t idle = now - s_last_activity;
  const bool is_busy = busy();
  const LinkInfo li = link_info();
  const bool pairing = !li.have_receiver && s_screen;
  link_want_radio(is_busy || idle < TK_RADIO_IDLE_MS || pairing);

  if (s_screen && !is_busy && s_mode != Mode::Result && s_mode != Mode::Message &&
      idle > TK_SCREEN_TIMEOUT_MS) {
    s_screen = false;
    ui_screen(false);
  }
  if (mic_running() && !is_busy && idle > TK_MIC_KEEPALIVE_MS) mic_stop();
  poll_battery(now, false);

#if TK_LIGHT_SLEEP
  if (!s_screen && s_mode == Mode::Idle && !mic_running() && !li.radio_up && !b.a_down && !b.b_down) {
    // Sleeping would stall USB, so check for a cable plugged in since the last poll.
    if (now - s_battery_ms > 2000) poll_battery(now, true);
    if (!s_usb) light_sleep();
  }
#endif
}

void handle_serial() {
  while (Serial.available() > 0) {
    int c = Serial.read();
    if (c < 0) break;
    if (c == '\n') {
      s_serial[s_serial_len] = 0;
      if (s_serial[0] == 'I') {  // lets the Mac app tell the stick from the receiver
        uint8_t mac[6];
        esp_read_mac(mac, ESP_MAC_WIFI_STA);
        Serial.printf("I stick mac=%02x%02x%02x%02x%02x%02x fw=%d.%d clip_s=%lu\n", mac[0], mac[1], mac[2],
                      mac[3], mac[4], mac[5], TK_FW_VERSION >> 8, TK_FW_VERSION & 0xFF,
                      (unsigned long)clip_capacity_seconds());
      }
      s_serial_len = 0;
    } else if (s_serial_len < sizeof(s_serial) - 1) {
      s_serial[s_serial_len++] = (char)c;
    }
  }
}

void render(uint32_t now) {
  if (!s_screen || s_mode == Mode::Boot) return;
  const bool fast = recording() || s_mode == Mode::Sending || s_mode == Mode::Waiting || s_mode == Mode::Finishing;
  const uint32_t interval = s_dirty ? 33 : (fast ? 66 : 500);
  if (now - s_last_render < interval) return;
  s_last_render = now;
  s_dirty = false;

  UiModel m;
  m.mode = s_mode;
  m.target = s_target;
  m.rec_ms = recording() ? now - s_rec_start : 0;
  m.level = mic_level();
  m.mic_ready = mic_ready();
  m.link = link_info();
  m.battery = s_battery;
  m.charging = s_charging;
  m.usb = s_usb;
  m.ok = s_ok;
  m.anim = now;
  m.line1 = s_line1;
  m.line2 = s_line2;
  ui_render(m);
}

}  // namespace

void setup() {
  auto cfg = M5.config();
  cfg.serial_baudrate = 115200;
  cfg.output_power = false;  // no Grove/Hat accessories: keep EXT 5V off
  cfg.internal_imu = false;
  cfg.internal_rtc = false;
  cfg.external_display_value = 0;
  cfg.external_speaker_value = 0;
  M5.begin(cfg);
  // With 0, a write to a host that has the port closed (e.g. charging from the
  // Mac) spins forever once the buffer fills. A few ms lets HWCDC give up and
  // switch to dropping old output instead.
  Serial.setTxTimeoutMs(3);

  ui_init();
  char ver[24];
  snprintf(ver, sizeof(ver), "firmware %d.%d", TK_FW_VERSION >> 8, TK_FW_VERSION & 0xFF);
  ui_splash(ver, "hold A: current agent - hold B: new agent");

  M5.Speaker.end();  // M5.begin starts the speaker; the mic needs the bus
  const bool audio_ok = audio_init();
  link_init();

  M5.update();
  const bool forget = M5.BtnB.isPressed();
  if (forget) link_forget_receiver();

  s_clip = (uint16_t)((esp_random() & 0x7FFF) + 1);  // don't reuse ids across reboots
  if (audio_ok) mic_start();                        // warm the codec up now
  link_want_radio(true);
  poll_battery(millis(), true);
  s_last_activity = millis();
  set_mode(Mode::Boot);
  if (!audio_ok)
    message("Audio error", "Could not start the microphone. Try a reset (side button).", 6000);
  else if (forget)
    message("Pairing reset", "Looking for a receiver...", 2500);

  Serial.printf("Claude Talkie stick %s, clips up to %lu s\n", ver, (unsigned long)clip_capacity_seconds());
}

void loop() {
  const uint32_t now = millis();
  const Buttons b = read_buttons();
  handle_buttons(b, now);
  handle_events(now);
  tick(now);
  mic_watchdog();
  manage_power(now, b);
  handle_serial();
  render(now);
  vTaskDelay(pdMS_TO_TICKS(5));
}
