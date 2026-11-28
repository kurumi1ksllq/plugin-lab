"""test_replica_proto.py — Tests for replica_proto.py (issue #106 T8).

replica_proto.py renders ONE chain_doc (v3: {contract_version, generated_at,
source, plugins:[{... stages[]}]}) as the five SPEC.md export documents a DSP
prototype reproduces:

    frequency_response / compression / gr_timeline /
    nonlinear_family / stereo_width

Tests exercise the public surface only: the pure renderer render_replica()
and the CLI (exit codes, files, byte stability). Every fixture is inline and
deterministic (no network, no randomness, no wall clock), so the same chain
doc always renders to the same bytes.

Runs with pytest from the repo root:
    PYTHONPATH=/root/plugin-lab/.venv/lib/python3.15/site-packages \
        python3 -m pytest tools/test_replica_proto.py -q
"""

import copy
import json
import subprocess
import sys
from pathlib import Path

import pytest

# The tools/ scripts are not a package (no __init__.py); import by path.
sys.path.insert(0, str(Path(__file__).resolve().parent))

import replica_proto as rp  # noqa: E402

_SCRIPT = Path(__file__).resolve().parent / "replica_proto.py"

_OUTPUTS = ("frequency_response", "compression", "gr_timeline",
            "nonlinear_family", "stereo_width")


# ---------------------------------------------------------------------------
# Inline fixture builder (minimal valid v3 chain_doc)
# ---------------------------------------------------------------------------


def _stage(kind, params):
    """One v3 stage: {kind, params, provenance, blockers, usable}."""
    return {"kind": kind, "params": params,
            "provenance": {"source": "fixture", "basis": ["inline fixture"]},
            "blockers": [], "usable": True}


def chain_doc(*, eq=True, dynamics=True, nonlinearity=True,
              plugin="Fixture Comp", attack_ms=10.0, release_ms=100.0,
              thd_range=(0.8, 2.1), threshold_db=-30.0, ratio=4.0,
              knee_db=3.0):
    """Build a one-plugin v3 chain_doc with the requested segments."""
    stages = []
    if eq:
        stages.append(_stage("eq", {"overall": "clean", "sections": [
            {"freq_hz": 1000.0, "gain_db": 6.0, "q": 1.5}]}))
    if dynamics:
        stages.append(_stage("dynamics", {
            "threshold_db": threshold_db, "ratio": ratio, "knee_db": knee_db,
            "attack_ms": attack_ms, "release_ms": release_ms,
            "conflict": False}))
    if nonlinearity:
        stages.append(_stage("nonlinearity", {
            "verdict": "clean",
            "thd_range_pct": None if thd_range is None else list(thd_range)}))

    eq_sections = ([{"freq_hz": 1000.0, "gain_db": 6.0, "q": 1.5,
                     "plausible": True, "flag": None, "reason": None}]
                   if eq else [])
    return {
        "contract_version": "3",
        "generated_at": "2026-11-27T00:00:00+00:00",
        "source": {"aggregate_report": "aggregate_report.json",
                   "dataset_dir": None,
                   "report_generated_at": "2026-11-27T00:00:00+00:00"},
        "plugins": [{
            "slug": "fixture",
            "plugin": plugin,
            "plugin_type": {"kind": "compressor", "confidence": "high",
                            "basis": ["inline fixture"]},
            "eq": {"present": bool(eq), "overall": "clean" if eq else "none",
                   "sections": eq_sections, "notes": []},
            "dynamics": {"present": bool(dynamics),
                         "compression": {
                             "threshold_derived": threshold_db if dynamics
                                                  else None,
                             "ratio_derived": ratio if dynamics else None,
                             "threshold_json": None, "ratio_json": None,
                             "knee_json": knee_db if dynamics else None,
                             "conflict": False, "conflict_note": None},
                         "gr": {"attack_ms": attack_ms if dynamics else None,
                                "release_ms": release_ms if dynamics else None,
                                "attack_plausible": True,
                                "release_plausible": True,
                                "release_flag": None, "note": None},
                         "notes": []},
            "nonlinearity": {"verdict": "clean" if nonlinearity
                                        else "not-measured",
                             "thd_range_pct": (list(thd_range)
                                               if nonlinearity and thd_range
                                               else None),
                             "description": None, "reason": None},
            "processing_order": {"order": "unknown", "confidence": "low",
                                 "basis": [],
                                 "suggested": "eq -> dyn -> eq",
                                 "suggestion_note": "canonical, not measured"},
            "stages": stages,
            # A document with no segment at all cannot claim spec usability.
            "usable_as_spec": bool(stages),
            "why_not_spec": [],
        }],
    }


def _render(**kwargs):
    return rp.render_replica(chain_doc(**kwargs))


def _point(points, key, target):
    """The point whose key value is nearest to target."""
    return min(points, key=lambda p: abs(p[key] - target))


def _write(tmp_path, name, doc):
    path = tmp_path / name
    path.write_text(json.dumps(doc, indent=2), encoding="utf-8")
    return path


def _run(args, cwd=None):
    return subprocess.run([sys.executable, str(_SCRIPT)] + [str(a) for a in args],
                          capture_output=True, text=True, cwd=cwd)


# ---------------------------------------------------------------------------
# Shape of the five documents
# ---------------------------------------------------------------------------


def test_render_returns_all_five_documents():
    docs = _render()
    assert set(docs) == set(_OUTPUTS)


def test_every_document_carries_its_type():
    docs = _render()
    assert docs["frequency_response"]["type"] == "frequency_response"
    assert docs["compression"]["type"] == "compression_curve"
    assert docs["gr_timeline"]["type"] == "gr_timeline"
    assert docs["nonlinear_family"]["type"] == "nonlinear_family"
    assert docs["stereo_width"]["type"] == "stereo_width"


def test_context_is_flat_for_freq_and_compression():
    docs = _render()
    for name in ("frequency_response", "compression"):
        doc = docs[name]
        assert doc["plugin"] == "Fixture Comp"
        assert doc["class_id"] == "replica-prototype"
        assert doc["latency_samples"] == 0
        assert doc["sample_rate"] == 48000.0
        assert doc["measurement"] == {"sample_rate": 48000.0,
                                      "block_size": 512}
        assert doc["parameter_snapshot"] == {}
        assert doc["source"] == {"type": "signal"}
        assert "context" not in doc


def test_context_is_nested_for_the_three_context_documents():
    docs = _render()
    for name in ("gr_timeline", "nonlinear_family", "stereo_width"):
        ctx = docs[name]["context"]
        assert ctx["plugin"] == "Fixture Comp"
        assert ctx["class_id"] == "replica-prototype"
        assert ctx["latency_samples"] == 0
        assert ctx["sample_rate"] == 48000.0
        assert ctx["measurement"] == {"sample_rate": 48000.0,
                                      "block_size": 512}
        assert ctx["parameter_snapshot"] == {}
        # SPEC.md §6: gr_timeline is driven by a level step -> "dynamic";
        # the two signal-derived documents keep "signal".
        expected_source = "dynamic" if name == "gr_timeline" else "signal"
        assert ctx["source"] == {"type": expected_source}


# ---------------------------------------------------------------------------
# frequency_response (EQ stages)
# ---------------------------------------------------------------------------


def test_eq_section_renders_a_peak_at_its_frequency():
    doc = _render()["frequency_response"]
    raw = doc["raw"]
    assert len(raw) == 200
    assert raw[0]["f"] == 100.0
    assert raw[-1]["f"] == 10000.0
    # A +6 dB peaking bell at 1 kHz: the peak lands on the section frequency.
    peak = _point(raw, "f", 1000.0)
    assert abs(peak["mag"] - 6.0) < 0.5
    # ... and it is a band, not a tilt: 100 Hz stays near unity.
    assert abs(_point(raw, "f", 100.0)["mag"]) < 1.0
    # The grid is strictly increasing (log sweep).
    assert all(a["f"] < b["f"] for a, b in zip(raw, raw[1:]))


def test_smoothing_is_the_identity():
    doc = _render()["frequency_response"]
    assert doc["smoothed_1_12"] == doc["raw"]
    assert doc["smoothed_1_3"] == doc["raw"]


def test_missing_eq_stage_renders_a_flat_line():
    doc = _render(eq=False)["frequency_response"]
    assert all(p["mag"] == 0.0 for p in doc["raw"])
    assert all(p["phase"] == 0.0 for p in doc["raw"])
    assert len(doc["raw"]) == 200


# ---------------------------------------------------------------------------
# compression_curve
# ---------------------------------------------------------------------------


def test_compression_fitted_reflects_the_dynamics_stage():
    doc = _render()["compression"]
    assert doc["fitted"] == {"ratio": 4.0, "threshold_db": -30.0,
                             "knee_db": 3.0}


def test_compression_curve_is_unity_below_and_compressed_above_threshold():
    curve = _render()["compression"]["curve"]
    assert len(curve) == 31
    assert curve[0]["input_db"] == -60.0
    assert curve[-1]["input_db"] == 0.0
    below = _point(curve, "input_db", -40.0)
    assert below["output_db"] == below["input_db"]
    assert below["gr_db"] == 0.0
    above = _point(curve, "input_db", -10.0)
    # thr -30, ratio 4 at -10 dBFS: -30 + (-10 - -30)/4 = -25 dB out.
    assert abs(above["output_db"] - -25.0) < 1e-9
    assert abs(above["gr_db"] - -15.0) < 1e-9


def test_missing_dynamics_stage_renders_an_identity_curve():
    doc = _render(dynamics=False)["compression"]
    assert doc["fitted"] == {"ratio": 1.0, "threshold_db": 0.0,
                             "knee_db": 0.0}
    assert all(p["output_db"] == p["input_db"] for p in doc["curve"])
    assert all(p["gr_db"] == 0.0 for p in doc["curve"])


# ---------------------------------------------------------------------------
# gr_timeline
# ---------------------------------------------------------------------------


def test_gr_timeline_has_the_fixed_grid():
    gr = _render()["gr_timeline"]["gr"]
    assert gr["sample_rate"] == 48000.0
    assert gr["num_points"] == 101
    assert len(gr["timeline"]) == 101
    assert gr["timeline"][0]["t"] == 0.0
    assert gr["timeline"][-1]["t"] == 0.5


def test_gr_timeline_attacks_then_releases():
    timeline = _render()["gr_timeline"]["gr"]["timeline"]
    assert timeline[0]["gr_db"] == 0.0
    # threshold -30, ratio 4, -6 dBFS excitation -> 18 dB of reduction.
    assert abs(min(p["gr_db"] for p in timeline) - -18.0) < 0.2
    # Everything after the release point climbs back toward 0.
    release = [p for p in timeline if p["t"] >= 0.1]
    assert release[0]["gr_db"] < release[-1]["gr_db"]
    assert abs(release[-1]["gr_db"]) < 1.0
    # Gain reduction is never positive (negative = compression, SPEC 6).
    assert all(p["gr_db"] <= 0.0 for p in timeline)


def test_gr_tau_reflects_the_stage_time_constants():
    tau = _render()["gr_timeline"]["tau"]
    assert tau["valid"] is True
    assert tau["attack_sec"] == 0.01          # 10 ms
    assert tau["release_sec"] == 0.1          # 100 ms
    assert tau["attack_by_level"] == [{"level_db": -6.0, "tau_sec": 0.01}]
    assert tau["release_by_level"] == [{"level_db": -6.0, "tau_sec": 0.1}]


def test_missing_dynamics_stage_renders_zero_gr_and_invalid_tau():
    doc = _render(dynamics=False)["gr_timeline"]
    assert all(p["gr_db"] == 0.0 for p in doc["gr"]["timeline"])
    tau = doc["tau"]
    assert tau["valid"] is False
    assert tau["attack_sec"] == 0.0
    assert tau["release_sec"] == 0.0
    assert tau["attack_by_level"] == [{"level_db": -6.0, "tau_sec": 0.0}]
    assert tau["release_by_level"] == [{"level_db": -6.0, "tau_sec": 0.0}]


# ---------------------------------------------------------------------------
# nonlinear_family
# ---------------------------------------------------------------------------


def test_nonlinear_family_uses_the_spec_level_grid():
    family = _render()["nonlinear_family"]["family"]
    assert [entry["input_level_db"] for entry in family] == [
        -60.0, -48.0, -36.0, -24.0, -12.0, -6.0, 0.0]
    assert all(entry["valid"] is True for entry in family)
    for entry in family:
        assert len(entry["tones"]) == 1
        tone = entry["tones"][0]
        assert tone["fundamental_hz"] == 400.0
        assert [h["order"] for h in tone["harmonics"]] == [2, 3]
        assert [h["freq"] for h in tone["harmonics"]] == [800.0, 1200.0]


def test_thd_rises_with_level_and_hits_the_measured_upper_bound():
    family = _render()["nonlinear_family"]["family"]
    thd = {entry["input_level_db"]: entry["tones"][0]["thd_percent"]
           for entry in family}
    # The cubic coefficient is solved so the -6 dB reference reproduces the
    # measured thd_range_pct upper bound (closed form, no fitting).
    assert abs(thd[-6.0] - 2.1) < 0.01
    assert thd[-60.0] == 0.0
    assert thd[-12.0] < thd[-6.0] < thd[0.0]
    # The third harmonic carries the whole THD; the cubic has no even order.
    tone = _point(family, "input_level_db", -6.0)["tones"][0]
    h2, h3 = tone["harmonics"]
    assert h3["percent"] == tone["thd_percent"]
    assert h2["percent"] == 0.0


def test_missing_nonlinearity_stage_renders_zero_thd():
    family = _render(nonlinearity=False)["nonlinear_family"]["family"]
    assert len(family) == 7
    for entry in family:
        tone = entry["tones"][0]
        assert tone["thd_percent"] == 0.0
        assert tone["fundamental_db"] == entry["input_level_db"]
        assert tone["harmonics"][1]["percent"] == 0.0
        assert tone["harmonics"][1]["mag_db"] == -120.0


def test_nonlinearity_without_a_thd_range_renders_zero_thd():
    family = _render(thd_range=None)["nonlinear_family"]["family"]
    assert all(entry["tones"][0]["thd_percent"] == 0.0 for entry in family)


# ---------------------------------------------------------------------------
# stereo_width
# ---------------------------------------------------------------------------


def test_stereo_width_of_a_mono_replica():
    docs = _render()
    stereo = docs["stereo_width"]
    mid = docs["frequency_response"]["raw"]
    assert stereo["mid_response"] == mid
    assert len(stereo["side_response"]) == len(mid)
    assert all(p["mag"] == -120.0 and p["phase"] == 0.0
               for p in stereo["side_response"])
    assert all(p["rho"] == 1.0 for p in stereo["correlation"])
    assert [p["f"] for p in stereo["width_db"]] == [p["f"] for p in mid]
    for width, point in zip(stereo["width_db"], mid):
        assert width["width_db"] == pytest.approx(point["mag"] + 120.0,
                                                  abs=1e-9)


# ---------------------------------------------------------------------------
# Determinism
# ---------------------------------------------------------------------------


def test_render_is_deterministic():
    doc = chain_doc()
    assert rp.render_replica(doc) == rp.render_replica(doc)
    assert (json.dumps(rp.render_replica(doc), indent=2, ensure_ascii=False)
            == json.dumps(rp.render_replica(doc), indent=2,
                          ensure_ascii=False))


def test_render_does_not_mutate_the_chain_doc():
    doc = chain_doc()
    before = copy.deepcopy(doc)
    rp.render_replica(doc)
    assert doc == before


# ---------------------------------------------------------------------------
# Input validation
# ---------------------------------------------------------------------------


def test_invalid_chain_doc_raises_valueerror():
    with pytest.raises(ValueError):
        rp.render_replica({"contract_version": "3"})


def test_multi_plugin_chain_doc_raises_valueerror():
    doc = chain_doc()
    doc["plugins"].append(copy.deepcopy(doc["plugins"][0]))
    with pytest.raises(ValueError, match="1 plugin"):
        rp.render_replica(doc)


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------


def test_cli_writes_five_documents(tmp_path):
    source = _write(tmp_path, "chain.json", chain_doc())
    out = tmp_path / "out"
    proc = _run([source, "--out-dir", out])
    assert proc.returncode == 0, proc.stderr
    written = sorted(p.name for p in out.iterdir())
    assert written == sorted(f"{name}.json" for name in _OUTPUTS)
    rendered = rp.render_replica(chain_doc())
    for name in _OUTPUTS:
        doc = json.loads((out / f"{name}.json").read_text(encoding="utf-8"))
        assert doc == rendered[name], name


def test_cli_output_is_byte_stable_across_runs(tmp_path):
    source = _write(tmp_path, "chain.json", chain_doc())
    first, second = tmp_path / "first", tmp_path / "second"
    assert _run([source, "--out-dir", first]).returncode == 0
    assert _run([source, "--out-dir", second]).returncode == 0
    for name in _OUTPUTS:
        assert ((first / f"{name}.json").read_bytes()
                == (second / f"{name}.json").read_bytes()), name


def test_cli_written_files_end_with_a_newline(tmp_path):
    source = _write(tmp_path, "chain.json", chain_doc())
    out = tmp_path / "out"
    assert _run([source, "--out-dir", out]).returncode == 0
    payload = (out / "frequency_response.json").read_bytes()
    assert payload.endswith(b"\n")
    assert payload.decode("utf-8").startswith("{")


def test_cli_missing_input_exits_2(tmp_path):
    proc = _run([tmp_path / "nope.json", "--out-dir", tmp_path / "out"])
    assert proc.returncode == 2
    assert proc.stderr.startswith("error: ")
    assert len(proc.stderr.strip().splitlines()) == 1


def test_cli_broken_json_exits_2(tmp_path):
    bad = tmp_path / "bad.json"
    bad.write_text("{not json", encoding="utf-8")
    proc = _run([bad, "--out-dir", tmp_path / "out"])
    assert proc.returncode == 2
    assert proc.stderr.startswith("error: ")


def test_cli_invalid_chain_doc_exits_2(tmp_path):
    bad = _write(tmp_path, "bad.json", {"contract_version": "3"})
    proc = _run([bad, "--out-dir", tmp_path / "out"])
    assert proc.returncode == 2
    assert proc.stderr.startswith("error: ")
