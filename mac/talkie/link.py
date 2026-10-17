"""USB serial link to the receiver dongle (firmware/src/receiver/main.cpp).

The receiver speaks text lines. This module finds it among your serial ports,
keeps the connection alive (heartbeats, reconnect on unplug), and turns lines
into events on a queue for the service to consume.
"""

from __future__ import annotations

import base64
import binascii
import logging
import queue
import re
import threading
import time
from dataclasses import dataclass, field
from typing import Optional

import serial
from serial.tools import list_ports

log = logging.getLogger("talkie.link")

ESPRESSIF_VID = 0x303A
HEARTBEAT_S = 2.0
PROBE_S = 1.5
RECONNECT_S = 2.0


@dataclass
class LinkEvent:
    kind: str  # packet | info | event | log | tx_error | connected | disconnected
    mac: str = ""
    rssi: int = 0
    data: bytes = b""
    text: str = ""
    fields: dict = field(default_factory=dict)


def parse_kv(text: str) -> dict:
    return dict(re.findall(r"(\w+)=(\S+)", text))


def candidate_ports() -> list[str]:
    """Serial ports that could be an Espressif USB device, best guesses first."""
    ports = []
    for p in list_ports.comports():
        dev = p.device
        if dev.startswith("/dev/tty.") and dev.replace("/dev/tty.", "/dev/cu.") != dev:
            continue  # on macOS use the /dev/cu.* twin
        score = 0
        if p.vid == ESPRESSIF_VID:
            score += 2
        if "usbmodem" in dev or "ttyACM" in dev:
            score += 1
        if score:
            ports.append((score, dev))
    return [d for _, d in sorted(ports, key=lambda t: -t[0])]


def probe(port: str, timeout: float = PROBE_S) -> Optional[dict]:
    """Return the receiver's info fields if `port` is a Talkie receiver."""
    try:
        with serial.Serial(port, 115200, timeout=0.1) as ser:
            ser.reset_input_buffer()
            ser.write(b"I\n")
            ser.flush()
            buf = b""
            deadline = time.monotonic() + timeout
            while time.monotonic() < deadline:
                buf += ser.read(512)
                while b"\n" in buf:
                    line, buf = buf.split(b"\n", 1)
                    text = line.decode("utf-8", "replace").strip()
                    if text.startswith("I receiver"):
                        info = parse_kv(text)
                        info["raw"] = text
                        return info
                    if text.startswith("I stick"):
                        return None  # the stick itself is plugged in for charging
    except (OSError, serial.SerialException):
        return None
    return None


def find_receiver() -> tuple[Optional[str], Optional[dict]]:
    for port in candidate_ports():
        info = probe(port)
        if info:
            return port, info
    return None, None


class ReceiverLink:
    def __init__(self, port: str = "auto") -> None:
        self.port_setting = port
        self.events: "queue.Queue[LinkEvent]" = queue.Queue()
        self.port: Optional[str] = None
        self.info: dict = {}
        self._ser: Optional[serial.Serial] = None
        self._lock = threading.Lock()
        self._stop = threading.Event()
        self._thread: Optional[threading.Thread] = None
        self._warned_missing = False

    # -- public ---------------------------------------------------------

    @property
    def connected(self) -> bool:
        return self._ser is not None

    def start(self) -> None:
        self._thread = threading.Thread(target=self._run, name="talkie-link", daemon=True)
        self._thread.start()

    def stop(self) -> None:
        self._stop.set()
        if self._thread:
            self._thread.join(timeout=3)
        self._close()

    def send_packet(self, mac: str, data: bytes) -> bool:
        return self.send_line(f"S {mac} {base64.b64encode(data).decode()}")

    def send_line(self, line: str) -> bool:
        with self._lock:
            ser = self._ser
            if ser is None:
                return False
            try:
                ser.write((line + "\n").encode())
                return True
            except (OSError, serial.SerialException) as e:
                log.debug("write failed: %s", e)
                return False

    # -- internals ------------------------------------------------------

    def _close(self) -> None:
        with self._lock:
            ser, self._ser = self._ser, None
        if ser is not None:
            try:
                ser.close()
            except Exception:
                pass

    def _resolve_port(self) -> Optional[str]:
        if self.port_setting and self.port_setting != "auto":
            return self.port_setting
        port, info = find_receiver()
        if port:
            self.info = info or {}
        return port

    def _run(self) -> None:
        while not self._stop.is_set():
            port = self._resolve_port()
            if not port:
                if not self._warned_missing:
                    log.warning("receiver not found; plug it in (looking every %.0f s)", RECONNECT_S)
                    self._warned_missing = True
                self._stop.wait(RECONNECT_S)
                continue
            try:
                ser = serial.Serial(port, 115200, timeout=0.1)
            except (OSError, serial.SerialException) as e:
                log.warning("cannot open %s: %s", port, e)
                self._stop.wait(RECONNECT_S)
                continue
            with self._lock:
                self._ser = ser
            self.port = port
            self._warned_missing = False
            self.events.put(LinkEvent("connected", text=port, fields=dict(self.info)))
            self.send_line("I")
            try:
                self._read_loop(ser)
            except (OSError, serial.SerialException) as e:
                log.warning("receiver disconnected (%s)", e)
            finally:
                self._close()
                self.events.put(LinkEvent("disconnected", text=port))
            self._stop.wait(RECONNECT_S)

    def _read_loop(self, ser: serial.Serial) -> None:
        buf = b""
        last_hb = 0.0
        while not self._stop.is_set():
            now = time.monotonic()
            if now - last_hb >= HEARTBEAT_S:
                if not self.send_line("H"):
                    raise serial.SerialException("write failed")
                last_hb = now
            chunk = ser.read(ser.in_waiting or 1)
            if not chunk:
                continue
            buf += chunk
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                self._handle_line(line.decode("utf-8", "replace").strip())
            if len(buf) > 8192:  # garbage without newlines
                buf = b""

    def _handle_line(self, line: str) -> None:
        if not line:
            return
        kind, _, rest = line.partition(" ")
        if kind == "R":
            parts = rest.split(" ")
            if len(parts) != 3:
                return
            mac, rssi, b64 = parts
            try:
                data = base64.b64decode(b64, validate=True)
                rssi_i = int(rssi)
            except (ValueError, binascii.Error):
                log.debug("bad packet line: %r", line[:80])
                return
            self.events.put(LinkEvent("packet", mac=mac, rssi=rssi_i, data=data))
        elif kind == "I":
            self.info = parse_kv(rest)
            self.info["raw"] = line
            self.events.put(LinkEvent("info", text=rest, fields=dict(self.info)))
        elif kind == "E":
            self.events.put(LinkEvent("event", text=rest))
        elif kind == "X":
            mac, _, reason = rest.partition(" ")
            self.events.put(LinkEvent("tx_error", mac=mac, text=reason))
        elif kind == "L":
            self.events.put(LinkEvent("log", text=rest))
        else:
            log.debug("receiver: %s", line[:120])
