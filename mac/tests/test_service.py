"""End to end on the Mac side: packets in, Claude call + stick status out."""

import queue

import numpy as np

from talkie import protocol as P
from talkie.cli import _SimLink, simulate_packets
from talkie.config import Config
from talkie.link import LinkEvent
from talkie.router import RouteResult
from talkie.service import Service, stick_text
from conftest import speechlike

MAC = "a1b2c3d4e5f6"


class FakeTranscriber:
    name = "fake"

    def __init__(self, text="please run the test suite"):
        self.text = text
        self.wavs = []

    def transcribe(self, wav):
        self.wavs.append(wav)
        assert wav.exists() and wav.stat().st_size > 44
        return self.text


class FakeRouter:
    def __init__(self, ok=True):
        self.calls = []
        self.ok = ok

    def route(self, target, text):
        self.calls.append((target, text))
        if self.ok:
            return RouteResult(True, "Sent to Ghostty", "", "focused")
        return RouteResult(False, "Copied to clipboard", "Slack is in front", "clipboard")


def make_service(tmp_path, pcm, target=P.TARGET_CURRENT, text="please run the test suite", ok=True, loss=0.0):
    cfg = Config(home=tmp_path)
    cfg.feedback.notifications = False
    cfg.audio.keep_clips = 2
    packets, start, end = simulate_packets(pcm, 16000, 321, target)
    link = _SimLink(packets, MAC)
    notes = []
    svc = Service(cfg, link, router=FakeRouter(ok), transcriber=FakeTranscriber(text),
                  notify=lambda *a: notes.append(a))
    rng = np.random.default_rng(0)
    svc.handle_link_event(LinkEvent("packet", mac=MAC, data=start))
    for p in packets:
        if rng.random() >= loss:
            svc.handle_link_event(LinkEvent("packet", mac=MAC, data=p))
    svc.handle_link_event(LinkEvent("packet", mac=MAC, data=end))
    for _ in range(200):
        if link.acks:
            break
        try:
            svc.handle_link_event(link.events.get(timeout=0.01))
        except queue.Empty:
            svc.assembler.tick()
    return svc, link


def test_happy_path(tmp_path):
    svc, link = make_service(tmp_path, speechlike(2.0))
    assert link.acks[-1][1] == P.ACK_COMPLETE
    res = svc.process(svc.jobs.get_nowait())
    assert res.ok
    stages = [s for s, _ in link.statuses]
    assert stages == [P.STAGE_TRANSCRIBING, P.STAGE_ROUTING, P.STAGE_DONE]
    assert link.statuses[-1][1] == "Sent to Ghostty\nplease run the test suite"
    assert svc.router.calls == [(P.TARGET_CURRENT, "please run the test suite")]
    assert (tmp_path / "history.jsonl").read_text().count("\n") == 1
    assert len(list((tmp_path / "clips").glob("*.wav"))) == 1


def test_lossy_link_still_delivers(tmp_path):
    svc, link = make_service(tmp_path, speechlike(4.0), loss=0.15, target=P.TARGET_NEW)
    assert link.resent > 0 and link.acks[-1][1] == P.ACK_COMPLETE
    assert svc.process(svc.jobs.get_nowait()).ok
    assert svc.router.calls[0][0] == P.TARGET_NEW


def test_silence_is_not_transcribed(tmp_path):
    quiet = (np.random.default_rng(1).normal(0, 20, 32000)).astype(np.int16)  # ~-64 dBFS hiss
    svc, link = make_service(tmp_path, quiet)
    res = svc.process(svc.jobs.get_nowait())
    assert not res.ok and res.summary == "Didn't hear you"
    assert svc._transcriber.wavs == [] and svc.router.calls == []
    assert link.statuses[-1][0] == P.STAGE_ERROR


def test_empty_transcript(tmp_path):
    svc, link = make_service(tmp_path, speechlike(1.0), text="")
    res = svc.process(svc.jobs.get_nowait())
    assert not res.ok and res.summary == "Didn't catch that"
    assert svc.router.calls == []


def test_routing_failure_reports_detail_and_text(tmp_path):
    svc, link = make_service(tmp_path, speechlike(1.0), ok=False)
    svc.process(svc.jobs.get_nowait())
    stage, text = link.statuses[-1]
    assert stage == P.STAGE_ERROR
    assert text.startswith("Copied to clipboard\nSlack is in front")
    assert "please run the test suite" in text


def test_stick_text_is_ascii_and_short():
    s = stick_text("Sent to Ghostty → café “quoted” " + "x" * 400)
    assert s.isascii() and len(s) <= P.STATUS_TEXT_MAX
    assert s.startswith('Sent to Ghostty -> cafe "quoted"')
