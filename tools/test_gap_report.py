"""Tests for gap_report.py (issue #106, T8).

gap_report diffs a measured export against the replica prototype's render of
the same chain_doc, per type, and attributes each gap to a stage. The
fixtures use the SPEC.md point shapes (f/mag, input_db/output_db, t/gr_db).
"""

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
