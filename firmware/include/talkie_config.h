// Claude Talkie - firmware settings shared by the stick and the receiver.
//
// Both devices MUST be built with the same radio settings (channel, long
// range, rate) and the same generated include/talkie_secret.h, or they will
// not hear each other. Change a value here, then rebuild and flash BOTH.
#pragma once

// ---------------------------------------------------------------- radio ----

// 2.4 GHz channel used by both devices (1-13). Neither device joins a Wi-Fi
// network; this only has to match between the stick and the receiver. If
// your office Wi-Fi is very busy on this channel, try 6 or 11.
#define TK_WIFI_CHANNEL 1

// Espressif Long Range (LR) mode: much better range, about a third of the
// bandwidth (still plenty for compressed voice). Set to 0 on BOTH devices if
// the link never comes up, to fall back to normal 1 Mbps ESP-NOW.
#define TK_LONG_RANGE 1

// LR data rate: 1 = 500 kbps (default, faster), 0 = 250 kbps (longest range).
#define TK_LR_500K 1

// Maximum transmit power in 0.25 dBm units (84 = 21 dBm, the chip maximum).
#define TK_TX_POWER_QDBM 84

// ---------------------------------------------------------------- audio ----

#define TK_SAMPLE_RATE 16000
// Samples per radio packet (4-bit ADPCM, so bytes = samples / 2).
// 456 samples = 28.5 ms of audio in a 240-byte packet.
#define TK_SAMPLES_PER_PACKET 456
// Longest clip the stick will record before sending automatically.
#define TK_MAX_CLIP_SECONDS 300
// Audio kept from just before you press the button, so a word you start
// saying as you press is not clipped.
#define TK_PREROLL_MS 300
// Software mic gain (M5Unified "magnification"). 8 suits talking into the
// stick from a hand's length; raise to 12-16 if transcripts miss quiet speech.
#define TK_MIC_MAGNIFICATION 8

// ------------------------------------------------------------- feedback ----

// Speaker volume 0-255. M5Stack advises staying under ~75% (190) on battery.
#define TK_SPEAKER_VOLUME 120
// The mic and speaker share one audio bus, so every beep stops the mic, and
// the ES8311 codec then needs about a second to warm up again (the screen
// shows "starting mic" until it has). Beeps also pause the stick for ~0.3 s.
// So by default the stick only beeps when something went wrong; the screen
// and the Mac's notifications show the rest.
#define TK_BEEP_ON_START 0
#define TK_BEEP_ON_SEND 0
#define TK_BEEP_ON_SUCCESS 0
#define TK_BEEP_ON_ERROR 1

// Screen rotation: 1 or 3 = landscape (either way up), 0 or 2 = portrait.
#define TK_ROTATION 1
#define TK_BRIGHTNESS 110

// --------------------------------------------------------------- buttons ---

// A press shorter than this is a "tap"; two taps lock recording on.
#define TK_TAP_MAX_MS 350
#define TK_DOUBLE_TAP_MS 400
// Clips shorter than this are discarded as accidental presses.
#define TK_MIN_CLIP_MS 300

// ----------------------------------------------------------------- power ---

#define TK_SCREEN_TIMEOUT_MS 15000
// Radio stays on this long after the last activity so results can arrive.
#define TK_RADIO_IDLE_MS 20000
// Mic keeps running this long after the last activity so the next press
// records instantly (with pre-roll). After that the first press needs ~1 s
// of codec warm-up (the screen shows "starting" until the mic is live).
#define TK_MIC_KEEPALIVE_MS 180000
// Light-sleep between presses when idle on battery (wakes on either button).
#define TK_LIGHT_SLEEP 1

// ---------------------------------------------------------------- timing ---

#define TK_RESULT_SHOW_MS 8000
#define TK_RESULT_TIMEOUT_MS 90000
#define TK_END_RETRY_MS 400
#define TK_END_TIMEOUT_MS 8000
#define TK_HELLO_INTERVAL_MS 300
#define TK_PING_INTERVAL_MS 3000
// The receiver reports "Mac listener alive" if it heard from it this recently.
#define TK_HOST_ALIVE_MS 6000

// -------------------------------------------------------------- versions ---

#define TK_FW_VERSION 0x0100  // 1.0
