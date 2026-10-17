"""The serial link against a fake receiver on a pseudo-terminal."""

import base64
import os
import sys
import threading
import time

import pytest

from talkie import protocol as P
from talkie.link import ReceiverLink, parse_kv

pytestmark = pytest.mark.skipif(sys.platform == "win32", reason="needs a pty")


class FakeReceiver:
    def __init__(self):
        self.master, slave = os.openpty()
        self.path = os.ttyname(slave)
        self.slave = slave  # keep open so the pty stays alive
        self.lines = []
        self._stop = False
        self.t = threading.Thread(target=self._read, daemon=True)
        self.t.start()

    def write(self, line: str):
        os.write(self.master, (line + "\n").encode())

    def _read(self):
        buf = b""
        while not self._stop:
            try:
                chunk = os.read(self.master, 1024)
            except OSError:
                return
            buf += chunk
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                self.lines.append(line.decode().strip())

    def close(self):
        self._stop = True
        os.close(self.master)
        os.close(self.slave)


def wait_for(cond, timeout=3.0):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        if cond():
            return True
        time.sleep(0.02)
    return False


def test_parse_kv():
    assert parse_kv("receiver mac=aabb ch=1 lr=1 err=channel:5") == {
        "mac": "aabb", "ch": "1", "lr": "1", "err": "channel:5"}


def test_link_round_trip():
    rx = FakeReceiver()
    link = ReceiverLink(rx.path)
    link.start()
    try:
        ev = link.events.get(timeout=3)
        assert ev.kind == "connected"
        assert wait_for(lambda: "I" in rx.lines)  # asks for info on connect

        rx.write("I receiver mac=0011223344aa ch=1 lr=1 fw=1.0 proto=1 sticks=1")
        ev = link.events.get(timeout=3)
        assert ev.kind == "info" and ev.fields["lr"] == "1"

        pkt = P.build_start(5, P.TARGET_NEW, 16000, 456)
        rx.write(f"R a1b2c3d4e5f6 -61 {base64.b64encode(pkt).decode()}")
        ev = link.events.get(timeout=3)
        assert (ev.kind, ev.mac, ev.rssi, ev.data) == ("packet", "a1b2c3d4e5f6", -61, pkt)

        rx.write("R garbage")  # ignored, no crash
        rx.write("E paired a1b2c3d4e5f6")
        ev = link.events.get(timeout=3)
        assert ev.kind == "event" and "paired" in ev.text

        ack = P.build_clip_ack(5, P.ACK_COMPLETE, 10, 10)
        assert link.send_packet("a1b2c3d4e5f6", ack)
        assert wait_for(lambda: any(l.startswith("S a1b2c3d4e5f6 ") for l in rx.lines))
        line = next(l for l in rx.lines if l.startswith("S "))
        assert base64.b64decode(line.split()[2]) == ack
        assert wait_for(lambda: "H" in rx.lines, timeout=4)  # heartbeat
    finally:
        link.stop()
        rx.close()
