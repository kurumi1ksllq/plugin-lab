"""test_repro_check.py — Tests for repro_check.py (reproducibility comparison).

Runs with pytest from the repo root:
    python -m pytest tools/test_repro_check.py -q

The repro checker compares two dataset.json docs across the four
measurement types using compare_all's pure functions. Identical docs must
produce zero deltas; docs with a perturbed measurement must fail.
"""

import sys
from pathlib import Path

import pytest

# The tools/ scripts are not a package (no __init__.py); import by path.
sys.path.insert(0, str(Path(__file__).resolve().parent))

import repro_check as rc  # noqa: E402


# ---------------------------------------------------------------------------
# Minimal dataset fixtures (SPEC.md-shaped)
# ---------------------------------------------------------------------------


def dataset_doc(freq_mag=None, comp_output=None, gr_db=None, thd=None):
    """A minimal dataset doc carrying the four measurement blocks."""
    doc = {
        "type": "dataset",
        "context": {"plugin": "Test", "sample_rate": 48000},
        "frequency_response": {
            "raw": [{"f": 100.0, "mag": freq_mag if freq_mag is not None else 0.0,
                     "phase": 0.0},
                    {"f": 1000.0, "mag": freq_mag if freq_mag is not None else 0.0,
                     "phase": 0.0}],
            "smoothed_1_12": [],
        },
        "compression": {
            "curve": [{"input_db": -40.0,
                       "output_db": comp_output if comp_output is not None else -40.0,
                       "gr_db": 0.0},
                      {"input_db": -20.0,
                       "output_db": comp_output if comp_output is not None else -20.0,
                       "gr_db": 0.0}],
        },
        "gr_timeline": {
            "gr": {"timeline": [{"t": 0.0, "gr_db": gr_db if gr_db is not None else 0.0},
                                {"t": 0.01, "gr_db": gr_db if gr_db is not None else 0.0}],
                   "sample_rate": 48000, "num_points": 2},
        },
        "harmonic": {
            "tones": [{"fundamental_hz": 100.0, "fundamental_db": 70.0,
                       "thd_percent": thd if thd is not None else 0.1,
                       "harmonics": []}],
        },
    }
    return doc


class TestCompareDatasets:
    def test_identical_datasets_pass(self, tmp_path):
        a = tmp_path / "a.json"
        b = tmp_path / "b.json"
        a.write_text(__import__("json").dumps(dataset_doc()))
        b.write_text(__import__("json").dumps(dataset_doc()))
        report = rc.compare_datasets(str(a), str(b))
        assert report["ok"] is True
        assert report["freq"]["mean_abs"] == pytest.approx(0.0, abs=1e-9)
        assert report["compression"]["mean_abs"] == pytest.approx(0.0, abs=1e-9)
        assert report["harmonic"]["mean_abs"] == pytest.approx(0.0, abs=1e-9)

    def test_freq_perturbation_fails(self, tmp_path):
        a = tmp_path / "a.json"
        b = tmp_path / "b.json"
        a.write_text(__import__("json").dumps(dataset_doc(freq_mag=0.0)))
        b.write_text(__import__("json").dumps(dataset_doc(freq_mag=6.0)))
        report = rc.compare_datasets(str(a), str(b))
        assert report["ok"] is False
        assert report["freq"]["mean_abs"] > 0.5  # beyond default limit

    def test_compression_perturbation_fails(self, tmp_path):
        a = tmp_path / "a.json"
        b = tmp_path / "b.json"
        a.write_text(__import__("json").dumps(dataset_doc(comp_output=-20.0)))
        b.write_text(__import__("json").dumps(dataset_doc(comp_output=-10.0)))
        report = rc.compare_datasets(str(a), str(b))
        assert report["ok"] is False
        assert report["compression"]["mean_abs"] > 0.5

    def test_harmonic_perturbation_fails(self, tmp_path):
        a = tmp_path / "a.json"
        b = tmp_path / "b.json"
        a.write_text(__import__("json").dumps(dataset_doc(thd=0.1)))
        b.write_text(__import__("json").dumps(dataset_doc(thd=50.0)))
        report = rc.compare_datasets(str(a), str(b))
        assert report["ok"] is False
        assert report["harmonic"]["mean_abs"] > 20.0  # beyond 20% limit

    def test_missing_type_skipped(self, tmp_path):
        a = tmp_path / "a.json"
        b = tmp_path / "b.json"
        doc = dataset_doc()
        doc.pop("gr_timeline")
        a.write_text(__import__("json").dumps(doc))
        b.write_text(__import__("json").dumps(doc))
        report = rc.compare_datasets(str(a), str(b))
        assert report["ok"] is True
        assert report["gr_timeline"] is None

    def test_custom_limits(self, tmp_path):
        a = tmp_path / "a.json"
        b = tmp_path / "b.json"
        a.write_text(__import__("json").dumps(dataset_doc(freq_mag=0.0)))
        b.write_text(__import__("json").dumps(dataset_doc(freq_mag=0.3)))
        # Tight limit: 0.3 dB delta exceeds 0.1 dB -> fail
        report = rc.compare_datasets(str(a), str(b),
                                     limits={"freq": 0.1, "compression": 0.5,
                                             "gr_timeline": 0.5, "harmonic": 20.0})
        assert report["ok"] is False

    def test_verbose_summary_lines(self, tmp_path):
        a = tmp_path / "a.json"
        b = tmp_path / "b.json"
        a.write_text(__import__("json").dumps(dataset_doc()))
        b.write_text(__import__("json").dumps(dataset_doc()))
        report = rc.compare_datasets(str(a), str(b))
        lines = rc.format_summary(report)
        assert any("freq" in line for line in lines)
        assert any("compression" in line for line in lines)


if __name__ == "__main__":
    sys.exit(pytest.main([__file__, "-v"]))