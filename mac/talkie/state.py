"""Small persistent state (last voice-started agent) and the history log."""

from __future__ import annotations

import json
import logging
import time
from pathlib import Path
from typing import Any

log = logging.getLogger("talkie.state")


class State:
    def __init__(self, path: Path) -> None:
        self.path = path
        self.data: dict[str, Any] = {}
        try:
            self.data = json.loads(path.read_text())
        except FileNotFoundError:
            pass
        except (OSError, json.JSONDecodeError) as e:
            log.warning("ignoring unreadable state file %s: %s", path, e)

    def save(self) -> None:
        try:
            self.path.parent.mkdir(parents=True, exist_ok=True)
            tmp = self.path.with_suffix(".tmp")
            tmp.write_text(json.dumps(self.data, indent=2))
            tmp.replace(self.path)
        except OSError as e:
            log.warning("could not save state: %s", e)

    @property
    def last_agent(self) -> dict[str, Any]:
        return self.data.get("last_agent") or {}

    def set_last_agent(self, short_id: str, name: str, session_id: str | None, cwd: str) -> None:
        self.data["last_agent"] = {"id": short_id, "name": name, "session": session_id, "cwd": cwd,
                                   "time": time.time()}
        self.save()


def append_history(path: Path, entry: dict[str, Any]) -> None:
    try:
        path.parent.mkdir(parents=True, exist_ok=True)
        with open(path, "a") as f:
            f.write(json.dumps(entry, ensure_ascii=False) + "\n")
    except OSError as e:
        log.warning("could not write history: %s", e)
