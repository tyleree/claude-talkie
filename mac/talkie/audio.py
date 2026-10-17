"""Audio clean-up before transcription."""

from __future__ import annotations

import math
import wave
from dataclasses import dataclass
from pathlib import Path

import numpy as np

FRAME_S = 0.02


@dataclass
class AudioStats:
    seconds: float
    peak_db: float
    speech_seconds: float
    noise_db: float

    def describe(self) -> str:
        return (f"{self.seconds:.1f}s, peak {self.peak_db:.0f} dBFS, "
                f"speech {self.speech_seconds:.1f}s, noise {self.noise_db:.0f} dBFS")


def _db(x: float) -> float:
    return 20.0 * math.log10(max(x, 1e-9))


def analyze(pcm: np.ndarray, sample_rate: int, threshold_db: float = -48.0) -> AudioStats:
    x = pcm.astype(np.float32) / 32768.0
    if len(x) == 0:
        return AudioStats(0.0, -180.0, 0.0, -180.0)
    x = x - float(np.mean(x))
    n = max(1, int(sample_rate * FRAME_S))
    frames = len(x) // n
    if frames == 0:
        rms = np.array([float(np.sqrt(np.mean(x * x)))])
    else:
        rms = np.sqrt(np.mean(x[: frames * n].reshape(frames, n) ** 2, axis=1))
    noise = float(np.percentile(rms, 20))
    # A frame counts as speech if it is loud in absolute terms and well above
    # the background (the stick's mic gain is fixed, so absolute levels mean
    # something here).
    thresh = max(10 ** (threshold_db / 20.0), noise * 3.0)
    speech_frames = int(np.sum(rms > thresh))
    return AudioStats(
        seconds=len(x) / float(sample_rate),
        peak_db=_db(float(np.max(np.abs(x)))),
        speech_seconds=speech_frames * FRAME_S,
        noise_db=_db(noise),
    )


def prepare(pcm: np.ndarray, max_gain_db: float = 30.0) -> np.ndarray:
    """Remove DC and normalise to about -1 dBFS (Whisper likes a healthy level)."""
    x = pcm.astype(np.float32)
    if len(x) == 0:
        return pcm.astype(np.int16)
    x -= float(np.mean(x))
    peak = float(np.max(np.abs(x)))
    if peak > 1.0:
        gain = min(0.89 * 32767.0 / peak, 10 ** (max_gain_db / 20.0))
        x *= gain
    # 30 ms fades so a clipped start or end doesn't click
    fade = min(len(x) // 4, 480)
    if fade > 1:
        ramp = np.linspace(0.0, 1.0, fade, dtype=np.float32)
        x[:fade] *= ramp
        x[-fade:] *= ramp[::-1]
    return np.clip(x, -32768, 32767).astype(np.int16)


def write_wav(path: Path, pcm: np.ndarray, sample_rate: int) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with wave.open(str(path), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(sample_rate)
        w.writeframes(pcm.astype("<i2").tobytes())


def read_wav(path: Path) -> tuple[np.ndarray, int]:
    with wave.open(str(path), "rb") as w:
        rate = w.getframerate()
        channels = w.getnchannels()
        width = w.getsampwidth()
        raw = w.readframes(w.getnframes())
    if width != 2:
        raise ValueError("only 16-bit WAV files are supported")
    pcm = np.frombuffer(raw, dtype="<i2")
    if channels > 1:
        pcm = pcm.reshape(-1, channels).mean(axis=1).astype(np.int16)
    return pcm, rate


def resample(pcm: np.ndarray, src: int, dst: int) -> np.ndarray:
    if src == dst or len(pcm) == 0:
        return pcm
    n = int(round(len(pcm) * dst / src))
    xs = np.linspace(0, len(pcm) - 1, n)
    return np.interp(xs, np.arange(len(pcm)), pcm.astype(np.float32)).astype(np.int16)


def prune(directory: Path, keep: int, pattern: str = "*.wav") -> None:
    if not directory.exists():
        return
    files = sorted(directory.glob(pattern), key=lambda p: p.stat().st_mtime, reverse=True)
    for old in files[max(keep, 0):]:
        try:
            old.unlink()
        except OSError:
            pass
