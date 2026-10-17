"""Deliver a transcript to Claude.

Button A (current agent), per route.current_mode:
  focused  paste into the frontmost terminal / editor / Claude app and press Return
  tmux     type into the tmux pane running claude
  last     continue the most recent agent Talkie started (claude --resume <id> --bg)
Button B (new agent): start a background agent with `claude --bg` in route.new_cwd.
"""

from __future__ import annotations

import logging
import re
import shutil
import subprocess
import time
from dataclasses import dataclass
from typing import Callable, Optional

from . import macos
from . import protocol as P
from .agents import ClaudeCLI, ClaudeError
from .config import RouteConfig
from .state import State

log = logging.getLogger("talkie.router")


@dataclass
class RouteResult:
    ok: bool
    summary: str  # one short line for the stick's screen
    detail: str = ""  # longer explanation (logs / notification)
    mode: str = ""


class Router:
    def __init__(
        self,
        cfg: RouteConfig,
        state: State,
        claude: Optional[ClaudeCLI] = None,
        frontmost: Callable[[], Optional[macos.FrontApp]] = macos.frontmost_app,
        paste: Callable[..., None] = macos.paste,
        set_clipboard: Callable[[str], bool] = macos.set_clipboard,
        run: Callable[..., subprocess.CompletedProcess] = subprocess.run,
    ) -> None:
        self.cfg = cfg
        self.state = state
        self.claude = claude or ClaudeCLI(cfg.claude_bin, cfg.timeout)
        self._frontmost = frontmost
        self._paste = paste
        self._set_clipboard = set_clipboard
        self._run = run

    def route(self, target: int, text: str) -> RouteResult:
        prompt = (self.cfg.prompt_prefix + text).strip()
        try:
            if target == P.TARGET_NEW:
                return self._new(prompt)
            mode = self.cfg.current_mode
            if mode == "tmux":
                return self._tmux(prompt)
            if mode == "last":
                return self._last(prompt)
            return self._focused(prompt)
        except ClaudeError as e:
            return RouteResult(False, "Claude error", str(e), "claude")

    # -- button B ---------------------------------------------------------

    def _agent_name(self, prompt: str) -> str:
        tpl = self.cfg.new_name
        if not tpl:
            return ""
        words = re.sub(r"[^a-z0-9 ]", "", prompt.lower()).split()[:4]
        return tpl.replace("{time}", time.strftime("%H%M")).replace("{words}", "-".join(words) or "voice")

    def _new(self, prompt: str, note: str = "") -> RouteResult:
        cwd = self.cfg.new_cwd
        res = self.claude.start_background(
            prompt, cwd, name=self._agent_name(prompt), permission_mode=self.cfg.new_permission_mode,
            model=self.cfg.new_model, agent=self.cfg.new_agent, extra_args=self.cfg.extra_args,
        )
        self.state.set_last_agent(res.short_id, res.name, None, cwd)
        label = res.name or res.short_id
        summary = f"New agent {label}" if not note else note
        return RouteResult(True, summary, f"claude --bg in {cwd} -> {res.short_id} {res.name}".strip(), "new")

    # -- button A ---------------------------------------------------------

    def _focused(self, prompt: str) -> RouteResult:
        app = self._frontmost()
        if app is None:
            return self._fallback(prompt, "Can't tell which app is in front")
        if app.bundle_id not in self.cfg.focused_apps:
            return self._fallback(prompt, f"{app.name} is in front", app.name)
        try:
            self._paste(prompt, submit=self.cfg.focused_submit, restore_clipboard=self.cfg.restore_clipboard)
        except macos.AccessibilityError:
            self._set_clipboard(prompt)
            return RouteResult(
                False, "Allow Accessibility",
                "macOS blocked the keystrokes. System Settings > Privacy & Security > Accessibility: "
                "enable the app that runs Talkie (e.g. Ghostty or Terminal). Text is on the clipboard.",
                "focused",
            )
        except RuntimeError as e:
            self._set_clipboard(prompt)
            return RouteResult(False, "Paste failed", f"{e}. Text is on the clipboard.", "focused")
        verb = "Sent to" if self.cfg.focused_submit else "Typed into"
        return RouteResult(True, f"{verb} {app.name}", app.bundle_id, "focused")

    def _fallback(self, prompt: str, why: str, app_name: str = "") -> RouteResult:
        fb = self.cfg.focused_fallback
        if fb == "last":
            return self._last(prompt)
        if fb == "new":
            return self._new(prompt)
        if fb == "clipboard":
            self._set_clipboard(prompt)
            return RouteResult(False, "Copied to clipboard",
                               f"{why}, not a terminal; nothing was sent. Paste it where you want it.", "clipboard")
        return RouteResult(False, "Not sent", f"{why}; focus a terminal running Claude Code.", "none")

    def _tmux_target(self) -> Optional[str]:
        if self.cfg.tmux_target:
            return self.cfg.tmux_target
        fmt = "#{pane_id}\t#{pane_current_command}\t#{pane_active}\t#{window_active}\t#{session_attached}\t#{pane_title}"
        r = self._run(["tmux", "list-panes", "-a", "-F", fmt], capture_output=True, text=True, timeout=5)
        if r.returncode != 0:
            return None
        best, best_score = None, -1
        for line in r.stdout.splitlines():
            parts = line.split("\t")
            if len(parts) < 6:
                continue
            pane, cmd, pane_active, win_active, attached, title = parts[:6]
            if cmd != "claude" and "claude" not in title.lower():
                continue
            score = (pane_active == "1") + (win_active == "1") + (attached not in ("", "0")) * 2
            if score > best_score:
                best, best_score = pane, score
        return best

    def _tmux(self, prompt: str) -> RouteResult:
        if not shutil.which("tmux"):
            return RouteResult(False, "tmux not found", "Install tmux or use current_mode = \"focused\"", "tmux")
        target = self._tmux_target()
        if not target:
            return self._fallback(prompt, "No tmux pane is running claude")
        flat = " ".join(prompt.split())  # a newline would submit early
        r1 = self._run(["tmux", "send-keys", "-t", target, "-l", "--", flat], capture_output=True, text=True, timeout=5)
        r2 = self._run(["tmux", "send-keys", "-t", target, "Enter"], capture_output=True, text=True, timeout=5)
        if r1.returncode != 0 or r2.returncode != 0:
            return RouteResult(False, "tmux error", (r1.stderr or r2.stderr).strip()[:200], "tmux")
        return RouteResult(True, f"Sent to tmux {target}", target, "tmux")

    def _last(self, prompt: str) -> RouteResult:
        last = self.state.last_agent
        if not last.get("id"):
            return self._new(prompt, note="No previous agent; started one")
        session = last.get("session") or self.claude.session_id_for(last["id"])
        if not session:
            return self._new(prompt, note="Last agent gone; started one")
        res = self.claude.resume_background(session, prompt, cwd=last.get("cwd"))
        same = res.short_id == last.get("id")
        self.state.set_last_agent(res.short_id, res.name or last.get("name", ""), session if same else None,
                                  last.get("cwd", self.cfg.new_cwd))
        label = res.name or last.get("name") or res.short_id
        return RouteResult(True, f"Continued {label}", f"claude --resume {session} --bg", "last")
