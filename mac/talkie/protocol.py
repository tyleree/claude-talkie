"""Wire protocol shared with the firmware (firmware/include/talkie_protocol.h).

Packets are little-endian and at most 250 bytes. Keep the struct layouts here
in lockstep with the C header; tests/test_protocol.py checks them against the
C compiler's sizeof() when gcc is available.
"""

from __future__ import annotations

import struct
from dataclasses import dataclass
from typing import Optional, Union

MAGIC = 0xC7
PROTO_VERSION = 1
MAX_PAYLOAD = 250

HELLO = 0x01
HELLO_ACK = 0x02
PING = 0x03
PONG = 0x04
START = 0x10
AUDIO = 0x11
END = 0x12
CANCEL = 0x13
NACK = 0x20
CLIP_ACK = 0x21
STATUS = 0x22

TARGET_CURRENT = 1
TARGET_NEW = 2
TARGET_NAMES = {TARGET_CURRENT: "current", TARGET_NEW: "new"}

CODEC_IMA_ADPCM = 1

END_RELEASED = 0
END_MAX_LENGTH = 1
END_LATCHED = 2

ACK_COMPLETE = 0
ACK_PARTIAL = 1
ACK_UNKNOWN = 2
ACK_CANCELLED = 3

STAGE_RECEIVED = 1
STAGE_TRANSCRIBING = 2
STAGE_ROUTING = 3
STAGE_DONE = 4
STAGE_ERROR = 5
STAGE_INFO = 6

STATUS_SOUND = 0x01

_HDR = struct.Struct("<BBH")  # magic, type, clip
_PING = struct.Struct("<BB")  # battery, flags
_START = struct.Struct("<BBHHBB")  # target, codec, sample_rate, spp, battery, flags
_AUDIO = struct.Struct("<HhBBH")  # seq, predictor, step_index, reserved, nsamples
_END = struct.Struct("<HIBB")  # total_packets, total_samples, target, reason
_NACK = struct.Struct("<BB")  # count, reserved
_CLIP_ACK = struct.Struct("<BBHH")  # status, reserved, received, total
_STATUS = struct.Struct("<BB")  # stage, flags

HDR_SIZE = _HDR.size
AUDIO_HDR_SIZE = HDR_SIZE + _AUDIO.size
NACK_MAX = (MAX_PAYLOAD - HDR_SIZE - _NACK.size) // 2
STATUS_TEXT_MAX = MAX_PAYLOAD - HDR_SIZE - _STATUS.size

# Sizes the C header asserts; used by the tests.
C_SIZES = {
    "tk_hdr_t": 4,
    "tk_hello_t": 26,
    "tk_hello_ack_t": 32,
    "tk_ping_t": 6,
    "tk_pong_t": 6,
    "tk_start_t": HDR_SIZE + _START.size,
    "tk_audio_hdr_t": AUDIO_HDR_SIZE,
    "tk_end_t": HDR_SIZE + _END.size,
    "tk_nack_hdr_t": HDR_SIZE + _NACK.size,
    "tk_clip_ack_t": HDR_SIZE + _CLIP_ACK.size,
    "tk_status_hdr_t": HDR_SIZE + _STATUS.size,
}


class ProtocolError(ValueError):
    pass


@dataclass
class Ping:
    clip: int
    battery: Optional[int]
    charging: bool
    usb: bool


@dataclass
class Start:
    clip: int
    target: int
    codec: int
    sample_rate: int
    samples_per_packet: int
    battery: Optional[int]
    flags: int


@dataclass
class Audio:
    clip: int
    seq: int
    predictor: int
    step_index: int
    nsamples: int
    data: bytes


@dataclass
class End:
    clip: int
    total_packets: int
    total_samples: int
    target: int
    reason: int


@dataclass
class Cancel:
    clip: int


@dataclass
class Other:
    clip: int
    type: int
    raw: bytes


Packet = Union[Ping, Start, Audio, End, Cancel, Other]


def _battery(v: int) -> Optional[int]:
    return None if v == 255 else v


def parse(data: bytes) -> Packet:
    """Parse a packet the stick sent (as forwarded by the receiver)."""
    if len(data) < HDR_SIZE:
        raise ProtocolError("short packet")
    magic, ptype, clip = _HDR.unpack_from(data)
    if magic != MAGIC:
        raise ProtocolError(f"bad magic 0x{magic:02x}")
    body = data[HDR_SIZE:]
    if ptype == AUDIO:
        if len(body) < _AUDIO.size:
            raise ProtocolError("short audio packet")
        seq, pred, idx, _res, n = _AUDIO.unpack_from(body)
        payload = body[_AUDIO.size:]
        if len(payload) < (n + 1) // 2:
            raise ProtocolError("truncated audio payload")
        if idx > 88:
            raise ProtocolError("bad ADPCM step index")
        return Audio(clip, seq, pred, idx, n, bytes(payload[: (n + 1) // 2]))
    if ptype == START:
        if len(body) < _START.size:
            raise ProtocolError("short start packet")
        target, codec, rate, spp, batt, flags = _START.unpack_from(body)
        return Start(clip, target, codec, rate, spp, _battery(batt), flags)
    if ptype == END:
        if len(body) < _END.size:
            raise ProtocolError("short end packet")
        total, samples, target, reason = _END.unpack_from(body)
        return End(clip, total, samples, target, reason)
    if ptype == CANCEL:
        return Cancel(clip)
    if ptype == PING:
        if len(body) < _PING.size:
            raise ProtocolError("short ping")
        batt, flags = _PING.unpack_from(body)
        return Ping(clip, _battery(batt), bool(flags & 1), bool(flags & 2))
    return Other(clip, ptype, bytes(data))


def hdr(ptype: int, clip: int) -> bytes:
    return _HDR.pack(MAGIC, ptype, clip & 0xFFFF)


def build_nack(clip: int, seqs: list[int]) -> bytes:
    if len(seqs) > NACK_MAX:
        raise ValueError("too many sequence numbers for one NACK")
    return hdr(NACK, clip) + _NACK.pack(len(seqs), 0) + b"".join(struct.pack("<H", s) for s in seqs)


def build_clip_ack(clip: int, status: int, received: int, total: int) -> bytes:
    return hdr(CLIP_ACK, clip) + _CLIP_ACK.pack(status, 0, min(received, 0xFFFF), min(total, 0xFFFF))


def build_status(clip: int, stage: int, text: str, sound: bool = False) -> bytes:
    raw = text.encode("utf-8")[:STATUS_TEXT_MAX]
    # don't cut a multi-byte character in half
    while raw:
        try:
            raw.decode("utf-8")
            break
        except UnicodeDecodeError:
            raw = raw[:-1]
    return hdr(STATUS, clip) + _STATUS.pack(stage, STATUS_SOUND if sound else 0) + raw


# --- stick-side builders: used by the simulator and the tests -------------


def build_start(clip: int, target: int, sample_rate: int, spp: int, battery: int = 255) -> bytes:
    return hdr(START, clip) + _START.pack(target, CODEC_IMA_ADPCM, sample_rate, spp, battery, 0)


def build_audio(clip: int, seq: int, predictor: int, step_index: int, nsamples: int, data: bytes) -> bytes:
    return hdr(AUDIO, clip) + _AUDIO.pack(seq, predictor, step_index, 0, nsamples) + data


def build_end(clip: int, total_packets: int, total_samples: int, target: int, reason: int = 0) -> bytes:
    return hdr(END, clip) + _END.pack(total_packets, total_samples, target, reason)


def build_cancel(clip: int) -> bytes:
    return hdr(CANCEL, clip)


def parse_nack(data: bytes) -> tuple[int, list[int]]:
    _m, _t, clip = _HDR.unpack_from(data)
    count, _r = _NACK.unpack_from(data, HDR_SIZE)
    off = HDR_SIZE + _NACK.size
    return clip, [struct.unpack_from("<H", data, off + 2 * i)[0] for i in range(count)]


def parse_clip_ack(data: bytes) -> tuple[int, int, int, int]:
    _m, _t, clip = _HDR.unpack_from(data)
    status, _r, received, total = _CLIP_ACK.unpack_from(data, HDR_SIZE)
    return clip, status, received, total


def parse_status(data: bytes) -> tuple[int, int, int, str]:
    _m, _t, clip = _HDR.unpack_from(data)
    stage, flags = _STATUS.unpack_from(data, HDR_SIZE)
    return clip, stage, flags, data[HDR_SIZE + _STATUS.size:].decode("utf-8", "replace")


def packet_type(data: bytes) -> int:
    return data[1] if len(data) >= 2 else -1
