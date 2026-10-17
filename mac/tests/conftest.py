import os
import stat
import sys
import textwrap
from pathlib import Path

import numpy as np
import pytest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

# Keep tests away from the real ~/.claude-talkie
os.environ.setdefault("TALKIE_HOME", str(Path(os.environ.get("TMPDIR", "/tmp")) / "talkie-test-home"))


def speechlike(seconds: float = 2.0, rate: int = 16000, level: float = 0.3, seed: int = 0) -> np.ndarray:
    """A voice-ish test signal: harmonics with a syllable-rate envelope plus a little noise."""
    rng = np.random.default_rng(seed)
    t = np.arange(int(seconds * rate)) / rate
    f0 = 140 + 30 * np.sin(2 * np.pi * 0.7 * t)
    phase = 2 * np.pi * np.cumsum(f0) / rate
    x = sum(np.sin(k * phase) / k for k in range(1, 8))
    env = 0.5 * (1 + np.sin(2 * np.pi * 3.5 * t)) ** 2
    x = x * env
    x = x / np.max(np.abs(x)) * level
    x += rng.normal(0, 0.003, len(t))
    return np.clip(x * 32767, -32768, 32767).astype(np.int16)


@pytest.fixture
def fake_claude(tmp_path):
    """A stand-in `claude` that logs its arguments and prints what Claude Code prints."""
    log = tmp_path / "claude.log"
    script = tmp_path / "claude"
    script.write_text(textwrap.dedent(f"""\
        #!/bin/bash
        printf '%s\\n' "$*" >> "{log}"
        if [[ "$1" == "agents" ]]; then
          echo '[{{"id":"1234abcd","sessionId":"0f0e-uuid","state":"working","kind":"background","name":"fix-the-tests"}},
                 {{"id":"99999999","sessionId":"other","state":"blocked","kind":"background"}}]'
          exit 0
        fi
        if [[ "$FAKE_CLAUDE_FAIL" == "trust" ]]; then
          echo "Error: Workspace not trusted" >&2; exit 1
        fi
        echo "backgrounded · 1234abcd · fix-the-tests"
        echo "  claude agents             list sessions"
        """))
    script.chmod(script.stat().st_mode | stat.S_IEXEC)
    return script, log
