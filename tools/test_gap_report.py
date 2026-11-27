"""Tests for gap_report.py (issue #106, T8).

gap_report diffs a measured export against the replica prototype's render of
the same chain_doc, per type, and attributes each gap to a stage. The
fixtures use the SPEC.md point shapes (f/mag, input_db/output_db, t/gr_db).
"""

import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import gap_report as gr  # noqa: E402


def _freq_doc(peak_db):
    """A frequency_response export with a bump at 1 kHz (SPEC.md §1 shape)."""
    return {
        "type": "frequency_response",
        "raw": [
            {"f": 100.0, "mag": 0.0, "phase": 0.0},
            {"f": 1000.0, "mag": peak_db, "phase": 0.0},
            {"f": 10000.0, "mag": 0.0, "phase": 0.0},
        ],
    }


def test_identical_curves_verdict_match():
    """No gap at all -> verdict match, zero mean_abs."""
    report = gr.build_report({"frequency_response": _freq_doc(6.0)},
                             {"frequency_response": _freq_doc(6.0)})
    freq = report["per_type"]["frequency_response"]
    assert freq["verdict"] == "match"
    assert freq["mean_abs"] < 1e-6


def test_gap_is_measured_and_attributed_to_eq():
    """A 3 dB offset in the EQ region is a gap, attributed to the eq stage."""
    report = gr.build_report({"frequency_response": _freq_doc(6.0)},
                             {"frequency_response": _freq_doc(3.0)})
    freq = report["per_type"]["frequency_response"]
    assert freq["verdict"] == "mismatch"
    # One point is off by 3 dB, the other two match -> mean 1.0, worst 3.0.
    assert freq["mean_abs"] >= 1.0
    assert freq["worst_abs"] == 3.0
    assert report["attribution"]["frequency_response"] == "eq"


def test_missing_type_in_replica_is_reported_not_crashed():
    """A type the replica does not render is 'not-rendered', never a crash."""
    measured = {"compression": {"type": "compression_curve", "curve": [
        {"input_db": -20.0, "output_db": -20.0, "gr_db": 0.0}]}}
    replica = {"frequency_response": {"type": "frequency_response", "raw": []}}
    report = gr.build_report(measured, replica)
    assert report["per_type"]["compression"]["verdict"] == "not-rendered"


def test_markdown_render_contains_verdict_and_attribution():
    """The markdown report states each type's verdict and its stage."""
    report = gr.build_report({"frequency_response": _freq_doc(6.0)},
                             {"frequency_response": _freq_doc(3.0)})
    md = gr.render_markdown(report)
    assert "frequency_response" in md
    assert "mismatch" in md
    assert "eq" in md


def test_report_is_deterministic():
    """Same inputs -> identical report JSON (no timestamps)."""
    a = gr.render_json(gr.build_report({"frequency_response": _freq_doc(6.0)},
                                       {"frequency_response": _freq_doc(3.0)}))
    b = gr.render_json(gr.build_report({"frequency_response": _freq_doc(6.0)},
                                       {"frequency_response": _freq_doc(3.0)}))
    assert a == b


# ---------------------------------------------------------------------------
# CLI (subprocess) — the axis-1 review found the CLI crashed while every
# in-process test passed, so the entry point itself needs a test.
# ---------------------------------------------------------------------------


def _write_json(path, payload):
    path.write_text(json.dumps(payload), encoding="utf-8")
    return path


def _run_cli(args):
    import subprocess
    script = Path(__file__).resolve().parent / "gap_report.py"
    return subprocess.run([sys.executable, str(script)] + [str(a) for a in args],
                          capture_output=True, text=True)


def _chain_doc_for_cli():
    """A minimal v3 chain_doc the replica can render."""
    return {
        "contract_version": "3",
        "generated_at": "2026-11-27T00:00:00+00:00",
        "source": {"aggregate_report": "aggregate_report.json",
                   "dataset_dir": None,
                   "report_generated_at": "2026-11-27T00:00:00+00:00"},
        "plugins": [{
            "slug": "cli-fixture",
            "plugin": "CLI Fixture",
            "plugin_type": {"kind": "eq-only", "confidence": "high",
                            "basis": ["inline fixture"]},
            "eq": {"present": True, "overall": "clean", "notes": [], "sections": [
                {"freq_hz": 1000.0, "gain_db": 6.0, "q": 1.0,
                 "plausible": True, "flag": None, "reason": None}]},
            "dynamics": {"present": False, "notes": [],
                         "compression": {"threshold_derived": None,
                                         "ratio_derived": None,
                                         "knee_json": None, "conflict": False,
                                         "conflict_note": None},
                         "gr": {"attack_ms": None, "release_ms": None,
                                "attack_plausible": False,
                                "release_plausible": False,
                                "release_flag": None, "note": None}},
            "nonlinearity": {"verdict": "not-measured", "thd_range_pct": None,
                             "description": None, "reason": None},
            "processing_order": {"order": "unknown", "confidence": "low",
                                 "basis": [], "suggested": "eq -> dyn -> eq",
                                 "suggestion_note": "canonical, not measured"},
            "usable_as_spec": True,
            "why_not_spec": [],
            "stages": [{
                "kind": "eq",
                "params": {"overall": "eq", "sections": [
                    {"freq_hz": 1000.0, "gain_db": 6.0, "q": 1.0}]},
                "provenance": {"source": "fixture", "basis": ["inline"]},
                "blockers": [], "usable": True,
            }],
        }],
    }


def test_cli_writes_both_reports(tmp_path):
    """The CLI runs end to end and writes the JSON + markdown reports."""
    chain = _write_json(tmp_path / "chain.json", _chain_doc_for_cli())
    measured = _write_json(tmp_path / "measured.json",
                           {"frequency_response": _freq_doc(6.0)})
    json_out = tmp_path / "gap.json"
    md_out = tmp_path / "gap.md"
    proc = _run_cli(["--measured", measured, "--chain-doc", chain,
                     "--json-out", json_out, "--md-out", md_out])
    assert proc.returncode == 0, proc.stderr
    assert json_out.exists() and md_out.exists()
    report = json.loads(json_out.read_text(encoding="utf-8"))
    assert "per_type" in report and "attribution" in report


def test_cli_invalid_chain_doc_exits_2(tmp_path):
    """A chain_doc the replica rejects is a clean exit 2, not a traceback."""
    chain = _write_json(tmp_path / "bad.json", {"contract_version": "3"})
    measured = _write_json(tmp_path / "measured.json", {})
    proc = _run_cli(["--measured", measured, "--chain-doc", chain,
                     "--json-out", tmp_path / "gap.json"])
    assert proc.returncode == 2
    assert proc.stderr.startswith("error: ")
    assert "Traceback" not in proc.stderr


def test_cli_missing_file_exits_nonzero(tmp_path):
    """A missing input fails cleanly (no traceback)."""
    proc = _run_cli(["--measured", tmp_path / "nope.json",
                     "--chain-doc", tmp_path / "nope2.json",
                     "--json-out", tmp_path / "gap.json"])
    assert proc.returncode != 0
    assert "Traceback" not in proc.stderr
