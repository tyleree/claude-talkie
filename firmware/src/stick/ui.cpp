// Screen and sound for the stick. Everything is drawn into a full-screen
// sprite in PSRAM and pushed in one go, so updates never flicker.
#include <M5Unified.h>

#include "app.h"

namespace app {
namespace {

M5Canvas s_cv(&M5.Display);
bool s_sprite = false;
bool s_screen_on = true;

uint16_t C_BG, C_TEXT, C_DIM, C_FAINT, C_RED, C_GREEN, C_YELLOW, C_ORANGE, C_CURRENT, C_NEW;

LovyanGFX* gfx() { return s_sprite ? (LovyanGFX*)&s_cv : (LovyanGFX*)&M5.Display; }

uint16_t target_color(uint8_t t) { return t == TK_TARGET_NEW ? C_NEW : C_CURRENT; }
const char* target_name(uint8_t t) { return t == TK_TARGET_NEW ? "new agent" : "current agent"; }
const char* target_key(uint8_t t) { return t == TK_TARGET_NEW ? "B" : "A"; }

void text(LovyanGFX* g, const char* s, int x, int y, const lgfx::IFont* f, uint16_t color,
          textdatum_t datum) {
  g->setFont(f);
  g->setTextColor(color);
  g->setTextDatum(datum);
  g->drawString(s, x, y);
}

// Draw `s` truncated with "..." so it fits in `w` pixels.
void text_fit(LovyanGFX* g, const char* s, int x, int y, int w, const lgfx::IFont* f, uint16_t color,
              textdatum_t datum) {
  g->setFont(f);
  if (g->textWidth(s) <= w) {
    text(g, s, x, y, f, color, datum);
    return;
  }
  char buf[96];
  size_t n = strlen(s);
  if (n > sizeof(buf) - 4) n = sizeof(buf) - 4;
  while (n > 0) {
    memcpy(buf, s, n);
    strcpy(buf + n, "...");
    if (g->textWidth(buf) <= w) break;
    n--;
  }
  text(g, buf, x, y, f, color, datum);
}

// Simple word wrap. Returns the y below the last line drawn.
int text_wrap(LovyanGFX* g, const char* s, int x, int y, int w, int max_lines, int line_h,
              const lgfx::IFont* f, uint16_t color) {
  g->setFont(f);
  g->setTextColor(color);
  g->setTextDatum(top_left);
  char line[96];
  int lines = 0;
  const char* p = s;
  while (*p && lines < max_lines) {
    while (*p == ' ' || *p == '\n') p++;
    if (!*p) break;
    size_t len = 0;
    const char* best_end = p;
    const char* q = p;
    while (*q && *q != '\n') {
      const char* word_end = q;
      while (*word_end && *word_end != ' ' && *word_end != '\n') word_end++;
      size_t try_len = (size_t)(word_end - p);
      if (try_len >= sizeof(line) - 4) break;
      memcpy(line, p, try_len);
      line[try_len] = 0;
      if (g->textWidth(line) > w) break;
      best_end = word_end;
      len = try_len;
      q = word_end;
      while (*q == ' ') q++;
    }
    if (len == 0) {  // one very long word: hard cut
      len = 1;
      while (p[len] && p[len] != ' ' && len < sizeof(line) - 4) {
        memcpy(line, p, len + 1);
        line[len + 1] = 0;
        if (g->textWidth(line) > w) break;
        len++;
      }
      best_end = p + len;
    }
    memcpy(line, p, len);
    line[len] = 0;
    lines++;
    const char* rest = best_end;
    while (*rest == ' ' || *rest == '\n') rest++;
    if (lines == max_lines && *rest) {  // more text than room: ellipsis
      while (len > 0) {
        strcpy(line + len, "...");
        if (g->textWidth(line) <= w) break;
        line[--len] = 0;
      }
    }
    g->drawString(line, x, y);
    y += line_h;
    p = best_end;
  }
  return y;
}

void key_tag(LovyanGFX* g, int x, int y, const char* key, uint16_t color) {
  g->fillRoundRect(x, y - 8, 16, 16, 3, color);
  text(g, key, x + 8, y + 1, &fonts::DejaVu12, C_BG, middle_center);
}

void check_icon(LovyanGFX* g, int cx, int cy, uint16_t color) {
  g->drawWideLine(cx - 9, cy, cx - 3, cy + 7, 2.5f, color);
  g->drawWideLine(cx - 3, cy + 7, cx + 10, cy - 8, 2.5f, color);
}

void cross_icon(LovyanGFX* g, int cx, int cy, uint16_t color) {
  g->drawWideLine(cx - 8, cy - 8, cx + 8, cy + 8, 2.5f, color);
  g->drawWideLine(cx - 8, cy + 8, cx + 8, cy - 8, 2.5f, color);
}

void dots(LovyanGFX* g, int x, int y, uint32_t anim, uint16_t color) {
  int phase = (anim / 300) % 4;
  for (int i = 0; i < 3; ++i) g->fillCircle(x + i * 9, y, 2, i < phase ? color : C_FAINT);
}

void status_bar(LovyanGFX* g, const UiModel& m) {
  const int w = g->width();
  uint16_t dot = C_DIM;
  const char* label = "standby";
  if (m.link.radio_up) {
    if (!m.link.have_receiver) {
      dot = C_RED;
      label = "pairing";
    } else if (!m.link.linked) {
      dot = C_RED;
      label = "no receiver";
    } else if (!m.link.host_alive) {
      dot = C_YELLOW;
      label = "Mac app off";
    } else {
      dot = C_GREEN;
      label = "connected";
    }
  }
  g->fillCircle(7, 8, 4, dot);
  text(g, label, 15, 9, &fonts::DejaVu9, C_DIM, middle_left);
  g->setFont(&fonts::DejaVu9);
  int x = 17 + g->textWidth(label);
  if (m.link.radio_up && m.link.linked && m.link.rssi) {
    int bars = m.link.rssi >= -60 ? 4 : m.link.rssi >= -70 ? 3 : m.link.rssi >= -80 ? 2 : 1;
    for (int i = 0; i < 4; ++i) {
      int h = 3 + i * 2;
      g->fillRect(x + i * 4, 13 - h, 3, h, i < bars ? C_DIM : C_FAINT);
    }
    x += 18;
  }
  if (m.link.radio_up && m.link.lr) text(g, "LR", x + 2, 9, &fonts::DejaVu9, C_FAINT, middle_left);

  // battery, right aligned
  char pct[12];
  if (m.battery >= 0) {
    snprintf(pct, sizeof(pct), "%d%%", m.battery);
    text(g, pct, w - 2, 9, &fonts::DejaVu9, C_DIM, middle_right);
    g->setFont(&fonts::DejaVu9);
    int tw = g->textWidth(pct);
    int bx = w - 6 - tw - 20;
    g->drawRect(bx, 4, 18, 9, C_DIM);
    g->fillRect(bx + 18, 6, 2, 5, C_DIM);
    int fill = (m.battery * 14) / 100;
    uint16_t bc = m.charging ? C_YELLOW : (m.battery <= 15 ? C_RED : C_DIM);
    if (fill > 0) g->fillRect(bx + 2, 6, fill, 5, bc);
  } else if (m.usb) {
    text(g, "USB", w - 2, 9, &fonts::DejaVu9, C_DIM, middle_right);
  }
}

void level_meter(LovyanGFX* g, int x, int y, int w, int h, uint8_t level) {
  const int segs = 24;
  const int gap = 2;
  const int sw = (w - (segs - 1) * gap) / segs;
  int lit = (level * segs + 127) / 255;
  for (int i = 0; i < segs; ++i) {
    uint16_t c = C_FAINT;
    if (i < lit) c = i < segs * 6 / 10 ? C_GREEN : (i < segs * 85 / 100 ? C_YELLOW : C_RED);
    g->fillRect(x + i * (sw + gap), y, sw, h, c);
  }
}

void progress(LovyanGFX* g, int x, int y, int w, int h, uint32_t done, uint32_t total, uint32_t anim) {
  g->drawRoundRect(x, y, w, h, 3, C_FAINT);
  if (total) {
    int fw = (int)((uint64_t)(w - 4) * (done > total ? total : done) / total);
    if (fw > 0) g->fillRoundRect(x + 2, y + 2, fw, h - 4, 2, C_TEXT);
  } else {  // unknown total: sweeping block
    int bw = w / 5;
    int pos = (int)((anim / 12) % (uint32_t)(w - 4 - bw));
    g->fillRoundRect(x + 2 + pos, y + 2, bw, h - 4, 2, C_DIM);
  }
}

void draw_idle(LovyanGFX* g, const UiModel& m) {
  const int w = g->width();
  if (!m.link.have_receiver) {
    text(g, "Pairing", w / 2, 44, &fonts::DejaVu24, C_TEXT, middle_center);
    text(g, "plug the receiver into your Mac", w / 2, 70, &fonts::DejaVu9, C_DIM, middle_center);
  } else {
    text(g, "Ready", w / 2, 44, &fonts::DejaVu24, C_TEXT, middle_center);
    const char* hint = "";
    if (m.link.radio_up && !m.link.linked)
      hint = "looking for the receiver...";
    else if (m.link.linked && !m.link.host_alive)
      hint = "receiver OK - start the Talkie app on your Mac";
    text_fit(g, hint, w / 2, 70, w - 16, &fonts::DejaVu9, C_DIM, middle_center);
  }
  key_tag(g, 22, 94, "A", C_CURRENT);
  text(g, "hold: current agent", 46, 95, &fonts::DejaVu12, C_TEXT, middle_left);
  key_tag(g, 22, 118, "B", C_NEW);
  text(g, "hold: new agent", 46, 119, &fonts::DejaVu12, C_TEXT, middle_left);
}

void draw_rec(LovyanGFX* g, const UiModel& m) {
  const int w = g->width();
  const uint16_t tc = target_color(m.target);
  g->fillRect(0, 19, w, 3, m.mic_ready ? C_RED : C_ORANGE);

  char t[16];
  uint32_t s = m.rec_ms / 1000;
  snprintf(t, sizeof(t), "%lu:%02lu", (unsigned long)(s / 60), (unsigned long)(s % 60));
  uint16_t clock = m.mic_ready ? C_RED : C_ORANGE;
  text(g, t, w / 2 + 10, 52, &fonts::DejaVu40, clock, middle_center);
  if (((m.anim / 500) & 1) == 0 || !m.mic_ready) {
    g->setFont(&fonts::DejaVu40);
    int tw = g->textWidth(t);
    g->fillCircle(w / 2 + 10 - tw / 2 - 16, 52, 7, clock);
  }
  if (m.mode == Mode::RecLocked) text(g, "LOCKED", w - 6, 30, &fonts::DejaVu9, C_YELLOW, top_right);

  key_tag(g, 14, 87, target_key(m.target), tc);
  text(g, target_name(m.target), 38, 88, &fonts::DejaVu18, tc, middle_left);
  level_meter(g, 12, 102, w - 24, 9, m.mic_ready ? m.level : 0);

  const char* hint;
  char buf[48];
  if (!m.mic_ready) {
    hint = "starting mic - talk when the timer is red";
  } else if (m.mode == Mode::RecPending) {
    hint = "tap again to lock recording on";
  } else if (m.mode == Mode::RecLocked) {
    snprintf(buf, sizeof(buf), "tap %s to send - %s cancels", target_key(m.target),
             m.target == TK_TARGET_NEW ? "A" : "B");
    hint = buf;
  } else {
    snprintf(buf, sizeof(buf), "release to send - %s cancels", m.target == TK_TARGET_NEW ? "A" : "B");
    hint = buf;
  }
  text_fit(g, hint, w / 2, 125, w - 8, &fonts::DejaVu9, C_DIM, middle_center);
}

void draw_sending(LovyanGFX* g, const UiModel& m) {
  const int w = g->width();
  text(g, "Sending", w / 2 - 14, 44, &fonts::DejaVu24, C_TEXT, middle_center);
  g->setFont(&fonts::DejaVu24);
  dots(g, w / 2 - 14 + g->textWidth("Sending") / 2 + 8, 50, m.anim, C_TEXT);
  progress(g, 20, 68, w - 40, 12, m.link.tx_sent, m.link.tx_total, m.anim);
  key_tag(g, 22, 100, target_key(m.target), target_color(m.target));
  text(g, target_name(m.target), 46, 101, &fonts::DejaVu12, target_color(m.target), middle_left);
  if (!m.link.linked)
    text(g, "receiver not answering...", w / 2, 124, &fonts::DejaVu9, C_RED, middle_center);
}

void draw_waiting(LovyanGFX* g, const UiModel& m) {
  const int w = g->width();
  const char* l1 = (m.line1 && *m.line1) ? m.line1 : "Transcribing";
  text_fit(g, l1, w / 2, 46, w - 40, &fonts::DejaVu18, C_TEXT, middle_center);
  dots(g, w / 2 - 9, 72, m.anim, C_TEXT);
  key_tag(g, 22, 100, target_key(m.target), target_color(m.target));
  text(g, target_name(m.target), 46, 101, &fonts::DejaVu12, target_color(m.target), middle_left);
  text(g, "press A or B to talk again", w / 2, 124, &fonts::DejaVu9, C_DIM, middle_center);
}

void draw_result(LovyanGFX* g, const UiModel& m) {
  const int w = g->width();
  uint16_t c = m.ok ? C_GREEN : C_RED;
  if (m.ok)
    check_icon(g, 18, 40, c);
  else
    cross_icon(g, 18, 40, c);
  text_fit(g, m.line1 ? m.line1 : "", 36, 41, w - 42, &fonts::DejaVu18, c, middle_left);
  if (m.line2 && *m.line2) text_wrap(g, m.line2, 8, 62, w - 16, 5, 14, &fonts::DejaVu12, C_TEXT);
}

void draw_failed(LovyanGFX* g, const UiModel& m) {
  const int w = g->width();
  cross_icon(g, 18, 40, C_RED);
  text(g, "Not delivered", 36, 41, &fonts::DejaVu18, C_RED, middle_left);
  const char* why = !m.link.linked ? "The receiver didn't answer. Is it plugged in and in range?"
                                   : "The Mac didn't confirm. Is the Talkie app running?";
  text_wrap(g, why, 8, 60, w - 16, 3, 13, &fonts::DejaVu9, C_DIM);
  key_tag(g, 18, 119, "A", C_CURRENT);
  text(g, "retry", 40, 120, &fonts::DejaVu12, C_TEXT, middle_left);
  key_tag(g, w / 2 + 10, 119, "B", C_NEW);
  text(g, "discard", w / 2 + 32, 120, &fonts::DejaVu12, C_TEXT, middle_left);
}

void draw_message(LovyanGFX* g, const UiModel& m) {
  const int w = g->width();
  text_fit(g, m.line1 ? m.line1 : "", w / 2, 58, w - 16, &fonts::DejaVu18, C_TEXT, middle_center);
  if (m.line2 && *m.line2) text_wrap(g, m.line2, 10, 80, w - 20, 3, 13, &fonts::DejaVu9, C_DIM);
}

struct Tone {
  uint16_t hz;
  uint16_t ms;
};

void play(const Tone* tones, int n) {
  bool mic_was = mic_running();
  if (mic_was) mic_stop();  // mic and speaker share the audio bus
  M5.Speaker.begin();
  M5.Speaker.setVolume(TK_SPEAKER_VOLUME);
  for (int i = 0; i < n; ++i) {
    if (tones[i].hz) {
      M5.Speaker.tone(tones[i].hz, tones[i].ms);
      uint32_t until = millis() + tones[i].ms + 60;
      delay(5);
      while (M5.Speaker.isPlaying() && millis() < until) delay(2);
    } else {
      delay(tones[i].ms);
    }
  }
  M5.Speaker.end();
  if (mic_was) mic_start();
}

}  // namespace

void ui_init() {
  M5.Display.setRotation(TK_ROTATION);
  M5.Display.setBrightness(TK_BRIGHTNESS);
  auto& d = M5.Display;
  C_BG = d.color565(0, 0, 0);
  C_TEXT = d.color565(240, 240, 240);
  C_DIM = d.color565(150, 150, 150);
  C_FAINT = d.color565(55, 55, 55);
  C_RED = d.color565(235, 64, 52);
  C_GREEN = d.color565(70, 205, 100);
  C_YELLOW = d.color565(245, 195, 50);
  C_ORANGE = d.color565(255, 145, 30);
  C_CURRENT = d.color565(90, 175, 255);
  C_NEW = d.color565(217, 119, 87);  // Claude terracotta
  s_cv.setColorDepth(16);
  s_cv.setPsram(true);
  s_sprite = s_cv.createSprite(d.width(), d.height()) != nullptr;
}

void ui_splash(const char* l1, const char* l2) {
  LovyanGFX* g = gfx();
  g->fillScreen(C_BG);
  const int w = g->width();
  text(g, "Claude Talkie", w / 2, 50, &fonts::DejaVu24, C_NEW, middle_center);
  if (l1) text(g, l1, w / 2, 82, &fonts::DejaVu12, C_TEXT, middle_center);
  if (l2) text(g, l2, w / 2, 104, &fonts::DejaVu9, C_DIM, middle_center);
  if (s_sprite) s_cv.pushSprite(0, 0);
}

void ui_render(const UiModel& m) {
  if (!s_screen_on) return;
  LovyanGFX* g = gfx();
  g->startWrite();
  g->fillScreen(C_BG);
  status_bar(g, m);
  switch (m.mode) {
    case Mode::Boot:
    case Mode::Idle:
      draw_idle(g, m);
      break;
    case Mode::Rec:
    case Mode::RecPending:
    case Mode::RecLocked:
      draw_rec(g, m);
      break;
    case Mode::Finishing:
    case Mode::Sending:
      draw_sending(g, m);
      break;
    case Mode::Waiting:
      draw_waiting(g, m);
      break;
    case Mode::Result:
      draw_result(g, m);
      break;
    case Mode::Failed:
      draw_failed(g, m);
      break;
    case Mode::Message:
      draw_message(g, m);
      break;
  }
  g->endWrite();
  if (s_sprite) s_cv.pushSprite(0, 0);
}

void ui_screen(bool on) {
  if (on == s_screen_on) return;
  s_screen_on = on;
  if (on) {
    M5.Display.wakeup();
    M5.Display.setBrightness(TK_BRIGHTNESS);
  } else {
    M5.Display.setBrightness(0);
    M5.Display.sleep();
  }
}

void ui_beep(Beep b) {
  static const Tone start[] = {{1500, 60}};
  static const Tone sent[] = {{1200, 50}, {0, 30}, {1800, 70}};
  static const Tone ok[] = {{2000, 40}, {0, 40}, {2600, 60}};
  static const Tone err[] = {{420, 200}};
  switch (b) {
    case Beep::Start:
      play(start, 1);
      break;
    case Beep::Sent:
      play(sent, 3);
      break;
    case Beep::Ok:
      play(ok, 3);
      break;
    case Beep::Error:
      play(err, 1);
      break;
  }
}

}  // namespace app
