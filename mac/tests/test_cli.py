"""The command line, run as a user would (with a fake speech-to-text engine)."""

import os
import subprocess
import sys
from pathlib import Path

from talkie import audio
from conftest import speechlike

MAC_DIR = Path(__file__).resolve().parents[1]


def run_cli(tmp_path, *args, extra_cfg=""):
    stt = tmp_path / "fake_stt.py"
    stt.write_text("import sys\nprint('open the readme and summarize it')\n")
    cfg = tmp_path / "config.toml"
    cfg.write_text(f"""
[transcribe]
backend = "command"
command = ["{sys.executable}", "{stt}", "{{wav}}"]
[feedback]
notifications = false
{extra_cfg}
""")
    env = dict(os.environ, TALKIE_HOME=str(tmp_path / "home"), PYTHONPATH=str(MAC_DIR))
    return subprocess.run([sys.executable, "-m", "talkie", "--config", str(cfg), *args],
                          capture_output=True, text=True, env=env, cwd=tmp_path, timeout=60)


def test_simulate_dry_run(tmp_path):
    wav = tmp_path / "speech.wav"
    audio.write_wav(wav, speechlike(3.0), 16000)
    r = run_cli(tmp_path, "simulate", str(wav), "--loss", "0.2", "--dry-run")
    out = r.stdout + r.stderr
    assert r.returncode == 0, out
    assert "open the readme and summarize it" in out
    assert "ack status 0" in out  # every lost packet was recovered


def test_simulate_new_agent_with_fake_claude(tmp_path, fake_claude):
    wav = tmp_path / "speech.wav"
    audio.write_wav(wav, speechlike(2.0), 16000)
    r = run_cli(tmp_path, "simulate", str(wav), "--new",
                extra_cfg=f'[route]\nclaude_bin = "{fake_claude[0]}"\nnew_cwd = "{tmp_path}"\n')
    assert r.returncode == 0, r.stdout + r.stderr
    assert "New agent fix-the-tests" in r.stdout
    assert fake_claude[1].read_text().strip().endswith("open the readme and summarize it")


def test_send_dry_run_and_bad_config(tmp_path):
    r = run_cli(tmp_path, "send", "--new", "--dry-run", "hello", "there")
    assert r.returncode == 0 and "Dry run" in r.stdout
    bad = tmp_path / "bad.toml"
    bad.write_text("this is = = not toml")
    env = dict(os.environ, PYTHONPATH=str(MAC_DIR))
    r = subprocess.run([sys.executable, "-m", "talkie", "--config", str(bad), "doctor"],
                       capture_output=True, text=True, env=env, timeout=60)
    assert r.returncode == 2 and "cannot read config" in r.stderr
