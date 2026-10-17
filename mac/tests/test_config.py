from pathlib import Path

import pytest

from talkie.config import Config, load

EXAMPLE = Path(__file__).resolve().parents[1] / "config.example.toml"


def test_example_config_loads_cleanly():
    cfg = load(EXAMPLE)
    assert cfg.warnings == []
    assert cfg.route.current_mode == "focused"
    assert "com.mitchellh.ghostty" in cfg.route.focused_apps
    assert not cfg.transcribe.model.startswith("~")  # expanded
    assert cfg.route.new_cwd == str(Path.home())


def test_bad_values_warn_and_fall_back(tmp_path):
    p = tmp_path / "c.toml"
    p.write_text('[route]\ncurrent_mode = "telepathy"\nfocused_submit = "yes"\n[bogus]\nx = 1\n'
                 '[audio]\nkeep_clips = 3\nmin_seconds = 1\n')
    cfg = load(p)
    assert cfg.route.current_mode == "focused"
    assert cfg.route.focused_submit is True
    assert cfg.audio.keep_clips == 3 and cfg.audio.min_seconds == 1.0
    assert any("telepathy" in w for w in cfg.warnings)
    assert any("bogus" in w for w in cfg.warnings)
    assert any("focused_submit" in w for w in cfg.warnings)


def test_missing_explicit_config_raises(tmp_path):
    with pytest.raises(FileNotFoundError):
        load(tmp_path / "nope.toml")


def test_defaults_without_file(tmp_path, monkeypatch):
    cfg = Config(home=tmp_path)
    assert cfg.clips_dir == tmp_path / "clips"
