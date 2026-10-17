"""Reassemble clips from radio packets, asking the stick to resend gaps.

Each AUDIO packet decodes on its own (it carries the ADPCM state), so packets
are decoded as they arrive. When END comes in we know the total; any missing
sequence numbers are NACKed (a few rounds), then the clip is finalized: gaps
are filled with silence, CLIP_ACK goes back to the stick, and the PCM is
handed to the transcription worker.
"""

from __future__ import annotations

import logging
import time
from dataclasses import dataclass, field
from typing import Callable, Optional

import numpy as np

from . import adpcm, protocol as P

log = logging.getLogger("talkie.clips")

# The stick keeps resend requests in a 255-entry ring; stay under it per round.
MAX_NACK_PER_ROUND = 240
FINALIZED_KEEP_S = 600.0
STALE_S = 30.0
CANCELLED_KEEP_S = 120.0


@dataclass
class CompletedClip:
    mac: str
    clip: int
    target: int
    sample_rate: int
    pcm: np.ndarray
    status: int  # protocol ACK_COMPLETE or ACK_PARTIAL
    received: int
    total: int
    battery: Optional[int] = None
    end_reason: int = 0

    @property
    def seconds(self) -> float:
        return len(self.pcm) / float(self.sample_rate or 16000)


@dataclass
class _Clip:
    mac: str
    clip: int
    created: float
    last_rx: float
    target: Optional[int] = None
    sample_rate: int = 16000
    spp: int = 456
    battery: Optional[int] = None
    started: bool = False
    packets: dict = field(default_factory=dict)  # seq -> np.int16 array
    total_packets: Optional[int] = None
    total_samples: Optional[int] = None
    end_reason: int = 0
    nack_rounds: int = 0
    nack_deadline: Optional[float] = None
    finalized: bool = False
    final_status: Optional[int] = None
    finalized_at: float = 0.0
    received: int = 0

    def missing(self) -> list[int]:
        if self.total_packets is None:
            return []
        return [s for s in range(self.total_packets) if s not in self.packets]


class ClipAssembler:
    def __init__(
        self,
        send: Callable[[str, bytes], bool],
        on_complete: Callable[[CompletedClip], None],
        clock: Callable[[], float] = time.monotonic,
        nack_wait: float = 0.5,
        max_rounds: int = 4,
    ) -> None:
        self._send = send
        self._on_complete = on_complete
        self._clock = clock
        self._nack_wait = nack_wait
        self._max_rounds = max_rounds
        self._clips: dict[tuple[str, int], _Clip] = {}
        self._cancelled: dict[tuple[str, int], float] = {}
        self.last_status: dict[tuple[str, int], bytes] = {}  # resent on duplicate END

    # -- packet handling -------------------------------------------------

    def handle(self, mac: str, pkt: P.Packet) -> None:
        key = (mac, pkt.clip)
        if key in self._cancelled and not isinstance(pkt, P.Start):
            return
        if isinstance(pkt, P.Start):
            self._on_start(key, pkt)
        elif isinstance(pkt, P.Audio):
            self._on_audio(key, pkt)
        elif isinstance(pkt, P.End):
            self._on_end(key, pkt)
        elif isinstance(pkt, P.Cancel):
            self._on_cancel(key)

    def _get(self, key: tuple[str, int]) -> _Clip:
        c = self._clips.get(key)
        if c is None:
            now = self._clock()
            c = _Clip(mac=key[0], clip=key[1], created=now, last_rx=now)
            self._clips[key] = c
        return c

    def _on_start(self, key, pkt: P.Start) -> None:
        self._cancelled.pop(key, None)
        old = self._clips.get(key)
        if old and old.finalized:
            if self._clock() - old.finalized_at < 120:
                return  # the stick is retrying a clip we already have; END will re-ack
            del self._clips[key]
        if pkt.codec != P.CODEC_IMA_ADPCM:
            log.warning("clip %d uses unknown codec %d", pkt.clip, pkt.codec)
        c = self._get(key)
        c.started = True
        c.target = pkt.target
        c.sample_rate = pkt.sample_rate or 16000
        c.spp = pkt.samples_per_packet or c.spp
        c.battery = pkt.battery
        c.last_rx = self._clock()
        log.info("clip %d from %s: recording for the %s agent", pkt.clip, key[0],
                 P.TARGET_NAMES.get(pkt.target, "?"))

    def _on_audio(self, key, pkt: P.Audio) -> None:
        c = self._get(key)
        if c.finalized:
            return
        c.last_rx = self._clock()
        if pkt.seq in c.packets:
            return
        if c.total_packets is not None and pkt.seq >= c.total_packets:
            return
        c.packets[pkt.seq] = adpcm.decode(pkt.data, pkt.nsamples, pkt.predictor, pkt.step_index)
        if pkt.nsamples and pkt.seq == 0 and not c.started:
            c.spp = max(c.spp, pkt.nsamples)

    def _on_end(self, key, pkt: P.End) -> None:
        c = self._clips.get(key)
        if c is not None and c.finalized:
            # Our CLIP_ACK was lost; repeat it and the latest status.
            self._send_ack(c)
            st = self.last_status.get(key)
            if st:
                self._send(c.mac, st)
            return
        if c is None or not c.packets:
            # We never saw this clip (listener restarted mid-clip?). Ask the
            # stick to send everything again.
            log.info("clip %d from %s unknown; asking for a full resend", pkt.clip, key[0])
            self._send(key[0], P.build_clip_ack(pkt.clip, P.ACK_UNKNOWN, 0, pkt.total_packets))
            return
        c.last_rx = self._clock()
        if c.total_packets is not None:
            return  # duplicate END while repairing; tick() drives the repair
        c.total_packets = pkt.total_packets
        c.total_samples = pkt.total_samples
        c.target = pkt.target
        c.end_reason = pkt.reason
        self._check(c)

    def _on_cancel(self, key) -> None:
        self._clips.pop(key, None)
        self._cancelled[key] = self._clock()
        log.info("clip %d cancelled on the stick", key[1])

    # -- repair & finalize ----------------------------------------------

    def _check(self, c: _Clip) -> None:
        missing = c.missing()
        if not missing:
            self._finalize(c, P.ACK_COMPLETE)
            return
        if c.nack_rounds >= self._max_rounds:
            log.warning("clip %d: %d of %d packets never arrived; filling with silence",
                        c.clip, len(missing), c.total_packets)
            self._finalize(c, P.ACK_PARTIAL)
            return
        batch = missing[:MAX_NACK_PER_ROUND]
        for i in range(0, len(batch), P.NACK_MAX):
            self._send(c.mac, P.build_nack(c.clip, batch[i:i + P.NACK_MAX]))
        c.nack_rounds += 1
        # resends take a few ms each over the long-range link
        c.nack_deadline = self._clock() + self._nack_wait + 0.012 * len(batch)
        log.info("clip %d: asked for %d missing packets (round %d)", c.clip, len(batch), c.nack_rounds)

    def _expected_len(self, c: _Clip, seq: int) -> int:
        assert c.total_packets is not None
        if seq < c.total_packets - 1 or c.total_samples is None:
            return c.spp
        return max(0, c.total_samples - c.spp * (c.total_packets - 1))

    def _finalize(self, c: _Clip, status: int) -> None:
        assert c.total_packets is not None
        parts = []
        for seq in range(c.total_packets):
            got = c.packets.get(seq)
            parts.append(got if got is not None else np.zeros(self._expected_len(c, seq), dtype=np.int16))
        pcm = np.concatenate(parts) if parts else np.zeros(0, dtype=np.int16)
        c.finalized = True
        c.final_status = status
        c.finalized_at = self._clock()
        received = len(c.packets)
        c.packets = {}  # free memory; we keep the record to re-ack duplicates
        c.received = received
        self._send_ack(c)
        self._on_complete(CompletedClip(
            mac=c.mac, clip=c.clip, target=c.target or P.TARGET_CURRENT, sample_rate=c.sample_rate,
            pcm=pcm, status=status, received=received, total=c.total_packets, battery=c.battery,
            end_reason=c.end_reason,
        ))

    def _send_ack(self, c: _Clip) -> None:
        self._send(c.mac, P.build_clip_ack(c.clip, c.final_status or 0, c.received, c.total_packets or 0))

    def tick(self) -> None:
        now = self._clock()
        for key, c in list(self._clips.items()):
            if c.finalized:
                if now - c.finalized_at > FINALIZED_KEEP_S:
                    del self._clips[key]
                    self.last_status.pop(key, None)
                continue
            if c.total_packets is not None:
                if c.nack_deadline is not None and now >= c.nack_deadline:
                    c.nack_deadline = None
                    self._check(c)
            elif now - c.last_rx > STALE_S:
                log.warning("clip %d from %s: no END after %.0f s; dropping", c.clip, c.mac, STALE_S)
                del self._clips[key]
        for key, t in list(self._cancelled.items()):
            if now - t > CANCELLED_KEEP_S:
                del self._cancelled[key]

    @property
    def active(self) -> int:
        return sum(1 for c in self._clips.values() if not c.finalized)
