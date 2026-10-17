import random

from talkie import protocol as P
from talkie.cli import simulate_packets
from talkie.clips import ClipAssembler
from conftest import speechlike

MAC = "a1b2c3d4e5f6"


class FakeClock:
    def __init__(self):
        self.t = 1000.0

    def __call__(self):
        return self.t


class Harness:
    """A pretend stick on a lossy radio, wired to an assembler."""

    def __init__(self, seconds=3.0, loss=0.0, resend_loss=0.0, target=P.TARGET_CURRENT, seed=0):
        self.pcm = speechlike(seconds, seed=seed)
        self.clip = 4242
        self.packets, self.start, self.end = simulate_packets(self.pcm, 16000, self.clip, target)
        self.rng = random.Random(seed)
        self.loss, self.resend_loss = loss, resend_loss
        self.clock = FakeClock()
        self.sent = []  # packets the Mac sent to the stick
        self.done = []
        self.asm = ClipAssembler(self.on_send, self.done.append, clock=self.clock)

    def on_send(self, mac, data):
        self.sent.append(data)
        if P.packet_type(data) == P.NACK:
            _clip, seqs = P.parse_nack(data)
            for s in seqs:
                if self.rng.random() >= self.resend_loss:
                    self.asm.handle(MAC, P.parse(self.packets[s]))
        return True

    def stream(self, send_start=True):
        if send_start:
            self.asm.handle(MAC, P.parse(self.start))
        for p in self.packets:
            if self.rng.random() >= self.loss:
                self.asm.handle(MAC, P.parse(p))
        self.asm.handle(MAC, P.parse(self.end))

    def run_until_done(self, max_s=30):
        while not self.done and max_s > 0:
            self.clock.t += 0.1
            max_s -= 0.1
            self.asm.tick()

    def acks(self):
        return [P.parse_clip_ack(d) for d in self.sent if P.packet_type(d) == P.CLIP_ACK]

    def nacks(self):
        return [d for d in self.sent if P.packet_type(d) == P.NACK]


def test_clean_clip_completes_immediately():
    h = Harness()
    h.stream()
    assert len(h.done) == 1
    c = h.done[0]
    assert c.status == P.ACK_COMPLETE and c.received == c.total == len(h.packets)
    assert len(c.pcm) == len(h.pcm)
    assert h.acks() == [(h.clip, P.ACK_COMPLETE, len(h.packets), len(h.packets))]
    assert not h.nacks()


def test_lost_packets_are_recovered_by_nack():
    h = Harness(seconds=5, loss=0.2, seed=3)
    h.stream()
    h.run_until_done()
    assert h.done and h.done[0].status == P.ACK_COMPLETE
    assert h.nacks()
    assert len(h.done[0].pcm) == len(h.pcm)


def test_persistent_loss_ends_partial_with_silence():
    h = Harness(seconds=2, loss=0.3, resend_loss=1.0, seed=5)
    h.stream()
    h.run_until_done()
    c = h.done[0]
    assert c.status == P.ACK_PARTIAL and c.received < c.total
    assert len(c.pcm) == len(h.pcm)  # gaps filled, timing preserved
    assert len(h.nacks()) == 4  # gave up after max_rounds


def test_missing_start_still_works():
    h = Harness()
    h.stream(send_start=False)
    assert h.done and h.done[0].target == P.TARGET_CURRENT


def test_unknown_clip_end_asks_for_full_resend():
    h = Harness()
    h.asm.handle(MAC, P.parse(h.end))
    assert h.acks() == [(h.clip, P.ACK_UNKNOWN, 0, len(h.packets))]
    assert not h.done


def test_duplicate_end_reacks_and_repeats_status():
    h = Harness()
    h.stream()
    h.asm.last_status[(MAC, h.clip)] = P.build_status(h.clip, P.STAGE_DONE, "ok\nhi")
    h.asm.handle(MAC, P.parse(h.end))
    assert len(h.acks()) == 2
    assert P.packet_type(h.sent[-1]) == P.STATUS
    assert len(h.done) == 1  # not processed twice


def test_cancel_discards_and_ignores_stragglers():
    h = Harness()
    h.asm.handle(MAC, P.parse(h.start))
    h.asm.handle(MAC, P.parse(h.packets[0]))
    h.asm.handle(MAC, P.parse(P.build_cancel(h.clip)))
    h.asm.handle(MAC, P.parse(h.packets[1]))
    h.asm.handle(MAC, P.parse(h.end))
    assert not h.done and h.asm.active == 0


def test_stale_clip_dropped():
    h = Harness()
    h.asm.handle(MAC, P.parse(h.start))
    h.asm.handle(MAC, P.parse(h.packets[0]))
    h.clock.t += 31
    h.asm.tick()
    assert h.asm.active == 0


def test_short_last_packet_length():
    h = Harness(seconds=1.01)  # not a multiple of 456 samples
    h.stream()
    assert len(h.done[0].pcm) == len(h.pcm)
    assert len(h.packets[-1]) < len(h.packets[0])
