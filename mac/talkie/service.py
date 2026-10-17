"""The listener: receiver link -> clip assembly -> Whisper -> Claude, with
progress reported back to the stick's screen."""

from __future__ import annotations

import logging
import queue
import tempfile
import threading
import time
import unicodedata
from pathlib import Path
from typing import Callable, Optional

from . import audio, macos
from . import protocol as P
from .agents import ClaudeCLI
from .clips import ClipAssembler, CompletedClip
from .config import Config
from .link import LinkEvent, ReceiverLink
from .router import RouteResult, Router
from .state import State, append_history
from .transcribe import Transcriber, TranscriptionError, make_transcriber

log = logging.getLogger("talkie")

_ASCII = str.maketrans({"→": "->", "—": "-", "–": "-", "‘": "'", "’": "'",
                        "“": '"', "”": '"', "…": "...", " ": " "})


def stick_text(s: str) -> str:
    """The stick's fonts are ASCII-only; transliterate and keep it short."""
    s = unicodedata.normalize("NFKD", s.translate(_ASCII)).encode("ascii", "ignore").decode()
    return s[: P.STATUS_TEXT_MAX]


class Service:
    def __init__(
        self,
        cfg: Config,
        link: ReceiverLink,
        router: Optional[Router] = None,
        transcriber: Optional[Transcriber] = None,
        notify: Callable[[str, str, str], None] = macos.notify,
    ) -> None:
        self.cfg = cfg
        self.link = link
        self.state = State(cfg.state_path)
        self.claude = ClaudeCLI(cfg.route.claude_bin, cfg.route.timeout)
        self.router = router or Router(cfg.route, self.state, self.claude)
        self._transcriber = transcriber
        self._transcriber_error = ""
        self._notify = notify
        self.jobs: "queue.Queue[CompletedClip]" = queue.Queue()
        self.assembler = ClipAssembler(self._send, self.jobs.put)
        self._stop = threading.Event()
        self.results: list[dict] = []  # for tests / the simulator

    # -- plumbing ----------------------------------------------------------

    def _send(self, mac: str, data: bytes) -> bool:
        return self.link.send_packet(mac, data)

    def _status(self, clip: CompletedClip, stage: int, text: str, sound: bool = False) -> None:
        data = P.build_status(clip.clip, stage, stick_text(text), sound)
        self.assembler.last_status[(clip.mac, clip.clip)] = data
        self._send(clip.mac, data)

    def transcriber(self) -> Transcriber:
        if self._transcriber is None:
            try:
                self._transcriber = make_transcriber(self.cfg.transcribe)
                self._transcriber_error = ""
                log.info("speech-to-text: %s", self._transcriber.name)
            except TranscriptionError as e:
                self._transcriber_error = str(e)
                raise
        return self._transcriber

    # -- main loop -----------------------------------------------------------

    def run(self) -> None:
        try:
            self.transcriber()
        except TranscriptionError as e:
            log.error("%s", e)
        if not self.claude.bin:
            log.error("`claude` was not found on PATH; set route.claude_bin in the config")
        worker = threading.Thread(target=self._worker, name="talkie-worker", daemon=True)
        worker.start()
        self.link.start()
        log.info("listening (Ctrl-C to quit)")
        try:
            while not self._stop.is_set():
                try:
                    ev = self.link.events.get(timeout=0.1)
                    self.handle_link_event(ev)
                except queue.Empty:
                    pass
                self.assembler.tick()
        finally:
            self.link.stop()

    def stop(self) -> None:
        self._stop.set()

    def handle_link_event(self, ev: LinkEvent) -> None:
        if ev.kind == "packet":
            try:
                pkt = P.parse(ev.data)
            except P.ProtocolError as e:
                log.debug("ignoring packet from %s: %s", ev.mac, e)
                return
            if isinstance(pkt, P.Ping):
                batt = f"{pkt.battery}%" if pkt.battery is not None else "?"
                log.debug("stick %s alive, battery %s, rssi %d", ev.mac, batt, ev.rssi)
                return
            self.assembler.handle(ev.mac, pkt)
        elif ev.kind == "connected":
            info = ev.fields
            log.info("receiver on %s (mac %s, channel %s, long range %s, firmware %s, sticks paired: %s)",
                     ev.text, info.get("mac", "?"), info.get("ch", "?"),
                     "on" if info.get("lr") == "1" else "off", info.get("fw", "?"), info.get("sticks", "?"))
        elif ev.kind == "info":
            if "err" in ev.fields:
                log.error("receiver radio problem: %s (try TK_LONG_RANGE 0 on both devices)", ev.fields["err"])
        elif ev.kind == "disconnected":
            log.warning("receiver unplugged (%s)", ev.text)
        elif ev.kind == "event":
            log.info("receiver: %s", ev.text)
        elif ev.kind == "log":
            log.info("receiver says: %s", ev.text)
        elif ev.kind == "tx_error":
            log.debug("receiver could not reach stick %s (%s)", ev.mac, ev.text)

    # -- per-clip work -----------------------------------------------------

    def _worker(self) -> None:
        while not self._stop.is_set():
            try:
                clip = self.jobs.get(timeout=0.5)
            except queue.Empty:
                continue
            try:
                self.process(clip)
            except Exception:  # never let one clip kill the worker
                log.exception("clip %d failed", clip.clip)
                self._status(clip, P.STAGE_ERROR, "Talkie error\nSee the Mac app's log.", sound=True)

    def _wav_path(self, clip: CompletedClip) -> Path:
        keep = self.cfg.audio.keep_clips
        name = time.strftime("%Y%m%d-%H%M%S") + f"-{P.TARGET_NAMES.get(clip.target, 'x')}-{clip.clip}.wav"
        if keep > 0:
            return self.cfg.clips_dir / name
        return Path(tempfile.gettempdir()) / f"talkie-{name}"

    def process(self, clip: CompletedClip) -> RouteResult:
        t0 = time.monotonic()
        target = P.TARGET_NAMES.get(clip.target, "?")
        self._status(clip, P.STAGE_TRANSCRIBING, "Transcribing")
        stats = audio.analyze(clip.pcm, clip.sample_rate, self.cfg.audio.speech_threshold_db)
        lost = clip.total - clip.received
        log.info("clip %d (%s agent): %s%s", clip.clip, target, stats.describe(),
                 f", {lost} packets lost" if lost else "")

        def fail(summary: str, detail: str) -> RouteResult:
            res = RouteResult(False, summary, detail, "")
            self._finish(clip, res, "", t0, stats.seconds)
            return res

        if stats.seconds < self.cfg.audio.min_seconds:
            return fail("Too short", "Hold the button while you talk.")
        if self.cfg.audio.speech_gate and stats.speech_seconds < 0.15:
            return fail("Didn't hear you", f"No speech detected ({stats.describe()}). Talk closer to the stick.")

        wav = self._wav_path(clip)
        audio.write_wav(wav, audio.prepare(clip.pcm), clip.sample_rate)
        try:
            try:
                text = self.transcriber().transcribe(wav)
            except TranscriptionError as e:
                return fail("Speech-to-text failed", str(e))
        finally:
            if self.cfg.audio.keep_clips > 0:
                audio.prune(self.cfg.clips_dir, self.cfg.audio.keep_clips)
            else:
                wav.unlink(missing_ok=True)
        if not text:
            return fail("Didn't catch that", "Whisper heard no words. Try again a little closer.")
        log.info("clip %d heard: %s", clip.clip, text)

        self._status(clip, P.STAGE_ROUTING, "Sending to the new agent" if clip.target == P.TARGET_NEW
                     else "Sending to Claude")
        res = self.router.route(clip.target, text)
        self._finish(clip, res, text, t0, stats.seconds)
        return res

    def _finish(self, clip: CompletedClip, res: RouteResult, text: str, t0: float, seconds: float) -> None:
        if res.ok:
            body = f"{res.summary}\n{text}"
        else:
            body = f"{res.summary}\n{res.detail}" + (f' "{text}"' if text else "")
        self._status(clip, P.STAGE_DONE if res.ok else P.STAGE_ERROR, body, sound=True)
        elapsed = time.monotonic() - t0
        (log.info if res.ok else log.warning)("clip %d -> %s: %s%s (%.1fs)", clip.clip, res.mode or "-",
                                                res.summary, f" ({res.detail})" if res.detail and not res.ok else "",
                                                elapsed)
        entry = {"time": time.strftime("%Y-%m-%dT%H:%M:%S"), "target": P.TARGET_NAMES.get(clip.target, "?"),
                 "seconds": round(seconds, 2), "text": text, "ok": res.ok, "mode": res.mode,
                 "summary": res.summary, "detail": res.detail, "lost_packets": clip.total - clip.received}
        self.results.append(entry)
        append_history(self.cfg.history_path, entry)
        if self.cfg.feedback.notifications:
            extra = ""
            if res.ok and self.cfg.feedback.agent_summary:
                extra = self.claude.summary()
            self._notify("Claude Talkie", text or res.detail, res.summary + (f" - {extra}" if extra else ""))
