# Claude Talkie

Push-to-talk for Claude Code. Hold a button on an M5StickS3, say what you
want, let go. Your words land in the Claude Code session you're looking at
(button A) or start a new background agent (button B). The stick's screen
shows what happened.

```
 M5StickS3  ── ESP-NOW long range ──▶  XIAO ESP32-C3  ── USB ──▶  Mac: talkie  ──▶  whisper.cpp  ──▶  Claude Code
 mic, 2 buttons,   (encrypted, no Wi-Fi   (receiver dongle)          (listener)       (on-device       A: paste into the
 screen, battery    network, no VPN)                                                   speech-to-text)     front terminal
                                                                                                        B: claude --bg
```

No Wi-Fi network, VPN, Bluetooth pairing or cloud speech service is involved.
The stick talks straight to its own USB receiver, speech is transcribed on
your Mac, and only the final text goes to Claude Code.

## What you need

| Part | Price | Notes |
|---|---|---|
| M5Stack M5StickS3 | ~$32 | ESP32-S3, mic, speaker, screen, 250 mAh battery |
| Seeed Studio XIAO ESP32C3 | ~$10 | The receiver. Snap the included antenna onto its U.FL connector (press straight down, gently) |
| USB-C cable(s) | | Both boards are flashed over USB-C |
| A Mac with Homebrew and Claude Code | | Apple Silicon recommended (fast Whisper) |

## 1. Flash the firmware (once)

```bash
brew install platformio          # or: pipx install platformio
cd firmware
pio run                          # builds both; first run downloads toolchains (~5 min)
```

The first build creates `firmware/include/talkie_secret.h`, a random key the
stick and receiver share to encrypt and authenticate their radio link.
**Flash both devices from this same folder** so they get the same key.

Plug in **only the receiver**, then:

```bash
pio run -e receiver -t upload
```

Unplug it, plug in **only the stick**, then:

```bash
pio run -e stick -t upload
```

If an upload can't find or talk to the board:
- StickS3: hold its power button (the side button next to the USB-C port) until the green LED inside flashes. That's download mode; upload again, then press the power button once to restart.
- XIAO: hold BOOT while plugging it in, upload, then press RESET.

Plug the receiver into the Mac and leave it there.

## 2. Set up the Mac app

```bash
cd mac
./setup.sh          # venv, whisper.cpp (Homebrew), Whisper model, ~/.claude-talkie/config.toml
./talkie.sh run     # start listening
```

`setup.sh` downloads the `large-v3-turbo-q5_0` Whisper model (~550 MB),
which is accurate and fast on Apple Silicon. For a smaller one:
`TALKIE_MODEL=base.en ./setup.sh`.

Run `./talkie.sh doctor` any time to check the setup.

**Accessibility permission.** Button A pastes into your terminal by sending
Cmd-V and Return. The first time, macOS asks to allow Accessibility for the
app running Talkie (Ghostty, Terminal, ...). Allow it in System Settings >
Privacy & Security > Accessibility. If your work Mac blocks that, use
`current_mode = "tmux"` (see below), which needs no permission.

**Trust the new-agent folder.** Button B runs `claude --bg` in `new_cwd`
(your home folder by default). Run `claude` there once and accept the trust
prompt, or background agents can't start.

To keep the listener running in the background at login:
`./install-launch-agent.sh` (remove with `./install-launch-agent.sh uninstall`).

## 3. Use it

The stick: **A** is the blue button under the screen, **B** is the button on
the long side. The mic is at the USB-C end; talk toward it from a hand's
length or closer.

| Do this | What happens |
|---|---|
| Hold **A**, talk, release | Sends to the agent you're looking at |
| Hold **B**, talk, release | Starts a new background agent |
| Double-tap **A** or **B** | Locks recording on (hands-free); tap the same button again to send |
| Press the other button while recording | Cancels |
| **A** after "Not delivered" | Sends the clip again (B discards it) |
| Hold **B** while powering on | Forgets the paired receiver (re-pairs automatically) |
| Side power button: press / double-press | On / off |

The screen turns red with a running timer while recording. If it's orange
and says "starting mic", the audio codec is still warming up (after the
stick has slept, or for a moment after a beep); wait for red before talking.
After you let go: **Sending** → **Transcribing** → a green check with what
Claude got, or a red X with the reason. Results also appear as macOS
notifications. The stick beeps only when something went wrong; each beep
briefly restarts the mic (they share the audio bus), so send and success
beeps are off by default (`TK_BEEP_ON_*` in `talkie_config.h`).

Clips can be up to 5 minutes. Audio from ~0.3 s before you pressed is
included, so you can start talking as you press.

The top bar shows the link: green "connected", yellow "Mac app off" (the
receiver is there but `talkie.sh run` isn't), red "no receiver", grey
"standby" (radio asleep to save battery; any press wakes it).

## Where your words go

**Button A (`route.current_mode`)**

- `focused` (default): if the frontmost app is a terminal, editor or the Claude
  app on the `focused_apps` list, paste the transcript and press Return. If
  something else is in front (Slack, a browser), nothing is typed: the text
  goes on the clipboard and the stick says so (`focused_fallback` can instead
  send it to the last agent or a new one). If Claude is busy, Claude Code
  queues the message like anything else you type.
- `tmux`: types into the tmux pane running `claude` (the active one first),
  or `tmux_target`.
- `last`: continues the most recent agent Talkie started, with
  `claude --resume <session> --bg`.

**Button B** starts `claude --bg "<what you said>"` in `new_cwd`. It shows up
in `claude agents` like any other background agent. Optional:
`new_permission_mode`, `new_model`, `new_agent`, `new_name`.

Everything is in `~/.claude-talkie/config.toml` (commented copy in
`mac/config.example.toml`).

## Try it before the hardware arrives

```bash
./talkie.sh simulate some-recording.wav --dry-run   # full pipeline: stick codec, radio loss + repair, Whisper
./talkie.sh simulate some-recording.wav --new       # ...and actually start a background agent
./talkie.sh send --current "list the files here"    # test routing (gives you 3 s to focus a terminal)
./talkie.sh transcribe some-recording.wav           # just speech-to-text
```

`simulate` takes any 16-bit WAV. macOS can record one with QuickTime and
`afconvert -f WAVE -d LEI16@16000 -c 1 in.m4a out.wav`.

## Troubleshooting

| Symptom | Fix |
|---|---|
| Stick shows "no receiver" | Receiver plugged in? Both flashed from the same `firmware` folder? Then set `TK_LONG_RANGE 0` in `firmware/include/talkie_config.h` and reflash **both** (rules out long-range mode). |
| Stick shows "Mac app off" | Start `./talkie.sh run`. `./talkie.sh ports` shows whether the Mac sees the receiver. |
| "Allow Accessibility" | System Settings > Privacy & Security > Accessibility: enable the app running Talkie. The text is on the clipboard meanwhile. |
| "Didn't hear you" | Talk closer to the USB-C end, raise `TK_MIC_MAGNIFICATION` (firmware), or lower `speech_threshold_db` (Mac). |
| Words come out wrong | Add names and jargon to `transcribe.prompt`; use the large model; recordings are in `~/.claude-talkie/clips` to listen to. |
| "Folder not trusted yet" | Run `claude` in `new_cwd` once and accept. |
| Screen upside down | Set `TK_ROTATION` to 3 (or 1) and reflash the stick. |
| No macOS notifications | They come from `osascript`: allow notifications for Script Editor in System Settings > Notifications, or set `notifications = false`. |
| Anything else | `~/.claude-talkie/talkie.log`, `./talkie.sh run -v`, and `pio device monitor` for the device logs. |

## Battery

Rough guide for the 250 mAh cell: the mic stays warm for 3 minutes after
you use it (instant start), the radio stays on 20 s after activity (so
results can arrive), the screen goes dark after 15 s, and then the stick
light-sleeps until you press a button. Double-press the power button to turn
it fully off. Timings are in `talkie_config.h`.

## Security and privacy

- The radio link is ESP-NOW with AES-CCMP encryption. Keys come from the
  random secret generated on your machine; pairing broadcasts are
  HMAC-authenticated, so the stick only pairs with your receiver.
- Speech-to-text runs locally (whisper.cpp). Audio never leaves your Mac;
  only the transcript goes to Claude Code. Recent clips are kept in
  `~/.claude-talkie/clips` for debugging (`keep_clips = 0` to disable), and
  transcripts are logged in `~/.claude-talkie/history.jsonl` and `talkie.log`.
- Button A only pastes into apps on your allow-list.
- On a managed work laptop, check policy before plugging in a USB radio or
  granting Accessibility.

## How it's built

```
firmware/
  include/talkie_protocol.h   packet formats (mirrored in mac/talkie/protocol.py)
  include/talkie_config.h     radio, audio, button and power settings
  include/talkie_adpcm.h      4-bit IMA ADPCM encoder (16 kHz voice in 32 kbit/s)
  include/talkie_crypto.h     key derivation + HMAC
  src/stick/                  M5StickS3: audio.cpp (mic, pre-roll, encoder),
                              link.cpp (ESP-NOW, pairing, resend), ui.cpp, main.cpp (state machine, power)
  src/receiver/main.cpp       XIAO ESP32-C3: radio <-> USB serial bridge
  tools/vectors.c             host build of the encoder, used by the Mac tests
mac/
  talkie/                     link (serial), clips (reassembly + NACK repair), audio,
                              transcribe, router, agents (claude CLI), service, cli
  tests/                      pytest suite
```

The stick encodes audio into 240-byte packets (28.5 ms each) and streams
them as it records. Every packet carries its own decoder state, so a lost
packet costs only its 28 ms; at the end the Mac asks for anything missing,
then acknowledges the clip and reports progress back to the stick's screen.

## Status

- Both firmwares build with pinned versions (espressif32 6.12.0, M5Unified
  0.2.26, M5GFX 0.2.33).
- The Mac app has an automated test suite
  (`cd mac && .venv/bin/pip install -r requirements-dev.txt && .venv/bin/python -m pytest`),
  including a bit-exact check of the stick's encoder (compiled from the
  firmware headers) against the Mac decoder.
- End-to-end simulation with real whisper.cpp on a speech sample, with 25%
  of radio packets dropped, transcribed word-for-word.
- **Not yet run on real hardware.** Things most likely to need a tweak on
  first contact: mic gain (`TK_MIC_MAGNIFICATION`), long-range mode on the
  C3 (`TK_LONG_RANGE`), and screen rotation.

## Ideas for later

- Approve Claude Code permission prompts from the stick (the Claude desktop
  app's Hardware Buddy BLE protocol, or a Claude Code channel with permission
  relay for terminal sessions).
- Show background agents' progress on the stick between clips.

## License

MIT. See [LICENSE](LICENSE).
