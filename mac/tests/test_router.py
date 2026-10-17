import subprocess

import pytest

from talkie import macos, protocol as P
from talkie.agents import ClaudeCLI, parse_bg_output, safe_prompt
from talkie.config import RouteConfig
from talkie.router import Router
from talkie.state import State


class Spy:
    def __init__(self, front=("com.mitchellh.ghostty", "Ghostty"), paste_error=None):
        self.front = macos.FrontApp(*front) if front else None
        self.pasted = []
        self.clipboard = None
        self.paste_error = paste_error

    def frontmost(self):
        return self.front

    def paste(self, text, submit=True, restore_clipboard=True):
        if self.paste_error:
            raise self.paste_error
        self.pasted.append((text, submit))

    def set_clipboard(self, text):
        self.clipboard = text
        return True


def make(tmp_path, fake_claude=None, spy=None, **cfg_kw):
    cfg = RouteConfig(new_cwd=str(tmp_path), **cfg_kw)
    if fake_claude:
        cfg.claude_bin = str(fake_claude[0])
    spy = spy or Spy()
    r = Router(cfg, State(tmp_path / "state.json"), ClaudeCLI(cfg.claude_bin, 10),
               frontmost=spy.frontmost, paste=spy.paste, set_clipboard=spy.set_clipboard)
    return r, spy


def test_focused_pastes_into_allowed_terminal(tmp_path):
    r, spy = make(tmp_path)
    res = r.route(P.TARGET_CURRENT, "run the tests")
    assert res.ok and res.summary == "Sent to Ghostty"
    assert spy.pasted == [("run the tests", True)]


def test_focused_refuses_non_terminal_and_copies(tmp_path):
    r, spy = make(tmp_path, spy=Spy(front=("com.tinyspeck.slackmacgap", "Slack")))
    res = r.route(P.TARGET_CURRENT, "secret plan")
    assert not res.ok and res.summary == "Copied to clipboard"
    assert spy.pasted == [] and spy.clipboard == "secret plan"
    assert "Slack" in res.detail


def test_focused_fallback_none(tmp_path):
    r, spy = make(tmp_path, spy=Spy(front=("com.apple.Safari", "Safari")), focused_fallback="none")
    res = r.route(P.TARGET_CURRENT, "x")
    assert not res.ok and spy.clipboard is None


def test_accessibility_error_is_explained(tmp_path):
    r, spy = make(tmp_path, spy=Spy(paste_error=macos.AccessibilityError("1002")))
    res = r.route(P.TARGET_CURRENT, "hello")
    assert not res.ok and res.summary == "Allow Accessibility"
    assert spy.clipboard == "hello"


def test_prefix_is_applied(tmp_path):
    r, spy = make(tmp_path, prompt_prefix="(voice) ")
    r.route(P.TARGET_CURRENT, "hi")
    assert spy.pasted[0][0] == "(voice) hi"


def test_new_agent_uses_claude_bg(tmp_path, fake_claude):
    r, _ = make(tmp_path, fake_claude, new_permission_mode="acceptEdits", new_name="v-{words}")
    res = r.route(P.TARGET_NEW, "- fix the flaky test please")
    assert res.ok and res.summary == "New agent fix-the-tests"
    args = fake_claude[1].read_text().strip().splitlines()[-1]
    assert args.startswith("--bg --name v-fix-the-flaky-test --permission-mode acceptEdits")
    assert args.endswith("fix the flaky test please")  # leading dash stripped
    assert r.state.last_agent["id"] == "1234abcd"


def test_new_agent_trust_error(tmp_path, fake_claude, monkeypatch):
    monkeypatch.setenv("FAKE_CLAUDE_FAIL", "trust")
    r, _ = make(tmp_path, fake_claude)
    res = r.route(P.TARGET_NEW, "hello")
    assert not res.ok and "not trusted" in res.detail.lower()


def test_last_mode_resumes_previous_agent(tmp_path, fake_claude):
    r, _ = make(tmp_path, fake_claude, current_mode="last")
    first = r.route(P.TARGET_CURRENT, "start something")  # no previous agent yet: starts one
    assert first.ok and "started" in first.summary
    second = r.route(P.TARGET_CURRENT, "and also this")
    assert second.ok and second.summary.startswith("Continued")
    calls = fake_claude[1].read_text().splitlines()
    assert any(c.startswith("agents --json --all") for c in calls)
    assert calls[-1].startswith("--resume 0f0e-uuid --bg and also this")


def test_missing_claude_binary(tmp_path):
    r, _ = make(tmp_path, claude_bin="/nonexistent/claude")
    res = r.route(P.TARGET_NEW, "hello")
    assert not res.ok and "not found" in res.detail


def test_tmux_picks_active_claude_pane(tmp_path, monkeypatch):
    calls = []

    def fake_run(cmd, **kw):
        calls.append(cmd)
        if cmd[1] == "list-panes":
            out = ("%1\tzsh\t1\t1\t1\tshell\n"
                   "%2\tclaude\t0\t0\t1\t* Claude Code\n"
                   "%3\tclaude\t1\t1\t1\t* Claude Code\n")
            return subprocess.CompletedProcess(cmd, 0, out, "")
        return subprocess.CompletedProcess(cmd, 0, "", "")

    monkeypatch.setattr("shutil.which", lambda name, *a, **k: "/usr/bin/tmux" if name == "tmux" else None)
    cfg = RouteConfig(current_mode="tmux", new_cwd=str(tmp_path))
    r = Router(cfg, State(tmp_path / "s.json"), ClaudeCLI("claude"), run=fake_run)
    res = r.route(P.TARGET_CURRENT, "two\nlines")
    assert res.ok and res.summary == "Sent to tmux %3"
    assert calls[1] == ["tmux", "send-keys", "-t", "%3", "-l", "--", "two lines"]
    assert calls[2] == ["tmux", "send-keys", "-t", "%3", "Enter"]


@pytest.mark.parametrize("out,expected", [
    ("Starting background service...\nbackgrounded · 7c5dcf5d · flaky-test-fix\n", ("7c5dcf5d", "flaky-test-fix")),
    ("backgrounded · 7c5dcf5d\n  claude agents  list sessions\n", ("7c5dcf5d", "")),
    ("error\n", None),
])
def test_parse_bg_output(out, expected):
    res = parse_bg_output(out)
    assert (res.short_id, res.name) == expected if expected else res is None


def test_safe_prompt():
    assert safe_prompt("--help me") == "help me"
    assert safe_prompt("fix it") == "fix it"


def test_agent_summary(tmp_path, fake_claude):
    cli = ClaudeCLI(str(fake_claude[0]))
    assert cli.summary() == "1 working, 1 needs you"
    assert cli.session_id_for("1234abcd") == "0f0e-uuid"
