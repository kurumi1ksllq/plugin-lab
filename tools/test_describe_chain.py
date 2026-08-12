"""test_describe_chain.py — Tests for describe_chain.py (chain_doc builders).

Wave-2 Task T4 of issue #26: the processing-chain description generator.
Given the aggregate_report rows (tools/aggregate_report.py) plus optional
per-plugin dataset.json enrichment, describe_chain builds the chain_doc
per-plugin contract consumed by describe_render.py (and the T5 validator):

    {slug, plugin, plugin_type:{kind,confidence,basis[]},
     eq:{present,overall,sections[],notes[]},
     dynamics:{present,compression:{...},gr:{...},notes[]},
     nonlinearity:{verdict,thd_range_pct?,description?,reason?},
     processing_order:{order,confidence,basis[],suggested?,suggestion_note?},
     usable_as_spec:bool, why_not_spec[]}

Tests follow the red→green discipline: fixtures come from
test_data/chain_fixtures.py (deterministic builders), tmp_path is the
pytest builtin for dataset.json enrichment scenarios, and the CLI is
exercised via subprocess (sys.executable).

Runs with pytest from the repo root:
    python -m pytest tools/test_describe_chain.py -q
"""

import json
import subprocess
import sys
from pathlib import Path

# The tools/ scripts are not a package (no __init__.py); import by path.
sys.path.insert(0, str(Path(__file__).resolve().parent))

import describe_chain as dc  # noqa: E402
import describe_quality as dq  # noqa: E402
from test_data.chain_fixtures import (  # noqa: E402
    make_analyzer_snapshot,
    make_compression_clean,
    make_compression_conflict,
    make_compression_degenerate,
    make_compressor_snapshot,
    make_eq_unused_snapshot,
    make_freq_artifact,
    make_freq_clean,
    make_freq_degenerate,
    make_gr_clean,
    make_gr_implausible,
    make_gr_invalid,
    make_harmonic_artifact,
    make_harmonic_clean,
    make_harmonic_none,
    make_pro_c3_row,
    make_pro_q4_row,
    make_scepter_row,
)

_META = {"generated_at": "2026-08-13T00:00:00+00:00",
         "aggregate_report": "aggregate_report.json",
         "report_generated_at": "2026-08-13T00:00:00+00:00"}

_SCRIPT = Path(__file__).resolve().parent / "describe_chain.py"


# ---------------------------------------------------------------------------
# enrich_from_dataset
# ---------------------------------------------------------------------------


def _write_dataset(tmp_path, payload):
    path = tmp_path / "dataset.json"
    path.write_text(json.dumps(payload), encoding="utf-8")
    return path


def test_enrich_minimal_dataset(tmp_path):
    """A well-formed dataset.json yields the enriched context fields."""
    snapshot = make_analyzer_snapshot()
    path = _write_dataset(tmp_path, {
        "context": {"parameter_snapshot": snapshot, "sample_rate": 48000},
        "harmonic": {"tones": [{"freq": 100.0}]},
        "compression_family": True,
    })
    result = dc.enrich_from_dataset(str(path))
    assert result["parameter_snapshot"] == snapshot
    assert result["harmonic_block"] == [{"freq": 100.0}]
    assert result["sample_rate"] == 48000.0
    assert isinstance(result["sample_rate"], float)
    assert result["has_compression_family"] is True


def test_enrich_sample_rate_fallback(tmp_path):
    """context.sample_rate missing → context.measurement.sample_rate."""
    path = _write_dataset(tmp_path, {
        "context": {"measurement": {"sample_rate": 96000}},
        "harmonic": {"tones": []},
    })
    result = dc.enrich_from_dataset(str(path))
    assert result["sample_rate"] == 96000.0
    assert result["has_compression_family"] is False
    assert result["parameter_snapshot"] is None


def test_enrich_missing_file(tmp_path):
    """Missing dataset.json → all-None/false defensive shape, never raises."""
    result = dc.enrich_from_dataset(str(tmp_path / "nope" / "dataset.json"))
    assert result == {"parameter_snapshot": None, "harmonic_block": None,
                      "sample_rate": None, "has_compression_family": False}


def test_enrich_corrupt_json(tmp_path):
    """Unparseable JSON → all-None/false defensive shape, never raises."""
    path = tmp_path / "dataset.json"
    path.write_text("{not json", encoding="utf-8")
    result = dc.enrich_from_dataset(str(path))
    assert result == {"parameter_snapshot": None, "harmonic_block": None,
                      "sample_rate": None, "has_compression_family": False}


# ---------------------------------------------------------------------------
# classify_plugin_type
# ---------------------------------------------------------------------------


def test_classify_analyzer_snapshot():
    """FFT Size / Hold Peaks / Smoothing keys → analyzer, high confidence."""
    result = dc.classify_plugin_type(make_analyzer_snapshot())
    assert result["kind"] == "analyzer"
    assert result["confidence"] == "high"
    assert result["basis"]


def test_classify_compressor_snapshot():
    """Threshold + Ratio + Attack/Release keys → compressor."""
    result = dc.classify_plugin_type(make_compressor_snapshot())
    assert result["kind"] == "compressor"
    assert result["confidence"] == "high"


def test_classify_eq_unused_snapshot():
    """Band keys with no dynamics keys → eq-only; basis notes unused bands."""
    result = dc.classify_plugin_type(make_eq_unused_snapshot())
    assert result["kind"] == "eq-only"
    assert any("unused" in item.lower() for item in result["basis"])


def test_classify_no_snapshot():
    """None / {} snapshot → unknown, low confidence, explicit basis."""
    for snapshot in (None, {}):
        result = dc.classify_plugin_type(snapshot)
        assert result["kind"] == "unknown"
        assert result["confidence"] == "low"
        assert "no parameter snapshot" in result["basis"]


def test_classify_processor_heuristic():
    """No snapshot + row with >= 2 real-content blocks → processor heuristic."""
    result = dc.classify_plugin_type(None, row=make_pro_c3_row())
    assert result["kind"] == "processor"
    assert result["confidence"] == "low"
    assert any("processor" in item.lower() for item in result["basis"])


def test_classify_unknown_row_with_one_block():
    """Degenerate row (single real block) + no snapshot → still unknown."""
    result = dc.classify_plugin_type(None, row=make_scepter_row())
    assert result["kind"] == "unknown"


# ---------------------------------------------------------------------------
# build_eq
# ---------------------------------------------------------------------------


def test_build_eq_artifact():
    """pro-c-3 Q=18098.4 → overall artifact, implausible section with flag."""
    eq = dc.build_eq(make_freq_artifact(), {})
    assert eq["present"] is True
    assert eq["overall"] == "artifact"
    assert len(eq["sections"]) == 1
    section = eq["sections"][0]
    assert section["plausible"] is False
    assert section["flag"] == "q_out_of_range"
    assert section["reason"] is not None
    assert section["freq_hz"] == 18840.8


def test_build_eq_clean():
    """Sane bell → overall clean, plausible section, nyquist from ctx."""
    eq = dc.build_eq(make_freq_clean(), {"sample_rate": 48000.0})
    assert eq["present"] is True
    assert eq["overall"] == "clean"
    assert eq["sections"][0]["plausible"] is True
    assert eq["sections"][0]["flag"] is None


def test_build_eq_degenerate():
    """status degenerate → present False, overall none, no sections."""
    eq = dc.build_eq(make_freq_degenerate(), {})
    assert eq["present"] is False
    assert eq["overall"] == "none"
    assert eq["sections"] == []


def test_build_eq_missing_row():
    """freq_row None → present False, overall none."""
    eq = dc.build_eq(None, {})
    assert eq["present"] is False
    assert eq["overall"] == "none"
    assert eq["notes"]


# ---------------------------------------------------------------------------
# build_dynamics
# ---------------------------------------------------------------------------


def test_build_dynamics_conflict():
    """pro-c-3: derived vs json threshold conflict; release implausible."""
    dynamics = dc.build_dynamics(make_compression_conflict(),
                                 make_gr_implausible(), {})
    assert dynamics["present"] is True
    compression = dynamics["compression"]
    assert compression["threshold_derived"] == -13.47
    assert compression["ratio_derived"] == 3.27
    assert compression["threshold_json"] == -9.03
    assert compression["ratio_json"] == 3.33
    assert compression["knee_json"] == 3.0
    assert compression["conflict"] is True
    # conflict_note must describe both fits so a reader can judge.
    assert "-13.47" in compression["conflict_note"]
    assert "-9.03" in compression["conflict_note"]
    gr = dynamics["gr"]
    assert gr["attack_ms"] == 3.859
    assert gr["release_ms"] == 39676.69
    assert gr["attack_plausible"] is True
    assert gr["release_plausible"] is False
    assert gr["release_flag"] == "release_implausible"
    assert gr["note"] is not None


def test_build_dynamics_clean():
    """Consistent fits + sane GR → no conflict, everything plausible."""
    dynamics = dc.build_dynamics(make_compression_clean(), make_gr_clean(), {})
    assert dynamics["present"] is True
    assert dynamics["compression"]["conflict"] is False
    assert dynamics["compression"]["conflict_note"] is None
    assert dynamics["compression"]["threshold_json"] == -30.0
    assert dynamics["gr"]["attack_plausible"] is True
    assert dynamics["gr"]["release_plausible"] is True
    assert dynamics["gr"]["release_flag"] is None


def test_build_dynamics_absent():
    """Degenerate compression + invalid GR → present False, no fits."""
    dynamics = dc.build_dynamics(make_compression_degenerate(),
                                 make_gr_invalid(), {})
    assert dynamics["present"] is False
    compression = dynamics["compression"]
    assert compression["threshold_derived"] is None
    assert compression["conflict"] is False
    assert compression["threshold_json"] is None
    assert dynamics["gr"]["release_plausible"] is False
    assert dynamics["notes"]


# ---------------------------------------------------------------------------
# build_nonlinearity
# ---------------------------------------------------------------------------


def test_build_nonlinearity_artifact():
    """pro-c-3 96.6-195.9% THD → verdict artifact with a reason."""
    result = dc.build_nonlinearity(make_harmonic_artifact(), set())
    assert result["verdict"] == "artifact"
    assert result["thd_range_pct"] == [96.58, 195.8767]
    assert result["reason"]


def test_build_nonlinearity_clean():
    """Clean 0.8-2.1% THD → verdict clean, description carries the range."""
    result = dc.build_nonlinearity(make_harmonic_clean(), set())
    assert result["verdict"] == "clean"
    assert result["thd_range_pct"] == [0.8, 2.1]
    assert "THD" in result["description"]
    assert result["reason"] is None


def test_build_nonlinearity_not_measured():
    """No tones → verdict not-measured, no range/description."""
    result = dc.build_nonlinearity(make_harmonic_none(), set())
    assert result["verdict"] == "not-measured"
    assert result["thd_range_pct"] is None
    assert result["description"] is None


def test_build_nonlinearity_shared_fingerprint():
    """Clean THD but harmonic_raw in the shared-fingerprint set → artifact."""
    raw = [{"fundamental_hz": 1000.0, "thd_percent": 1.0,
            "dominant_order": 2, "dominant_mag_db": -40.0}]
    groups = dq.detect_duplicate_fingerprints(
        [{"slug": "a", "harmonic_raw": raw},
         {"slug": "b", "harmonic_raw": raw}])
    assert groups  # the fingerprint is genuinely shared
    row = dict(make_harmonic_clean())
    row["harmonic_raw"] = raw
    result = dc.build_nonlinearity(row, set(groups))
    assert result["verdict"] == "artifact"
    assert "fingerprint" in result["reason"]


def test_build_nonlinearity_artifact_shared_fingerprint():
    """Artifact THD + harmonic_raw in the shared-fingerprint set keeps the
    artifact verdict but ALSO names the shared fingerprint in the reason."""
    raw = [{"fundamental_hz": 100.3, "thd_percent": 195.8,
            "dominant_order": 2, "dominant_mag_db": 61.4}]
    groups = dq.detect_duplicate_fingerprints(
        [{"slug": "a", "harmonic_raw": raw},
         {"slug": "b", "harmonic_raw": raw}])
    assert groups  # the fingerprint is genuinely shared
    row = dict(make_harmonic_artifact())
    row["harmonic_raw"] = raw
    result = dc.build_nonlinearity(row, set(groups))
    assert result["verdict"] == "artifact"
    assert "fingerprint" in result["reason"]


# ---------------------------------------------------------------------------
# infer_order
# ---------------------------------------------------------------------------


def test_infer_order_unknown_with_heuristic():
    """Order stays unknown; decisive compressor snapshot adds a basis line."""
    plugin_type = {"kind": "compressor", "confidence": "high", "basis": []}
    result = dc.infer_order(plugin_type, {"present": True},
                            {"present": True}, make_compressor_snapshot())
    assert result["order"] == "unknown"
    assert result["confidence"] == "low"
    assert result["suggested"] == "eq -> dyn -> eq"
    assert "not measured" in result["suggestion_note"]
    assert any("dyn-only" in item for item in result["basis"])


def test_infer_order_basis_evidence_only():
    """No snapshot → basis is pure evidence, no heuristic line."""
    plugin_type = {"kind": "unknown", "confidence": "low", "basis": []}
    result = dc.infer_order(plugin_type, {"present": False},
                            {"present": False}, None)
    assert result["order"] == "unknown"
    assert any("plugin type" in item for item in result["basis"])
    assert any("eq: absent" in item for item in result["basis"])
    assert not any("dyn-only" in item for item in result["basis"])


# ---------------------------------------------------------------------------
# build_chain_doc
# ---------------------------------------------------------------------------


def test_build_chain_doc_proc3_without_dataset():
    """pro-c-3 row alone, no enrichment → artifact verdicts, not spec-usable."""
    doc = dc.build_chain_doc([make_pro_c3_row()], _META)
    assert doc["generated_at"] == _META["generated_at"]
    assert doc["source"] == {"aggregate_report": "aggregate_report.json",
                             "dataset_dir": None,
                             "report_generated_at": _META["report_generated_at"]}
    plugin = doc["plugins"][0]
    assert plugin["slug"] == "pro-c-3"
    assert plugin["eq"]["overall"] == "artifact"
    assert plugin["dynamics"]["compression"]["conflict"] is True
    assert plugin["dynamics"]["gr"]["release_plausible"] is False
    assert plugin["nonlinearity"]["verdict"] == "artifact"
    assert plugin["processing_order"]["order"] == "unknown"
    assert plugin["usable_as_spec"] is False
    assert plugin["why_not_spec"] == ["eq artifact",
                                      "compression fit conflict",
                                      "release implausible",
                                      "harmonic artifact"]


def test_build_chain_doc_two_plugins_with_dataset(tmp_path):
    """scepter enriched from dataset.json → analyzer; order deterministic."""
    scepter_dir = tmp_path / "scepter"
    scepter_dir.mkdir()
    (scepter_dir / "dataset.json").write_text(json.dumps({
        "context": {"parameter_snapshot": make_analyzer_snapshot(),
                    "sample_rate": 48000},
        "harmonic": {"tones": []},
    }), encoding="utf-8")
    doc = dc.build_chain_doc([make_pro_c3_row(), make_scepter_row()],
                             _META, dataset_dir=str(tmp_path))
    assert [p["slug"] for p in doc["plugins"]] == ["pro-c-3", "scepter"]
    assert doc["source"]["dataset_dir"] == str(tmp_path)
    # pro-c-3 has no dataset.json → missing-file defensive enrichment.
    assert doc["plugins"][0]["plugin_type"]["kind"] == "processor"
    assert doc["plugins"][0]["usable_as_spec"] is False
    assert doc["plugins"][1]["plugin_type"]["kind"] == "analyzer"
    assert doc["plugins"][1]["plugin_type"]["confidence"] == "high"
    # scepter shares the rig harmonic artifact → not spec-usable either.
    assert doc["plugins"][1]["usable_as_spec"] is False
    assert "harmonic artifact" in doc["plugins"][1]["why_not_spec"]


# ---------------------------------------------------------------------------
# main() CLI
# ---------------------------------------------------------------------------


def _write_report(tmp_path, rows):
    report = {"generated_at": _META["generated_at"], "out_dir": "out",
              "tolerances": {}, "counts": {}, "plugins": rows}
    path = tmp_path / "aggregate_report.json"
    path.write_text(json.dumps(report), encoding="utf-8")
    return path


def test_cli_success(tmp_path):
    """CLI against a real report → exit 0, both output files written."""
    report_path = _write_report(tmp_path,
                                [make_pro_c3_row(), make_scepter_row()])
    json_out = tmp_path / "chain_description.json"
    md_out = tmp_path / "chain_description.md"
    proc = subprocess.run(
        [sys.executable, str(_SCRIPT), str(report_path),
         "--json", str(json_out), "--markdown", str(md_out)],
        capture_output=True, text=True)
    assert proc.returncode == 0, proc.stderr
    assert json_out.is_file()
    assert md_out.is_file()
    doc = json.loads(json_out.read_text(encoding="utf-8"))
    assert doc["generated_at"] == _META["generated_at"]
    assert len(doc["plugins"]) == 2
    assert doc["plugins"][0]["slug"] == "pro-c-3"
    markdown = md_out.read_text(encoding="utf-8")
    assert "pro-c-3" in markdown
    assert "Spec-usable: no" in markdown


def test_cli_success_with_dataset_dir(tmp_path):
    """--dataset-dir enriches plugin types end-to-end through the CLI."""
    report_path = _write_report(tmp_path, [make_scepter_row()])
    scepter_dir = tmp_path / "scepter"
    scepter_dir.mkdir()
    (scepter_dir / "dataset.json").write_text(json.dumps({
        "context": {"parameter_snapshot": make_analyzer_snapshot(),
                    "sample_rate": 48000},
        "harmonic": {"tones": []},
    }), encoding="utf-8")
    json_out = tmp_path / "chain_description.json"
    md_out = tmp_path / "chain_description.md"
    proc = subprocess.run(
        [sys.executable, str(_SCRIPT), str(report_path),
         "--dataset-dir", str(tmp_path),
         "--json", str(json_out), "--markdown", str(md_out)],
        capture_output=True, text=True)
    assert proc.returncode == 0, proc.stderr
    doc = json.loads(json_out.read_text(encoding="utf-8"))
    assert doc["plugins"][0]["plugin_type"]["kind"] == "analyzer"


def test_cli_missing_input(tmp_path):
    """Missing report file → error on stderr, exit code 2."""
    proc = subprocess.run(
        [sys.executable, str(_SCRIPT), str(tmp_path / "missing.json")],
        capture_output=True, text=True)
    assert proc.returncode == 2
    assert "error" in proc.stderr.lower()
