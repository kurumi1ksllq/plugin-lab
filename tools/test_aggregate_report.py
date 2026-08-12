"""Tests for aggregate_report.py pure helpers (harmonic summary + validity).

Wave-1 T1-B of ticket #24: pure-function layer only. Fixtures are inline
literals matching the PluginLab export shapes (SPEC.md); this suite must
not depend on any synthetic dataset builder.

Usage:
    python -m pytest tools/test_aggregate_report.py -q
"""
from aggregate_report import (
    harmonic_summary,
    is_compression_unity,
    is_eq_flat,
    is_harmonic_empty,
    load_harmonic,
)

# ---------------------------------------------------------------------------
# load_harmonic
# ---------------------------------------------------------------------------


def test_load_harmonic_dataset_nested():
    """A dataset doc nests the harmonic block under data['harmonic']['tones']."""
    data = {"type": "dataset", "harmonic": {"tones": [
        {"fundamental_hz": 1000.0, "fundamental_db": -3.0, "thd_percent": 1.2,
         "harmonics": [{"order": 2, "freq": 2000.0, "mag_db": -40.0,
                        "percent": 0.8}]},
    ]}}
    tones = load_harmonic(data)
    assert len(tones) == 1
    assert tones[0]["fundamental_hz"] == 1000.0
    assert tones[0]["harmonics"][0]["order"] == 2


def test_load_harmonic_standalone():
    """A standalone layout carries tones directly at the top level."""
    data = {"tones": [
        {"fundamental_hz": 440.0, "fundamental_db": 0.0, "thd_percent": 0.5,
         "harmonics": [{"order": 2, "freq": 880.0, "mag_db": -50.0,
                        "percent": 0.3}]},
    ]}
    tones = load_harmonic(data)
    assert len(tones) == 1
    assert tones[0]["fundamental_hz"] == 440.0
    assert tones[0]["thd_percent"] == 0.5


def test_load_harmonic_body_is_data():
    """A doc whose body itself IS the harmonic block is accepted."""
    data = {"fundamental_hz": 220.0, "fundamental_db": -6.0,
            "thd_percent": 0.3, "tones": [
                {"fundamental_hz": 220.0, "fundamental_db": -6.0,
                 "thd_percent": 0.3, "harmonics": []},
            ]}
    tones = load_harmonic(data)
    assert len(tones) == 1
    assert tones[0]["fundamental_hz"] == 220.0


def test_load_harmonic_empty_doc():
    """An empty doc has no tones."""
    assert load_harmonic({}) == []


def test_load_harmonic_empty_block():
    """A present-but-empty harmonic block yields no tones."""
    assert load_harmonic({"harmonic": {}}) == []


def test_load_harmonic_missing_block():
    """A dataset doc without the harmonic block yields no tones."""
    assert load_harmonic({"type": "dataset"}) == []


# ---------------------------------------------------------------------------
# harmonic_summary
# ---------------------------------------------------------------------------


def test_harmonic_summary_dominant_by_max_mag():
    """The dominant harmonic is the one with max mag_db, not lowest order."""
    tones = [{"fundamental_hz": 1000.0, "fundamental_db": -3.0,
              "thd_percent": 1.2, "harmonics": [
                  {"order": 2, "freq": 2000.0, "mag_db": -40.0, "percent": 0.8},
                  {"order": 3, "freq": 3000.0, "mag_db": -25.0, "percent": 2.0},
                  {"order": 4, "freq": 4000.0, "mag_db": -33.0, "percent": 1.1},
              ]}]
    summary = harmonic_summary(tones)
    assert len(summary) == 1
    assert summary[0]["fundamental_hz"] == 1000.0
    assert summary[0]["thd_percent"] == 1.2
    assert summary[0]["dominant_order"] == 3
    assert summary[0]["dominant_mag_db"] == -25.0


def test_harmonic_summary_empty_harmonics_dominant_none():
    """A tone with no harmonics has no dominant harmonic."""
    tones = [{"fundamental_hz": 440.0, "fundamental_db": 0.0,
              "thd_percent": 0.0, "harmonics": []}]
    summary = harmonic_summary(tones)
    assert summary[0]["fundamental_hz"] == 440.0
    assert summary[0]["dominant_order"] is None
    assert summary[0]["dominant_mag_db"] is None


def test_harmonic_summary_empty_tones():
    """No tones in, no summaries out."""
    assert harmonic_summary([]) == []


# ---------------------------------------------------------------------------
# is_eq_flat
# ---------------------------------------------------------------------------


def test_is_eq_flat_zeros():
    """All-zero magnitudes are degenerate-flat."""
    assert is_eq_flat([0.0, 0.0, 0.0, 0.0]) is True


def test_is_eq_flat_bell_curve():
    """A real EQ bell (a few dB of variation) is not flat."""
    assert is_eq_flat([0.0, 3.0, 6.0, 3.0, 0.0]) is False


def test_is_eq_flat_near_flat():
    """Variation under the 0.1 dB threshold still counts as flat."""
    assert is_eq_flat([0.0, 0.04, -0.04]) is True


def test_is_eq_flat_empty():
    """An empty capture has nothing to measure, so it is flat."""
    assert is_eq_flat([]) is True


# ---------------------------------------------------------------------------
# is_compression_unity
# ---------------------------------------------------------------------------


def test_is_compression_unity_zero_gr():
    """All-zero gain reduction is degenerate-unity."""
    curve = [{"input_db": -60.0, "output_db": -60.0, "gr_db": 0.0},
             {"input_db": -20.0, "output_db": -20.0, "gr_db": 0.0},
             {"input_db": 0.0, "output_db": 0.0, "gr_db": 0.0}]
    assert is_compression_unity(curve) is True


def test_is_compression_unity_real_gain_reduction():
    """Real gain reduction (-3, -6 dB) means the curve is not unity."""
    curve = [{"input_db": -60.0, "output_db": -60.0, "gr_db": 0.0},
             {"input_db": -20.0, "output_db": -23.0, "gr_db": -3.0},
             {"input_db": 0.0, "output_db": -6.0, "gr_db": -6.0}]
    assert is_compression_unity(curve) is False


def test_is_compression_unity_empty():
    """An empty curve has no compression to report, so it is unity."""
    assert is_compression_unity([]) is True


# ---------------------------------------------------------------------------
# is_harmonic_empty
# ---------------------------------------------------------------------------


def test_is_harmonic_empty_true():
    """An empty tones list is empty."""
    assert is_harmonic_empty([]) is True


def test_is_harmonic_empty_false():
    """A tones list with one measurement is not empty."""
    tones = [{"fundamental_hz": 440.0, "fundamental_db": 0.0,
              "thd_percent": 0.0, "harmonics": []}]
    assert is_harmonic_empty(tones) is False
