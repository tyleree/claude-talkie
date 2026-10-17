"""Settings: defaults here, overridden by ~/.claude-talkie/config.toml."""

from __future__ import annotations

import dataclasses
import os
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

try:
    import tomllib  # Python 3.11+
except ModuleNotFoundError:  # pragma: no cover
    import tomli as tomllib  # type: ignore

HOME_DIR = Path(os.environ.get("TALKIE_HOME", "~/.claude-talkie")).expanduser()

DEFAULT_FOCUSED_APPS = [
    "com.mitchellh.ghostty",
    "com.apple.Terminal",
    "com.googlecode.iterm2",
    "dev.warp.Warp-Stable",
    "net.kovidgoyal.kitty",
    "org.alacritty",
    "com.github.wez.wezterm",
    "com.microsoft.VSCode",
    "com.todesktop.230313mzl4w4u92",  # Cursor
    "dev.zed.Zed",
    "com.anthropic.claudefordesktop",  # Claude desktop app
]


@dataclass
class ReceiverConfig:
    port: str = "auto"  # "auto" or a path like /dev/cu.usbmodem1101


@dataclass
class TranscribeConfig:
    backend: str = "auto"  # auto | whisper-cpp | mlx | command
    whisper_bin: str = ""  # empty: search PATH and Homebrew
    model: str = str(HOME_DIR / "models" / "ggml-large-v3-turbo-q5_0.bin")
    mlx_model: str = "mlx-community/whisper-large-v3-turbo"
    language: str = "en"
    # Words Whisper should expect. Helps with names and jargon.
    prompt: str = "Claude, Claude Code, git, GitHub, pull request, commit, npm, Python, TypeScript."
    threads: int = 4
    command: list[str] = field(default_factory=list)  # backend=command, e.g. ["my-stt", "{wav}"]
    timeout: float = 120.0


@dataclass
class AudioConfig:
    min_seconds: float = 0.4
    speech_gate: bool = True  # skip clips with no speech (stops Whisper inventing text)
    speech_threshold_db: float = -48.0
    keep_clips: int = 20  # keep the last N recordings as WAV for debugging (0 = none)


@dataclass
class RouteConfig:
    claude_bin: str = "claude"
    prompt_prefix: str = ""
    # Button A: the agent you're looking at.
    current_mode: str = "focused"  # focused | tmux | last
    focused_apps: list[str] = field(default_factory=lambda: list(DEFAULT_FOCUSED_APPS))
    focused_submit: bool = True  # press Return after pasting
    focused_fallback: str = "clipboard"  # clipboard | last | new | none
    restore_clipboard: bool = True
    tmux_target: str = ""  # empty: the pane running claude, preferring the active one
    # Button B: a new background agent (claude --bg).
    new_cwd: str = "~"
    new_name: str = ""  # empty: Claude names it; may use {time} and {words}
    new_permission_mode: str = ""  # e.g. "acceptEdits"; empty uses your settings
    new_model: str = ""
    new_agent: str = ""
    extra_args: list[str] = field(default_factory=list)
    timeout: float = 90.0


@dataclass
class FeedbackConfig:
    notifications: bool = True  # macOS notification for each result
    agent_summary: bool = True  # append "2 working, 1 needs you" to results


@dataclass
class Config:
    receiver: ReceiverConfig = field(default_factory=ReceiverConfig)
    transcribe: TranscribeConfig = field(default_factory=TranscribeConfig)
    audio: AudioConfig = field(default_factory=AudioConfig)
    route: RouteConfig = field(default_factory=RouteConfig)
    feedback: FeedbackConfig = field(default_factory=FeedbackConfig)
    home: Path = HOME_DIR
    source: str = "defaults"
    warnings: list[str] = field(default_factory=list)

    @property
    def clips_dir(self) -> Path:
        return self.home / "clips"

    @property
    def history_path(self) -> Path:
        return self.home / "history.jsonl"

    @property
    def state_path(self) -> Path:
        return self.home / "state.json"


def expand(path: str) -> str:
    return os.path.expandvars(os.path.expanduser(path)) if path else path


def _apply(section: Any, values: dict[str, Any], prefix: str, warnings: list[str]) -> None:
    known = {f.name: f for f in dataclasses.fields(section)}
    for key, value in values.items():
        if key not in known:
            warnings.append(f"unknown setting {prefix}.{key} (ignored)")
            continue
        current = getattr(section, key)
        if isinstance(current, bool) and not isinstance(value, bool):
            warnings.append(f"{prefix}.{key} should be true or false")
            continue
        if isinstance(current, (int, float)) and not isinstance(current, bool):
            if not isinstance(value, (int, float)) or isinstance(value, bool):
                warnings.append(f"{prefix}.{key} should be a number")
                continue
            value = type(current)(value)
        if isinstance(current, list) and not isinstance(value, list):
            warnings.append(f"{prefix}.{key} should be a list")
            continue
        if isinstance(current, str) and not isinstance(value, str):
            warnings.append(f"{prefix}.{key} should be a string")
            continue
        setattr(section, key, value)


def load(path: str | os.PathLike[str] | None = None) -> Config:
    cfg = Config()
    p = Path(expand(str(path))) if path else HOME_DIR / "config.toml"
    if p.exists():
        with open(p, "rb") as f:
            data = tomllib.load(f)
        cfg.source = str(p)
        for name, values in data.items():
            section = getattr(cfg, name, None)
            if not dataclasses.is_dataclass(section) or not isinstance(values, dict):
                cfg.warnings.append(f"unknown section [{name}] (ignored)")
                continue
            _apply(section, values, name, cfg.warnings)
    elif path:
        raise FileNotFoundError(p)

    t, r = cfg.transcribe, cfg.route
    t.model = expand(t.model)
    t.whisper_bin = expand(t.whisper_bin)
    r.new_cwd = expand(r.new_cwd)
    r.claude_bin = expand(r.claude_bin)
    if t.backend not in ("auto", "whisper-cpp", "mlx", "command"):
        cfg.warnings.append(f"transcribe.backend '{t.backend}' unknown; using auto")
        t.backend = "auto"
    if r.current_mode not in ("focused", "tmux", "last"):
        cfg.warnings.append(f"route.current_mode '{r.current_mode}' unknown; using focused")
        r.current_mode = "focused"
    if r.focused_fallback not in ("clipboard", "last", "new", "none"):
        cfg.warnings.append(f"route.focused_fallback '{r.focused_fallback}' unknown; using clipboard")
        r.focused_fallback = "clipboard"
    return cfg
