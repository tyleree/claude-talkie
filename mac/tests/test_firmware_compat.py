"""Cross-checks against the firmware's C headers (skipped if gcc is missing)."""

import shutil
import struct
import subprocess
from pathlib import Path

import numpy as np
import pytest

from talkie import adpcm, protocol as P
from conftest import speechlike

FW = Path(__file__).resolve().parents[2] / "firmware"
pytestmark = pytest.mark.skipif(not shutil.which("gcc") or not FW.exists(), reason="needs gcc and firmware/")


@pytest.fixture(scope="module")
def vectors(tmp_path_factory) -> Path:
    exe = tmp_path_factory.mktemp("bin") / "vectors"
    subprocess.run(["gcc", "-std=c11", "-O2", "-Wall", "-Werror", f"-I{FW / 'include'}",
                    str(FW / "tools" / "vectors.c"), "-o", str(exe)], check=True)
    return exe


def test_struct_sizes_match_c(vectors):
    out = subprocess.run([str(vectors), "sizes"], capture_output=True, text=True, check=True).stdout
    sizes = dict(line.split() for line in out.strip().splitlines())
    for name, size in P.C_SIZES.items():
        assert int(sizes[name]) == size, name


def test_c_encoder_is_bit_exact_with_python(vectors, tmp_path):
    pcm = speechlike(3.0, seed=4)
    raw = tmp_path / "in.raw"
    raw.write_bytes(pcm.astype("<i2").tobytes())
    out = tmp_path / "out.bin"
    subprocess.run([str(vectors), "adpcm", str(raw), str(out)], check=True)
    blob = out.read_bytes()

    enc = adpcm.Encoder()
    off = 0
    decoded = []
    for start in range(0, len(pcm), 456):
        pred, idx, n = struct.unpack_from("<hBH", blob, off)
        off += 5
        data = blob[off:off + (n + 1) // 2]
        off += (n + 1) // 2
        ref = enc.encode_block(int(v) for v in pcm[start:start + 456])
        assert (pred, idx, n, data) == ref  # C and Python encoders agree exactly
        decoded.append(adpcm.decode(data, n, pred, idx))
    assert off == len(blob)
    assert len(np.concatenate(decoded)) == len(pcm)
