#!/usr/bin/env bash
# Claude Talkie - Mac setup. Safe to run again.
#   ./setup.sh                      # default model: large-v3-turbo-q5_0 (~550 MB)
#   TALKIE_MODEL=base.en ./setup.sh # smaller, faster, a bit less accurate
set -euo pipefail
cd "$(dirname "$0")"
MODEL="${TALKIE_MODEL:-large-v3-turbo-q5_0}"
HOME_DIR="${TALKIE_HOME:-$HOME/.claude-talkie}"

say() { printf "\033[1m==> %s\033[0m\n" "$*"; }

# 1. Python 3.10+ in a virtualenv
PY=""
for c in python3.13 python3.12 python3.11 python3.10 python3; do
  if command -v "$c" >/dev/null 2>&1 && "$c" -c 'import sys; sys.exit(0 if sys.version_info >= (3, 10) else 1)'; then
    PY="$(command -v "$c")"
    break
  fi
done
if [ -z "$PY" ]; then
  echo "Python 3.10 or newer is required: brew install python" >&2
  exit 1
fi
say "Python: $PY"
"$PY" -m venv .venv
.venv/bin/pip install --quiet --upgrade pip
.venv/bin/pip install --quiet -r requirements.txt

# 2. whisper.cpp (speech to text, runs on the Mac's GPU)
if ! command -v whisper-cli >/dev/null 2>&1 && [ ! -x /opt/homebrew/bin/whisper-cli ] && [ ! -x /usr/local/bin/whisper-cli ]; then
  if command -v brew >/dev/null 2>&1; then
    say "Installing whisper-cpp with Homebrew"
    brew install whisper-cpp
  else
    echo "Homebrew not found. Install whisper.cpp yourself, or set backend = \"mlx\" in the config." >&2
  fi
fi

# 3. Whisper model
mkdir -p "$HOME_DIR/models"
MODEL_PATH="$HOME_DIR/models/ggml-$MODEL.bin"
if [ ! -s "$MODEL_PATH" ]; then
  say "Downloading Whisper model '$MODEL' (one time)"
  curl -L --fail --progress-bar -o "$MODEL_PATH.part" \
    "https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-$MODEL.bin"
  mv "$MODEL_PATH.part" "$MODEL_PATH"
fi

# 4. Config file
if [ ! -f "$HOME_DIR/config.toml" ]; then
  say "Writing $HOME_DIR/config.toml"
  sed "s#ggml-large-v3-turbo-q5_0.bin#ggml-$MODEL.bin#" config.example.toml > "$HOME_DIR/config.toml"
fi

chmod +x talkie.sh
say "Checking everything"
./talkie.sh doctor || true
cat <<EOF

Next:
  1. Flash the receiver and the stick (see the README).
  2. Start the listener:   $(pwd)/talkie.sh run
     Test without hardware: $(pwd)/talkie.sh simulate some-recording.wav --dry-run
  3. The first time it pastes into your terminal, macOS asks for Accessibility
     permission for the app running Talkie. Allow it.
EOF
