#!/usr/bin/env bash
# Run the Claude Talkie listener in the background at login (launchd).
#   ./install-launch-agent.sh            install and start
#   ./install-launch-agent.sh uninstall  stop and remove
set -euo pipefail
cd "$(dirname "$0")"
DIR="$(pwd)"
LABEL="com.claude-talkie.listener"
PLIST="$HOME/Library/LaunchAgents/$LABEL.plist"
LOG_DIR="${TALKIE_HOME:-$HOME/.claude-talkie}"

if [ "${1:-}" = "uninstall" ]; then
  launchctl bootout "gui/$(id -u)/$LABEL" 2>/dev/null || true
  rm -f "$PLIST"
  echo "Removed $LABEL."
  exit 0
fi

[ -x "$DIR/.venv/bin/python" ] || { echo "Run ./setup.sh first." >&2; exit 1; }
CLAUDE_DIR="$(dirname "$(command -v claude 2>/dev/null || echo /usr/local/bin/claude)")"
mkdir -p "$HOME/Library/LaunchAgents" "$LOG_DIR"
cat > "$PLIST" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>Label</key><string>$LABEL</string>
  <key>ProgramArguments</key>
  <array>
    <string>$DIR/talkie.sh</string>
    <string>run</string>
  </array>
  <key>EnvironmentVariables</key>
  <dict>
    <key>PATH</key><string>$CLAUDE_DIR:/opt/homebrew/bin:/usr/local/bin:/usr/bin:/bin</string>
  </dict>
  <key>RunAtLoad</key><true/>
  <key>KeepAlive</key><true/>
  <key>StandardOutPath</key><string>$LOG_DIR/launchd.log</string>
  <key>StandardErrorPath</key><string>$LOG_DIR/launchd.log</string>
</dict>
</plist>
EOF
launchctl bootout "gui/$(id -u)/$LABEL" 2>/dev/null || true
launchctl bootstrap "gui/$(id -u)" "$PLIST"
echo "Installed and started $LABEL. Log: $LOG_DIR/talkie.log"
echo "If button A should paste into your terminal, macOS will ask to allow"
echo "Accessibility for Python the first time. Allow it, or use current_mode = \"tmux\"."
