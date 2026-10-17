"""Thin wrapper around the `claude` CLI (Claude Code background agents)."""

from __future__ import annotations

import json
import logging
import os
import re
import shutil
import subprocess
from dataclasses import dataclass
from pathlib import Path
from typing import Optional

log = logging.getLogger("talkie.agents")

EXTRA_PATH = [
    str(Path.home() / ".local" / "bin"),
    str(Path.home() / ".claude" / "local"),
    "/opt/homebrew/bin",
    "/usr/local/bin",
]


class ClaudeError(RuntimeError):
    pass


@dataclass
class BgResult:
    short_id: str
    name: str
    output: str


def child_env() -> dict:
    env = dict(os.environ)
    parts = env.get("PATH", "").split(os.pathsep)
    for p in EXTRA_PATH:
        if p not in parts:
            parts.append(p)
    env["PATH"] = os.pathsep.join(x for x in parts if x)
    return env


def resolve_claude(configured: str = "claude") -> Optional[str]:
    if os.path.sep in configured:
        return configured if os.access(configured, os.X_OK) else None
    return shutil.which(configured, path=child_env()["PATH"])


def safe_prompt(text: str) -> str:
    # A leading "-" would be read as a command-line flag.
    return text.lstrip("- \t") or text


_BG_RE = re.compile(r"backgrounded\W+([0-9a-f]{6,})(?:\W+(\S.*))?", re.IGNORECASE)


def parse_bg_output(out: str) -> Optional[BgResult]:
    for line in out.splitlines():
        m = _BG_RE.search(line)
        if m:
            return BgResult(m.group(1), (m.group(2) or "").strip(), out)
    return None


def _explain(stderr: str, stdout: str, cwd: str) -> str:
    text = (stderr or stdout or "").strip()
    low = text.lower()
    if "not trusted" in low or "trust" in low and "workspace" in low:
        return f"Folder not trusted yet: run `claude` once in {cwd} and accept"
    if "not logged in" in low or "authentication" in low or "login" in low:
        return "Claude Code is not logged in (run `claude` and /login)"
    last = text.splitlines()[-1] if text else "no output"
    return last[:200]


class ClaudeCLI:
    def __init__(self, configured: str = "claude", timeout: float = 90.0) -> None:
        self.bin = resolve_claude(configured)
        self.timeout = timeout

    def require(self) -> str:
        if not self.bin:
            raise ClaudeError("`claude` not found. Install Claude Code or set route.claude_bin")
        return self.bin

    def _run(self, args: list[str], cwd: Optional[str] = None, timeout: Optional[float] = None) -> subprocess.CompletedProcess:
        bin_ = self.require()
        try:
            return subprocess.run([bin_, *args], cwd=cwd, capture_output=True, text=True,
                                  timeout=timeout or self.timeout, env=child_env(), stdin=subprocess.DEVNULL)
        except subprocess.TimeoutExpired:
            raise ClaudeError("claude took too long to respond")
        except OSError as e:
            raise ClaudeError(f"could not run claude: {e}")

    def start_background(self, prompt: str, cwd: str, name: str = "", permission_mode: str = "",
                         model: str = "", agent: str = "", extra_args: Optional[list[str]] = None) -> BgResult:
        if not os.path.isdir(cwd):
            raise ClaudeError(f"new-agent folder does not exist: {cwd}")
        args: list[str] = []
        if agent:
            args += ["--agent", agent]
        args.append("--bg")
        if name:
            args += ["--name", name]
        if permission_mode:
            args += ["--permission-mode", permission_mode]
        if model:
            args += ["--model", model]
        args += list(extra_args or [])
        args.append(safe_prompt(prompt))
        r = self._run(args, cwd=cwd)
        res = parse_bg_output(r.stdout + "\n" + r.stderr)
        if r.returncode != 0 or res is None:
            raise ClaudeError(_explain(r.stderr, r.stdout, cwd))
        return res

    def resume_background(self, session_id: str, prompt: str, cwd: Optional[str] = None) -> BgResult:
        r = self._run(["--resume", session_id, "--bg", safe_prompt(prompt)], cwd=cwd)
        res = parse_bg_output(r.stdout + "\n" + r.stderr)
        if r.returncode != 0 or res is None:
            raise ClaudeError(_explain(r.stderr, r.stdout, cwd or "."))
        return res

    def list_agents(self, include_done: bool = True, timeout: float = 8.0) -> list[dict]:
        args = ["agents", "--json"] + (["--all"] if include_done else [])
        r = self._run(args, timeout=timeout)
        if r.returncode != 0:
            raise ClaudeError(_explain(r.stderr, r.stdout, "."))
        try:
            data = json.loads(r.stdout or "[]")
        except json.JSONDecodeError:
            raise ClaudeError("could not parse `claude agents --json` output")
        return data if isinstance(data, list) else []

    def session_id_for(self, short_id: str) -> Optional[str]:
        for a in self.list_agents():
            if a.get("id") == short_id:
                return a.get("sessionId")
        return None

    def summary(self) -> str:
        """e.g. "2 working, 1 needs you" for background agents; "" when idle."""
        try:
            agents = self.list_agents(include_done=False, timeout=5.0)
        except ClaudeError as e:
            log.debug("agent summary unavailable: %s", e)
            return ""
        working = sum(1 for a in agents if a.get("kind") == "background" and a.get("state") == "working")
        blocked = sum(1 for a in agents if a.get("kind") == "background" and a.get("state") == "blocked")
        parts = []
        if working:
            parts.append(f"{working} working")
        if blocked:
            parts.append(f"{blocked} need{'s' if blocked == 1 else ''} you")
        return ", ".join(parts)
