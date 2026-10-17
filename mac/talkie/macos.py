"""Small macOS helpers: frontmost app, clipboard, keystrokes, notifications."""

from __future__ import annotations

import logging
import os
import re
import subprocess
import sys
import threading
from dataclasses import dataclass
from typing import Optional

log = logging.getLogger("talkie.macos")

IS_MAC = sys.platform == "darwin"
_UTF8 = dict(os.environ, LANG="en_US.UTF-8", LC_ALL="en_US.UTF-8")


class AccessibilityError(RuntimeError):
    """macOS refused to let us send keystrokes."""


@dataclass
class FrontApp:
    bundle_id: str
    name: str


def _run(cmd: list[str], timeout: float = 5.0, input_text: Optional[str] = None) -> subprocess.CompletedProcess:
    return subprocess.run(cmd, capture_output=True, text=True, timeout=timeout, input=input_text, env=_UTF8)


def frontmost_app() -> Optional[FrontApp]:
    """The app in front, via lsappinfo (needs no special permission)."""
    try:
        asn = _run(["lsappinfo", "front"]).stdout.strip()
        if not asn:
            return None
        out_id = _run(["lsappinfo", "info", "-only", "bundleid", asn]).stdout
        out_name = _run(["lsappinfo", "info", "-only", "name", asn]).stdout
    except (OSError, subprocess.SubprocessError):
        return None
    bundle = re.search(r'"?(?:CFBundleIdentifier|bundleID)"?\s*=\s*"([^"]*)"', out_id)
    name = re.search(r'"?(?:LSDisplayName|CFBundleName|name)"?\s*=\s*"([^"]*)"', out_name)
    if not bundle:
        return None
    return FrontApp(bundle.group(1), name.group(1) if name else bundle.group(1))


def get_clipboard() -> Optional[str]:
    try:
        r = _run(["pbpaste"])
        return r.stdout if r.returncode == 0 else None
    except (OSError, subprocess.SubprocessError):
        return None


def set_clipboard(text: str) -> bool:
    try:
        return _run(["pbcopy"], input_text=text).returncode == 0
    except (OSError, subprocess.SubprocessError):
        return False


def _osascript(lines: list[str]) -> subprocess.CompletedProcess:
    cmd = ["osascript"]
    for line in lines:
        cmd += ["-e", line]
    return _run(cmd, timeout=10.0)


def paste(text: str, submit: bool = True, restore_clipboard: bool = True) -> None:
    """Paste `text` into the frontmost app and optionally press Return.

    Uses the clipboard plus Cmd-V (reliable for long text and any keyboard
    layout). Needs Accessibility permission for the app running Talkie.
    """
    previous = get_clipboard() if restore_clipboard else None
    if not set_clipboard(text):
        raise RuntimeError("could not set the clipboard")
    settle = min(1.0, 0.2 + len(text) / 8000.0)  # long pastes take a moment to land
    lines = ['tell application "System Events" to keystroke "v" using command down']
    if submit:
        lines += [f"delay {settle:.2f}", 'tell application "System Events" to key code 36']
    r = _osascript(lines)
    if r.returncode != 0:
        err = (r.stderr or "").strip()
        if "1002" in err or "not allowed" in err.lower() or "assistive" in err.lower() or "1743" in err:
            raise AccessibilityError(err)
        raise RuntimeError(err or "osascript failed")
    if restore_clipboard and previous is not None:
        def restore() -> None:
            if get_clipboard() == text:  # don't clobber something the user copied since
                set_clipboard(previous)
        threading.Timer(1.5, restore).start()


def _escape(s: str) -> str:
    return s.replace("\\", "\\\\").replace('"', '\\"').replace("\n", " ")


def notify(title: str, message: str, subtitle: str = "") -> None:
    if not IS_MAC:
        return
    script = f'display notification "{_escape(message[:240])}" with title "{_escape(title)}"'
    if subtitle:
        script += f' subtitle "{_escape(subtitle[:120])}"'
    try:
        _osascript([script])
    except (OSError, subprocess.SubprocessError) as e:
        log.debug("notification failed: %s", e)
