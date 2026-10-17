"""4-bit IMA ADPCM, bit-exact with firmware/include/talkie_adpcm.h."""

from __future__ import annotations

from typing import Iterable

import numpy as np

STEPS = (
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73,
    80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494,
    544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499,
    2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487,
    12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767,
)
INDEX_ADJUST = (-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8)

# For every (step index, nibble): the signed predictor delta and the next index.
# Turns the decoder's inner loop into two table lookups.
_DELTA = [[0] * 16 for _ in range(89)]
_NEXT = [[0] * 16 for _ in range(89)]
for _i, _step in enumerate(STEPS):
    for _nib in range(16):
        _dq = _step >> 3
        if _nib & 4:
            _dq += _step
        if _nib & 2:
            _dq += _step >> 1
        if _nib & 1:
            _dq += _step >> 2
        _DELTA[_i][_nib] = -_dq if _nib & 8 else _dq
        _NEXT[_i][_nib] = min(88, max(0, _i + INDEX_ADJUST[_nib]))


def decode(data: bytes, nsamples: int, predictor: int, index: int) -> np.ndarray:
    """Decode one packet's nibbles (low nibble first) into int16 samples."""
    out = [0] * nsamples
    p = predictor
    idx = index
    delta = _DELTA
    nxt = _NEXT
    for i in range(nsamples):
        b = data[i >> 1]
        nib = (b >> 4) if (i & 1) else (b & 0x0F)
        p += delta[idx][nib]
        if p > 32767:
            p = 32767
        elif p < -32768:
            p = -32768
        idx = nxt[idx][nib]
        out[i] = p
    return np.asarray(out, dtype=np.int16)


class Encoder:
    """Reference encoder (the stick's algorithm), used by tests and the simulator."""

    def __init__(self) -> None:
        self.predictor = 0
        self.index = 0

    def encode_sample(self, sample: int) -> int:
        step = STEPS[self.index]
        diff = int(sample) - self.predictor
        nib = 0
        if diff < 0:
            nib = 8
            diff = -diff
        dq = step >> 3
        if diff >= step:
            nib |= 4
            diff -= step
            dq += step
        step >>= 1
        if diff >= step:
            nib |= 2
            diff -= step
            dq += step
        step >>= 1
        if diff >= step:
            nib |= 1
            dq += step
        p = self.predictor + (-dq if nib & 8 else dq)
        self.predictor = max(-32768, min(32767, p))
        self.index = min(88, max(0, self.index + INDEX_ADJUST[nib]))
        return nib

    def encode_block(self, samples: Iterable[int]) -> tuple[int, int, int, bytes]:
        """Encode samples into one packet: (predictor, index, count, nibble bytes)."""
        pred, idx = self.predictor, self.index
        samples = list(samples)
        out = bytearray((len(samples) + 1) // 2)
        for i, s in enumerate(samples):
            nib = self.encode_sample(s)
            if i & 1:
                out[i >> 1] |= nib << 4
            else:
                out[i >> 1] = nib
        return pred, idx, len(samples), bytes(out)
