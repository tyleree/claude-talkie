"""Command line: `talkie run` and friends. See `talkie --help`."""

from __future__ import annotations

import argparse
import logging
import logging.handlers
import os
import queue
import random
import shutil
import signal
import sys
import time
from pathlib import Path

from . import __version__, audio, macos
from . import protocol as P
from .adpcm import Encoder
from .config import Config, load
from .link import LinkEvent, ReceiverLink, candidate_ports, probe
from .router import RouteResult, Router
from .transcribe import TranscriptionError, find_whisper_bin, make_transcriber

log = logging.getLogger("talkie")


def setup_logging(cfg: Config, verbose: bool) -> None:
    root = logging.getLogger()
    root.setLevel(logging.DEBUG)
    console = logging.StreamHandler()
    console.setLevel(logging.DEBUG if verbose else logging.INFO)
    console.setFormatter(logging.Formatter("%(asctime)s %(message)s", "%H:%M:%S"))
    root.addHandler(console)
    try:
        cfg.home.mkdir(parents=True, exist_ok=True)
        fh = logging.handlers.RotatingFileHandler(cfg.home / "talkie.log", maxBytes=1_000_000, backupCount=3)
        fh.setLevel(logging.DEBUG)
        fh.setFormatter(logging.Formatter("%(asctime)s %(levelname)s %(name)s: %(message)s"))
        root.addHandler(fh)
    except OSError:
        pass
    for w in cfg.warnings:
        log.warning("config: %s", w)


class DryRunRouter(Router):
    """Logs where a transcript would go instead of sending it."""

    def route(self, target: int, text: str) -> RouteResult:
        where = "a new background agent" if target == P.TARGET_NEW else f"the current agent ({self.cfg.current_mode})"
        log.info("[dry run] would send to %s: %s", where, text)
        return RouteResult(True, "Dry run: not sent", where, "dry-run")


# -- commands ---------------------------------------------------------------


def cmd_run(cfg: Config, args: argparse.Namespace) -> int:
    from .service import Service

    link = ReceiverLink(args.port or cfg.receiver.port)
    svc = Service(cfg, link)
    if args.dry_run:
        svc.router = DryRunRouter(cfg.route, svc.state, svc.claude)
    signal.signal(signal.SIGTERM, lambda *_: svc.stop())
    log.info("Claude Talkie %s (config: %s)", __version__, cfg.source)
    try:
        svc.run()
    except KeyboardInterrupt:
        pass
    return 0


def cmd_ports(cfg: Config, args: argparse.Namespace) -> int:
    ports = candidate_ports()
    if not ports:
        print("No Espressif USB serial ports found. Is the receiver plugged in?")
        return 1
    for p in ports:
        info = probe(p)
        print(f"{p}: {'receiver ' + info.get('raw', '') if info else 'not a receiver (maybe the stick)'}")
    return 0


def cmd_transcribe(cfg: Config, args: argparse.Namespace) -> int:
    try:
        t = make_transcriber(cfg.transcribe)
    except TranscriptionError as e:
        print(f"error: {e}", file=sys.stderr)
        return 1
    pcm, rate = audio.read_wav(Path(args.wav))
    stats = audio.analyze(pcm, rate, cfg.audio.speech_threshold_db)
    print(f"[{t.name}] {stats.describe()}")
    start = time.monotonic()
    print(t.transcribe(Path(args.wav)) or "(no words)")
    print(f"took {time.monotonic() - start:.1f}s")
    return 0


def cmd_send(cfg: Config, args: argparse.Namespace) -> int:
    from .state import State

    router = Router(cfg.route, State(cfg.state_path))
    if args.dry_run:
        router = DryRunRouter(cfg.route, State(cfg.state_path))
    target = P.TARGET_NEW if args.new else P.TARGET_CURRENT
    if not args.new and cfg.route.current_mode == "focused" and args.delay:
        print(f"Focus the window to send to... ({args.delay:.0f} s)")
        time.sleep(args.delay)
    res = router.route(target, " ".join(args.text))
    print(("ok: " if res.ok else "failed: ") + res.summary + (f" ({res.detail})" if res.detail else ""))
    return 0 if res.ok else 1


class _SimLink:
    """Stands in for the receiver: loops packets between a fake stick and the service."""

    def __init__(self, packets: list[bytes], mac: str) -> None:
        self.events: "queue.Queue[LinkEvent]" = queue.Queue()
        self.packets = packets
        self.mac = mac
        self.acks: list[tuple] = []
        self.statuses: list[tuple] = []
        self.resent = 0

    def send_packet(self, mac: str, data: bytes) -> bool:
        t = P.packet_type(data)
        if t == P.NACK:
            clip, seqs = P.parse_nack(data)
            for s in seqs:
                self.resent += 1
                self.events.put(LinkEvent("packet", mac=self.mac, data=self.packets[s]))
        elif t == P.CLIP_ACK:
            self.acks.append(P.parse_clip_ack(data))
        elif t == P.STATUS:
            clip, stage, flags, text = P.parse_status(data)
            self.statuses.append((stage, text))
            print(f"  stick screen <- [{stage}] {text!r}")
        return True

    def start(self) -> None:
        pass

    def stop(self) -> None:
        pass


def simulate_packets(pcm, rate: int, clip_id: int, target: int, spp: int = 456) -> tuple[list[bytes], bytes, bytes]:
    if rate != 16000:
        pcm = audio.resample(pcm, rate, 16000)
    enc = Encoder()
    packets = []
    for seq, i in enumerate(range(0, len(pcm), spp)):
        pred, idx, n, data = enc.encode_block(int(v) for v in pcm[i:i + spp])
        packets.append(P.build_audio(clip_id, seq, pred, idx, n, data))
    start = P.build_start(clip_id, target, 16000, spp, battery=88)
    end = P.build_end(clip_id, len(packets), len(pcm), target)
    return packets, start, end


def cmd_simulate(cfg: Config, args: argparse.Namespace) -> int:
    """Push a WAV through the whole pipeline as if the stick had sent it."""
    from .service import Service

    pcm, rate = audio.read_wav(Path(args.wav))
    target = P.TARGET_NEW if args.new else P.TARGET_CURRENT
    clip_id = random.randint(1, 30000)
    mac = "aabbccddeeff"
    packets, start, end = simulate_packets(pcm, rate, clip_id, target)
    link = _SimLink(packets, mac)
    svc = Service(cfg, link)  # type: ignore[arg-type]
    if args.dry_run:
        svc.router = DryRunRouter(cfg.route, svc.state, svc.claude)
    rng = random.Random(1)
    lost = 0
    print(f"simulating clip {clip_id}: {len(packets)} packets, {args.loss:.0%} loss, target={P.TARGET_NAMES[target]}")
    svc.handle_link_event(LinkEvent("packet", mac=mac, data=start))
    for p in packets:
        if rng.random() < args.loss:
            lost += 1
            continue
        svc.handle_link_event(LinkEvent("packet", mac=mac, data=p))
    svc.handle_link_event(LinkEvent("packet", mac=mac, data=end))
    deadline = time.monotonic() + 10
    while not link.acks and time.monotonic() < deadline:
        try:
            svc.handle_link_event(link.events.get(timeout=0.05))
        except queue.Empty:
            pass
        svc.assembler.tick()
    if not link.acks:
        print("no CLIP_ACK: assembly failed")
        return 1
    _c, status, received, total = link.acks[-1]
    print(f"  dropped {lost}, resent {link.resent}; ack status {status} ({received}/{total} packets)")
    job = svc.jobs.get_nowait()
    res = svc.process(job)
    return 0 if res.ok else 1


def cmd_doctor(cfg: Config, args: argparse.Namespace) -> int:
    ok = True

    def check(cond: bool, good: str, bad: str, fatal: bool = True) -> None:
        nonlocal ok
        print(("  ok   " if cond else ("  FAIL " if fatal else "  warn ")) + (good if cond else bad))
        if not cond and fatal:
            ok = False

    print(f"Claude Talkie {__version__}  config: {cfg.source}")
    for w in cfg.warnings:
        check(False, "", f"config: {w}", fatal=False)
    print("Speech to text")
    t = cfg.transcribe
    wb = find_whisper_bin(t.whisper_bin)
    check(bool(wb), f"whisper.cpp: {wb}", "whisper.cpp not found (brew install whisper-cpp)", fatal=t.backend == "whisper-cpp")
    check(os.path.exists(t.model), f"model: {t.model}", f"model missing: {t.model} (run ./setup.sh)",
          fatal=t.backend == "whisper-cpp")
    try:
        tr = make_transcriber(t)
        check(True, f"using {tr.name}", "")
    except TranscriptionError as e:
        check(False, "", str(e))
    print("Claude Code")
    from .agents import ClaudeCLI

    cli = ClaudeCLI(cfg.route.claude_bin)
    check(bool(cli.bin), f"claude: {cli.bin}", "claude not found on PATH (set route.claude_bin)")
    check(os.path.isdir(cfg.route.new_cwd), f"new agents start in {cfg.route.new_cwd}",
          f"route.new_cwd does not exist: {cfg.route.new_cwd}")
    print(f"  info button A mode: {cfg.route.current_mode}")
    if cfg.route.current_mode == "focused":
        front = macos.frontmost_app() if macos.IS_MAC else None
        check(front is not None, f"front app detection works ({front.name if front else ''})",
              "cannot detect the front app (lsappinfo)", fatal=macos.IS_MAC)
        print("  info pasting needs Accessibility permission for the app running Talkie;\n"
              "       test it with: talkie send --current hello")
    if cfg.route.current_mode == "tmux":
        check(bool(shutil.which("tmux")), "tmux installed", "tmux not found")
    print("Receiver")
    ports = candidate_ports()
    found = None
    for p in ports:
        info = probe(p)
        if info:
            found = (p, info)
            break
    if found:
        p, info = found
        check(True, f"{p}: mac {info.get('mac')} channel {info.get('ch')} long-range {info.get('lr')} "
                    f"firmware {info.get('fw')} sticks {info.get('sticks')}", "")
        if "err" in info:
            check(False, "", f"receiver radio error: {info['err']}")
    else:
        check(False, "", "receiver not found (plug it in; flash it with `pio run -e receiver -t upload`)",
              fatal=False)
    print("All good." if ok else "Fix the FAIL lines above.")
    return 0 if ok else 1


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(prog="talkie", description="Push-to-talk from an M5StickS3 to Claude Code.")
    ap.add_argument("--config", help="config file (default ~/.claude-talkie/config.toml)")
    ap.add_argument("-v", "--verbose", action="store_true", help="debug logging")
    ap.add_argument("--version", action="version", version=__version__)
    sub = ap.add_subparsers(dest="cmd")

    p = sub.add_parser("run", help="run the listener (default)")
    p.add_argument("--port", help="receiver serial port (default: auto-detect)")
    p.add_argument("--dry-run", action="store_true", help="transcribe but don't send anything to Claude")

    sub.add_parser("doctor", help="check the setup")
    sub.add_parser("ports", help="list serial ports and find the receiver")

    p = sub.add_parser("transcribe", help="transcribe a WAV file with the configured engine")
    p.add_argument("wav")

    p = sub.add_parser("send", help="route text as if you had said it")
    g = p.add_mutually_exclusive_group(required=True)
    g.add_argument("--current", action="store_true", help="button A")
    g.add_argument("--new", action="store_true", help="button B")
    p.add_argument("--dry-run", action="store_true")
    p.add_argument("--delay", type=float, default=3.0, help="seconds to focus a window first (focused mode)")
    p.add_argument("text", nargs="+")

    p = sub.add_parser("simulate", help="run a WAV through the whole pipeline without hardware")
    p.add_argument("wav")
    p.add_argument("--new", action="store_true", help="as if button B was used")
    p.add_argument("--loss", type=float, default=0.05, help="fraction of radio packets to drop")
    p.add_argument("--dry-run", action="store_true", help="don't send anything to Claude")

    args = ap.parse_args(argv)
    try:
        cfg = load(args.config)
    except FileNotFoundError as e:
        print(f"config not found: {e}", file=sys.stderr)
        return 2
    except Exception as e:  # bad TOML
        print(f"cannot read config: {e}", file=sys.stderr)
        return 2
    cmd = args.cmd or "run"
    if cmd == "run" and not hasattr(args, "port"):
        args.port, args.dry_run = None, False
    setup_logging(cfg, args.verbose)
    return {
        "run": cmd_run,
        "doctor": cmd_doctor,
        "ports": cmd_ports,
        "transcribe": cmd_transcribe,
        "send": cmd_send,
        "simulate": cmd_simulate,
    }[cmd](cfg, args)
