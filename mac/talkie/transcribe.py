"""Speech to text, entirely on the Mac.

Backends:
  whisper-cpp  `brew install whisper-cpp` + a ggml model (setup.sh fetches one)
  mlx          `pip install mlx-whisper` (Apple Silicon); downloads its model on first use
  command      any program that prints a transcript for "{wav}"
"""

from __future__ import annotations

import logging
import os
import re
import shutil
import subprocess
from pathlib import Path
from typing import Optional, Protocol

from .config import TranscribeConfig

log = logging.getLogger("talkie.transcribe")

HOMEBREW_DIRS = ["/opt/homebrew/bin", "/usr/local/bin"]

# Things Whisper emits for silence or noise instead of words.
_JUNK = re.compile(
    r"^\s*[\[\(]\s*(blank[_ ]audio|silence|music|noise|inaudible|applause|laughter|no speech)\s*[\]\)]\s*$",
    re.IGNORECASE,
)


class TranscriptionError(RuntimeError):
    pass


class Transcriber(Protocol):
    name: str

    def transcribe(self, wav: Path) -> str: ...


def clean(text: str) -> str:
    lines = []
    for line in text.splitlines():
        line = line.strip()
        if not line or _JUNK.match(line):
            continue
        # whisper.cpp occasionally keeps timestamps if flags change between versions
        line = re.sub(r"^\[\d\d:\d\d[:.\d]*\s*-->\s*\d\d:\d\d[:.\d]*\]\s*", "", line)
        lines.append(line)
    out = " ".join(lines)
    out = re.sub(r"\s+", " ", out).strip()
    return "" if _JUNK.match(out) else out


def find_whisper_bin(configured: str = "") -> Optional[str]:
    if configured:
        return configured if os.path.exists(configured) else shutil.which(configured)
    for name in ("whisper-cli", "whisper-cpp"):
        found = shutil.which(name)
        if found:
            return found
        for d in HOMEBREW_DIRS:
            p = os.path.join(d, name)
            if os.path.exists(p):
                return p
    return None


class WhisperCpp:
    name = "whisper-cpp"

    def __init__(self, cfg: TranscribeConfig) -> None:
        self.cfg = cfg
        self.bin = find_whisper_bin(cfg.whisper_bin)
        if not self.bin:
            raise TranscriptionError("whisper-cli not found (brew install whisper-cpp)")
        if not os.path.exists(cfg.model):
            raise TranscriptionError(f"Whisper model not found at {cfg.model} (run ./setup.sh)")

    def transcribe(self, wav: Path) -> str:
        cmd = [self.bin, "-m", self.cfg.model, "-f", str(wav), "-nt", "-np", "-t", str(self.cfg.threads)]
        if self.cfg.language:
            cmd += ["-l", self.cfg.language]
        if self.cfg.prompt:
            cmd += ["--prompt", self.cfg.prompt]
        try:
            r = subprocess.run(cmd, capture_output=True, text=True, timeout=self.cfg.timeout)
        except subprocess.TimeoutExpired:
            raise TranscriptionError("whisper timed out")
        if r.returncode != 0:
            tail = (r.stderr or r.stdout).strip().splitlines()[-1:] or ["no output"]
            raise TranscriptionError(f"whisper failed: {tail[0][:160]}")
        return clean(r.stdout)


class MlxWhisper:
    name = "mlx"

    def __init__(self, cfg: TranscribeConfig) -> None:
        try:
            import mlx_whisper  # noqa: F401
        except ImportError:
            raise TranscriptionError("mlx-whisper is not installed (pip install mlx-whisper)")
        self.cfg = cfg

    def transcribe(self, wav: Path) -> str:
        import mlx_whisper

        kwargs = {"path_or_hf_repo": self.cfg.mlx_model}
        if self.cfg.language:
            kwargs["language"] = self.cfg.language
        if self.cfg.prompt:
            kwargs["initial_prompt"] = self.cfg.prompt
        try:
            result = mlx_whisper.transcribe(str(wav), **kwargs)
        except Exception as e:  # mlx raises a variety of things
            raise TranscriptionError(f"mlx-whisper failed: {e}")
        return clean(str(result.get("text", "")))


class CommandTranscriber:
    name = "command"

    def __init__(self, cfg: TranscribeConfig) -> None:
        if not cfg.command:
            raise TranscriptionError("transcribe.command is empty")
        self.cfg = cfg

    def transcribe(self, wav: Path) -> str:
        cmd = [part.replace("{wav}", str(wav)) for part in self.cfg.command]
        if not any("{wav}" in part for part in self.cfg.command):
            cmd.append(str(wav))
        try:
            r = subprocess.run(cmd, capture_output=True, text=True, timeout=self.cfg.timeout)
        except FileNotFoundError:
            raise TranscriptionError(f"command not found: {cmd[0]}")
        except subprocess.TimeoutExpired:
            raise TranscriptionError("transcription command timed out")
        if r.returncode != 0:
            raise TranscriptionError(f"transcription command failed: {(r.stderr or '').strip()[:160]}")
        return clean(r.stdout)


def make_transcriber(cfg: TranscribeConfig) -> Transcriber:
    if cfg.backend == "whisper-cpp":
        return WhisperCpp(cfg)
    if cfg.backend == "mlx":
        return MlxWhisper(cfg)
    if cfg.backend == "command":
        return CommandTranscriber(cfg)
    errors = []
    for factory in (WhisperCpp, MlxWhisper):
        try:
            return factory(cfg)
        except TranscriptionError as e:
            errors.append(str(e))
    raise TranscriptionError("no speech-to-text engine is ready: " + "; ".join(errors))
