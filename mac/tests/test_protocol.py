import pytest

from talkie import protocol as P


def test_audio_round_trip():
    raw = P.build_audio(77, 5, -1234, 40, 456, bytes(range(228)))
    pkt = P.parse(raw)
    assert isinstance(pkt, P.Audio)
    assert (pkt.clip, pkt.seq, pkt.predictor, pkt.step_index, pkt.nsamples) == (77, 5, -1234, 40, 456)
    assert len(raw) == P.AUDIO_HDR_SIZE + 228 <= P.MAX_PAYLOAD


def test_start_end_cancel():
    s = P.parse(P.build_start(9, P.TARGET_NEW, 16000, 456, battery=255))
    assert isinstance(s, P.Start) and s.target == P.TARGET_NEW and s.battery is None
    e = P.parse(P.build_end(9, 120, 54321, P.TARGET_NEW, P.END_LATCHED))
    assert isinstance(e, P.End) and (e.total_packets, e.total_samples, e.reason) == (120, 54321, P.END_LATCHED)
    assert isinstance(P.parse(P.build_cancel(9)), P.Cancel)


def test_nack_limits_and_round_trip():
    seqs = list(range(P.NACK_MAX))
    raw = P.build_nack(3, seqs)
    assert len(raw) <= P.MAX_PAYLOAD
    assert P.parse_nack(raw) == (3, seqs)
    with pytest.raises(ValueError):
        P.build_nack(3, list(range(P.NACK_MAX + 1)))


def test_status_text_is_capped_without_splitting_utf8():
    raw = P.build_status(1, P.STAGE_DONE, "é" * 300, sound=True)
    assert len(raw) <= P.MAX_PAYLOAD
    clip, stage, flags, text = P.parse_status(raw)
    assert stage == P.STAGE_DONE and flags == P.STATUS_SOUND
    assert set(text) == {"é"}


def test_bad_packets_rejected():
    with pytest.raises(P.ProtocolError):
        P.parse(b"\x00\x11\x01\x00")
    with pytest.raises(P.ProtocolError):
        P.parse(P.build_audio(1, 0, 0, 0, 456, b"\x00" * 10))  # truncated payload
    with pytest.raises(P.ProtocolError):
        P.parse(P.build_audio(1, 0, 0, 99, 2, b"\x00"))  # step index out of range
