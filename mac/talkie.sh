#!/usr/bin/env bash
# Runs the Claude Talkie Mac app from its virtualenv. Usage: ./talkie.sh run
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
if [ ! -x "$DIR/.venv/bin/python" ]; then
  echo "Run $DIR/setup.sh first." >&2
  exit 1
fi
export PYTHONPATH="$DIR${PYTHONPATH:+:$PYTHONPATH}"
exec "$DIR/.venv/bin/python" -m talkie "$@"
