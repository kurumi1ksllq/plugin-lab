"""Tests for batch_collect.py setup-preset support (issue #72).

Covers the config-schema validation for the new "setup" key
(_validate_setup via validate_config) and the per-plugin application of
setup setParams inside process_one (order + failure handling), plus the
dry-run display. The process_one tests drive a fake pipe module that
records every send_line payload and replays scripted read_line responses,
so no real app / named pipe is needed.

Usage:
    python -m pytest tools/test_batch_collect.py -q
"""
import argparse
import json

import pytest

import batch_collect as bc


# ---------------------------------------------------------------------------
# validate_config: setup schema
# ---------------------------------------------------------------------------


def test_validate_config_setup_param_id_and_name():
    """A setup list mixing param_id and name entries passes validation."""
    data = {"plugins": {"Pro-Q 4": {"setup": [
        {"name": "Band 1 Used", "value": 1.0},
        {"param_id": "2", "value": 0.5752}]}}}
    plugins = bc.validate_config(data)
    assert plugins["Pro-Q 4"]["setup"] == [
        {"name": "Band 1 Used", "value": 1.0},
        {"param_id": "2", "value": 0.5752}]


def test_validate_config_setup_null_skipped():
    """A null setup block is skipped (compression_family semantics)."""
    data = {"plugins": {"Pro-Q 4": {"setup": None}}}
    assert bc.validate_config(data) == data["plugins"]


def test_validate_config_setup_empty_list_allowed():
    """An empty setup list is a valid no-op."""
    data = {"plugins": {"Pro-Q 4": {"setup": []}}}
    assert bc.validate_config(data) == data["plugins"]


def test_validate_config_setup_unknown_key():
    """An unknown setup key is rejected, naming the entry index."""
    data = {"plugins": {"Pro-Q 4": {"setup": [
        {"name": "Band 1 Used", "value": 1.0, "bogus": 1}]}}}
    with pytest.raises(ValueError, match="unknown setup key"):
        bc.validate_config(data)


def test_validate_config_setup_value_missing():
    """A setup entry without a value is rejected."""
    data = {"plugins": {"Pro-Q 4": {"setup": [{"name": "Band 1 Used"}]}}}
    with pytest.raises(ValueError, match="value"):
        bc.validate_config(data)


def test_validate_config_setup_value_non_number():
    """A non-numeric value is rejected."""
    data = {"plugins": {"Pro-Q 4": {"setup": [
        {"name": "Band 1 Used", "value": "1.0"}]}}}
    with pytest.raises(ValueError, match="value"):
        bc.validate_config(data)


def test_validate_config_setup_value_bool_rejected():
    """A bool value is not a number (bool is an int subclass)."""
    data = {"plugins": {"Pro-Q 4": {"setup": [
        {"name": "Band 1 Used", "value": True}]}}}
    with pytest.raises(ValueError, match="value"):
        bc.validate_config(data)


def test_validate_config_setup_value_out_of_range():
    """A value outside [0, 1] is rejected (setParam contract)."""
    data = {"plugins": {"Pro-Q 4": {"setup": [
        {"name": "Band 1 Used", "value": 1.5}]}}}
    with pytest.raises(ValueError, match="value"):
        bc.validate_config(data)


def test_validate_config_setup_value_nan_rejected():
    """A NaN value is not a finite number in [0, 1]."""
    data = {"plugins": {"Pro-Q 4": {"setup": [
        {"name": "Band 1 Used", "value": float("nan")}]}}}
    with pytest.raises(ValueError, match="value"):
        bc.validate_config(data)


def test_validate_config_setup_missing_both_name_and_param_id():
    """An entry with neither name nor param_id is rejected."""
    data = {"plugins": {"Pro-Q 4": {"setup": [{"value": 1.0}]}}}
    with pytest.raises(ValueError, match="exactly one"):
        bc.validate_config(data)


def test_validate_config_setup_both_name_and_param_id():
    """An entry with both name and param_id is rejected."""
    data = {"plugins": {"Pro-Q 4": {"setup": [
        {"name": "Band 1 Used", "param_id": "2", "value": 1.0}]}}}
    with pytest.raises(ValueError, match="exactly one"):
        bc.validate_config(data)


def test_validate_config_setup_not_a_list():
    """A non-list setup block is rejected."""
    data = {"plugins": {"Pro-Q 4": {"setup": {"name": "Band 1 Used",
                                              "value": 1.0}}}}
    with pytest.raises(ValueError, match="'setup' must be a list"):
        bc.validate_config(data)


# ---------------------------------------------------------------------------
# process_one: setup application (fake pipe module)
# ---------------------------------------------------------------------------


class _FakePipe:
    """Records every send_line payload; replays scripted read_line responses."""

    def __init__(self, responses):
        self.sent = []
        self._responses = list(responses)

    def send_line(self, handle, line):
        self.sent.append(json.loads(line))

    def read_line(self, handle, timeout_sec=None):
        return json.dumps(self._responses.pop(0))


def _entry(cfg):
    return bc.PlanEntry("Pro-Q 4", "C:/plugins/ProQ4.vst3", "pro-q-4", cfg)


def test_process_one_applies_setup_before_dataset(monkeypatch, tmp_path):
    """setup setParams are sent in order after loadPlugin/getParams and
    before the dataset command; param_id entries send only param_id."""
    monkeypatch.setattr(bc, "run_reverse_derive",
                        lambda path, expected: (0, "report"))
    fake = _FakePipe([
        {"ok": True, "name": "Pro-Q 4"},                     # loadPlugin
        {"ok": True, "params": []},                          # getParams (loaded)
        {"ok": True, "param": "2", "value": 0.5752},         # setParam param_id
        {"ok": True, "param": "Band 1 Used", "value": 1.0},  # setParam name
        {"ok": True, "types": {"frequency_response": True},  # dataset
         "scan": True},
    ])
    entry = _entry({"setup": [
        {"param_id": "2", "value": 0.5752},
        {"name": "Band 1 Used", "value": 1.0}]})

    result = bc.process_one(fake, 1, entry, tmp_path, None)

    cmds = [p["cmd"] for p in fake.sent]
    assert cmds == ["loadPlugin", "getParams", "setParam", "setParam", "dataset"]
    assert fake.sent[2] == {"cmd": "setParam", "param_id": "2", "value": 0.5752}
    assert fake.sent[3] == {"cmd": "setParam", "name": "Band 1 Used", "value": 1.0}
    assert fake.sent[4]["cmd"] == "dataset"
    assert fake.sent[4]["path"].endswith("dataset.json")
    assert result["ok"] is True


def test_process_one_setup_failure_marks_entry_failed(tmp_path):
    """A setParam ok:false fails the entry with a setup-named reason."""
    fake = _FakePipe([
        {"ok": True, "name": "Pro-Q 4"},
        {"ok": True, "params": []},
        {"ok": False, "error": "no such parameter"},
    ])
    entry = _entry({"setup": [{"name": "Band 1 Used", "value": 1.0}]})

    result = bc.process_one(fake, 1, entry, tmp_path, None)

    assert result["ok"] is False
    assert result["skip_reason"] == "setup name 'Band 1 Used': no such parameter"


def test_process_one_setup_retries_transient_no_plugin(tmp_path, monkeypatch):
    """Issue #81: a setup setParam can transiently see 'no plugin loaded'
    while the async load settles (FabFilter Pro-L 2). The transient error is
    retried within the setup-retry window; once it succeeds the dataset
    battery proceeds. Other setParam errors are NOT retried."""
    monkeypatch.setattr(bc, "run_reverse_derive",
                        lambda path, expected: (0, "report"))
    fake = _FakePipe([
        {"ok": True, "name": "Pro-L 2"},
        {"ok": True, "params": []},
        {"ok": False, "error": "no plugin loaded"},   # transient
        {"ok": False, "error": "no plugin loaded"},   # transient again
        {"ok": True, "param": "0", "value": 0.8},     # settled
        {"ok": True, "types": {"frequency_response": True}, "scan": True},
    ])
    entry = _entry({"setup": [{"param_id": "0", "value": 0.8}]})

    result = bc.process_one(fake, 1, entry, tmp_path, None)

    cmds = [p["cmd"] for p in fake.sent]
    # loadPlugin, getParams, then setParam x3 (2 transient + 1 success), dataset
    assert cmds == ["loadPlugin", "getParams",
                    "setParam", "setParam", "setParam", "dataset"]
    assert result["ok"] is True


def test_process_one_no_setup_sends_no_setparam(tmp_path):
    """Without a setup block no setParam command is sent."""
    fake = _FakePipe([
        {"ok": True, "name": "Pro-Q 4"},
        {"ok": True, "params": []},
        {"ok": False, "error": "boom"},   # dataset fails -> early return
    ])
    entry = _entry({})

    bc.process_one(fake, 1, entry, tmp_path, None)

    assert [p["cmd"] for p in fake.sent] == ["loadPlugin", "getParams", "dataset"]


# ---------------------------------------------------------------------------
# run_dry_run: setup display
# ---------------------------------------------------------------------------


def _dry_run_args(config_path, plugin="Pro-Q 4"):
    return argparse.Namespace(plugin=plugin, all=False, limit=None,
                              config=str(config_path), out="out", types=None,
                              dry_run=True, launch=False, quit=False)


def test_dry_run_shows_setup_count(capsys, tmp_path, monkeypatch):
    """A valid setup config prints setup=N on the per-plugin line."""
    cache = tmp_path / "pluginlist.xml"
    cache.write_text('<root><PLUGIN name="Pro-Q 4" file="C:/plugins/ProQ4.vst3"/>'
                     "</root>", encoding="utf-8")
    monkeypatch.setattr(bc, "cache_path", lambda: cache)
    cfg = tmp_path / "cfg.json"
    cfg.write_text(json.dumps({"plugins": {"Pro-Q 4": {"setup": [
        {"name": "Band 1 Used", "value": 1.0},
        {"param_id": "2", "value": 0.5752}]}}}), encoding="utf-8")

    code = bc.run_dry_run(_dry_run_args(cfg))

    out = capsys.readouterr().out
    assert code == 0
    assert "setup=2" in out


def test_dry_run_rejects_invalid_setup(capsys, tmp_path, monkeypatch):
    """An out-of-range setup value fails dry-run validation (exit 1)."""
    cache = tmp_path / "pluginlist.xml"
    cache.write_text('<root><PLUGIN name="Pro-Q 4" file="C:/plugins/ProQ4.vst3"/>'
                     "</root>", encoding="utf-8")
    monkeypatch.setattr(bc, "cache_path", lambda: cache)
    cfg = tmp_path / "cfg.json"
    cfg.write_text(json.dumps({"plugins": {"Pro-Q 4": {"setup": [
        {"name": "Band 1 Used", "value": 2.0}]}}}), encoding="utf-8")

    with pytest.raises(SystemExit) as excinfo:
        bc.run_dry_run(_dry_run_args(cfg))

    assert excinfo.value.code == 1
    assert "invalid config" in capsys.readouterr().out
