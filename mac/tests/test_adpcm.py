import numpy as np

from talkie import adpcm
from conftest import speechlike


def snr_db(ref: np.ndarray, got: np.ndarray) -> float:
    ref = ref.astype(np.float64)
    err = ref - got.astype(np.float64)
    return 10 * np.log10(np.sum(ref ** 2) / max(np.sum(err ** 2), 1e-9))


def encode_packets(pcm: np.ndarray, spp: int = 456):
    enc = adpcm.Encoder()
    return [enc.encode_block(int(v) for v in pcm[i:i + spp]) for i in range(0, len(pcm), spp)]


def test_round_trip_is_bit_exact_with_encoder_state():
    pcm = speechlike(1.0)
    enc = adpcm.Encoder()
    pred, idx, n, data = enc.encode_block(int(v) for v in pcm[:456])
    out = adpcm.decode(data, n, pred, idx)
    # The decoder must land exactly where the encoder's internal model did.
    assert int(out[-1]) == enc.predictor


def test_packets_decode_independently_and_sound_right():
    pcm = speechlike(2.0)
    packets = encode_packets(pcm)
    decoded = np.concatenate([adpcm.decode(d, n, p, i) for p, i, n, d in packets])
    assert len(decoded) == len(pcm)
    assert snr_db(pcm, decoded) > 18  # 4-bit ADPCM on voice is typically 20+ dB
    # Any single packet decodes alone from its header state.
    p, i, n, d = packets[10]
    alone = adpcm.decode(d, n, p, i)
    assert np.array_equal(alone, decoded[10 * 456:11 * 456])


def test_extremes_do_not_overflow():
    pcm = np.array([32767, -32768] * 500, dtype=np.int16)
    packets = encode_packets(pcm)
    decoded = np.concatenate([adpcm.decode(d, n, p, i) for p, i, n, d in packets])
    assert decoded.dtype == np.int16
    assert decoded.max() <= 32767 and decoded.min() >= -32768


def test_odd_sample_count():
    pcm = speechlike(0.05)[:101]
    (p, i, n, d), = encode_packets(pcm, spp=456)
    assert n == 101 and len(d) == 51
    assert len(adpcm.decode(d, n, p, i)) == 101
