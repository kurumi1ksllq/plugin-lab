"""test_describe_quality.py — Tests for describe_quality.py (pure predicates).

Wave-1 Task T1 of issue #26: measurement-quality predicates that the
processing-chain description generator (describe_chain) will use to flag
implausible measurement data before describing a plugin chain:

  - classify_freq_peak:    sanity of an EQ-style peak {freq_hz, gain_db, q}
  - classify_harmonic:     THD artifact / clean / not-measured verdict
  - tau_sanity:            attack/release time-constant plausibility
  - summarize_by_level:    robust median of per-level tau_sec arrays
  - detect_duplicate_fingerprints: sha1 grouping of identical raw harmonic
    blocks across plugins (same processing chain suspected)

Runs with pytest from the repo root:
    python -m pytest tools/test_describe_quality.py -q

All fixtures are inline literals (real-style values from measured exports) —
no dependency on any other module (test_data/chain_fixtures.py,
describe_render.py, etc.).
"""

import sys
from pathlib import Path

# The tools/ scripts are not a package (no __init__.py); import by path.
sys.path.insert(0, str(Path(__file__).resolve().parent))

import describe_quality as dq  # noqa: E402


# ---------------------------------------------------------------------------
# classify_freq_peak
# ---------------------------------------------------------------------------


def test_freq_peak_q_out_of_range():
    """q=18098.4 (a real-style degenerate value) → implausible, q flag."""
    row = {"freq_hz": 120.0, "gain_db": 3.0, "q": 18098.4}
    result = dq.classify_freq_peak(row)
    assert result["plausible"] is False
    assert result["flag"] == "q_out_of_range"
    assert result["reason"] is not None


def test_freq_peak_q_in_range():
    """q=1.5 → plausible."""
    row = {"freq_hz": 1000.0, "gain_db": 6.0, "q": 1.5}
    result = dq.classify_freq_peak(row)
    assert result["plausible"] is True
    assert result["flag"] is None


def test_freq_peak_q_none():
    """q=None → nothing to judge → plausible."""
    row = {"freq_hz": 1000.0, "gain_db": 0.0, "q": None}
    result = dq.classify_freq_peak(row)
    assert result["plausible"] is True
    assert result["flag"] is None


def test_freq_peak_gain_out_of_range():
    """gain_db=40.0 → implausible, gain flag."""
    row = {"freq_hz": 1000.0, "gain_db": 40.0, "q": 1.0}
    result = dq.classify_freq_peak(row)
    assert result["plausible"] is False
    assert result["flag"] == "gain_out_of_range"


def test_freq_peak_freq_out_of_range_with_nyquist():
    """freq_hz=5.0 below 20 Hz bound → implausible when nyquist given."""
    row = {"freq_hz": 5.0, "gain_db": 0.0, "q": 1.0}
    result = dq.classify_freq_peak(row, nyquist=1000.0)
    assert result["plausible"] is False
    assert result["flag"] == "freq_out_of_range"


def test_freq_peak_nyquist_none_skips_freq_bound():
    """nyquist=None → freq bound skipped → freq 5.0 still plausible."""
    row = {"freq_hz": 5.0, "gain_db": 0.0, "q": 1.0}
    result = dq.classify_freq_peak(row, nyquist=None)
    assert result["plausible"] is True
    assert result["flag"] is None


def test_freq_peak_all_none_plausible():
    """All-None row → plausible True, no flag/reason."""
    result = dq.classify_freq_peak({"freq_hz": None, "gain_db": None, "q": None})
    assert result["plausible"] is True
    assert result["flag"] is None
    assert result["reason"] is None


# ---------------------------------------------------------------------------
# classify_harmonic
# ---------------------------------------------------------------------------


def test_harmonic_thd_impossible_artifact():
    """thd 195.8767 (>=100, impossible physics) → artifact."""
    row = {
        "tones_count": 1,
        "summary": [
            {"fundamental_hz": 100.0, "thd_percent": 195.8767,
             "dominant_order": 3, "dominant_mag_db": -2.0},
        ],
        "status": "ok",
    }
    result = dq.classify_harmonic(row)
    assert result["verdict"] == "artifact"
    assert result["thd_range_pct"] == [195.8767, 195.8767]


def test_harmonic_thd_clean():
    """thd 3.2 (well below 20) → clean."""
    row = {
        "tones_count": 1,
        "summary": [
            {"fundamental_hz": 100.0, "thd_percent": 3.2,
             "dominant_order": 1, "dominant_mag_db": -25.0},
        ],
        "status": "ok",
    }
    result = dq.classify_harmonic(row)
    assert result["verdict"] == "clean"
    assert result["thd_range_pct"] == [3.2, 3.2]
    assert result["reasons"] == []


def test_harmonic_clean_thd_range_two_tones():
    """Two clean tones → thd_range_pct = [min, max]."""
    row = {
        "tones_count": 2,
        "summary": [
            {"fundamental_hz": 100.0, "thd_percent": 1.0,
             "dominant_order": 2, "dominant_mag_db": -30.0},
            {"fundamental_hz": 500.0, "thd_percent": 3.2,
             "dominant_order": 1, "dominant_mag_db": -28.0},
        ],
        "status": "ok",
    }
    result = dq.classify_harmonic(row)
    assert result["verdict"] == "clean"
    assert result["thd_range_pct"] == [1.0, 3.2]


def test_harmonic_not_measured_no_tones():
    """tones_count=0 → not-measured, no thd range."""
    row = {"tones_count": 0, "summary": [], "status": "ok"}
    result = dq.classify_harmonic(row)
    assert result["verdict"] == "not-measured"
    assert result["thd_range_pct"] is None


def test_harmonic_not_measured_no_data_status():
    """status='no-data' → not-measured even with tone entries."""
    row = {
        "tones_count": 3,
        "summary": [{"fundamental_hz": 100.0, "thd_percent": 1.0,
                     "dominant_order": 1, "dominant_mag_db": -30.0}],
        "status": "no-data",
    }
    result = dq.classify_harmonic(row)
    assert result["verdict"] == "not-measured"


def test_harmonic_artifact_even_order_reason():
    """thd 96.6 → artifact; even dominant_order=2 adds belt-and-braces reason."""
    row = {
        "tones_count": 1,
        "summary": [
            {"fundamental_hz": 100.0, "thd_percent": 96.6,
             "dominant_order": 2, "dominant_mag_db": -3.0},
        ],
        "status": "ok",
    }
    result = dq.classify_harmonic(row)
    assert result["verdict"] == "artifact"
    assert result["thd_range_pct"] == [96.6, 96.6]
    assert "even-order dominant" in result["reasons"]


# ---------------------------------------------------------------------------
# tau_sanity
# ---------------------------------------------------------------------------


def test_tau_release_implausible_mismatch():
    """release 39676.7ms with attack 3.86ms (10000x) → release implausible."""
    row = {"attack_ms": 3.86, "release_ms": 39676.7, "valid": True,
           "status": "ok"}
    result = dq.tau_sanity(row)
    assert result["release_plausible"] is False
    assert result["flag"] == "release_implausible"
    assert result["note"] is not None


def test_tau_attack_plausible():
    """attack 3.86ms within bounds → attack plausible."""
    row = {"attack_ms": 3.86, "release_ms": 50.0, "valid": True,
           "status": "ok"}
    result = dq.tau_sanity(row)
    assert result["attack_plausible"] is True
    assert result["release_plausible"] is True


def test_tau_invalid_values_not_plausible_with_note():
    """attack 0.0 (below TAU_MIN) + release None → not plausible + note."""
    row = {"attack_ms": 0.0, "release_ms": None, "valid": True,
           "status": "ok"}
    result = dq.tau_sanity(row)
    assert result["attack_plausible"] is False
    assert result["release_plausible"] is False
    assert result["note"] is not None


def test_tau_valid_false_not_plausible():
    """valid=False → both not plausible with note."""
    row = {"attack_ms": 3.86, "release_ms": 50.0, "valid": False,
           "status": "ok"}
    result = dq.tau_sanity(row)
    assert result["attack_plausible"] is False
    assert result["release_plausible"] is False
    assert result["note"] is not None


def test_tau_missing_keys_defensive():
    """Empty row → no raise; not plausible with note."""
    result = dq.tau_sanity({})
    assert result["attack_plausible"] is False
    assert result["release_plausible"] is False
    assert result["note"] is not None


# ---------------------------------------------------------------------------
# summarize_by_level
# ---------------------------------------------------------------------------


def test_summarize_by_level_real_style():
    """Real-style tau_sec array → robust median, 3 outliers (0.0+28.9+289.7)."""
    by_level = [
        {"tau_sec": 0.0},
        {"tau_sec": 0.00075},
        {"tau_sec": 0.0011},
        {"tau_sec": 28.9},
        {"tau_sec": 289.7},
    ]
    result = dq.summarize_by_level(by_level, "release")
    assert 0.0 < result["median_ms"] < 5.0
    assert result["n_outliers"] == 3
    assert len(result["ignored_values"]) == 3
    assert result["ignored_values"] == [0.0, 28.9, 289.7]


def test_summarize_by_level_empty():
    """Empty by_level → median_ms None, no outliers."""
    result = dq.summarize_by_level([], "attack")
    assert result["median_ms"] is None
    assert result["n_outliers"] == 0
    assert result["ignored_values"] == []


# ---------------------------------------------------------------------------
# detect_duplicate_fingerprints
# ---------------------------------------------------------------------------


def test_detect_duplicate_fingerprints():
    """2 identical raw blocks → grouped; different + None rows skipped."""
    rows = [
        {"slug": "a", "harmonic_raw": [{"f": 100.0, "thd_percent": 1.0}]},
        {"slug": "b", "harmonic_raw": [{"f": 100.0, "thd_percent": 1.0}]},
        {"slug": "c", "harmonic_raw": [{"f": 200.0, "thd_percent": 2.0}]},
        {"slug": "d", "harmonic_raw": None},
    ]
    result = dq.detect_duplicate_fingerprints(rows)
    assert len(result) == 1
    slugs = next(iter(result.values()))
    assert sorted(slugs) == ["a", "b"]
