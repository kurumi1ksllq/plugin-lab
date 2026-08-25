"""test_passthrough.py — Tests for aggregate_report.detect_passthrough (T4 #100).

The not-exercised marker: a plugin whose parameter surface is real but whose
processing does NOT engage under headless measurement (Ozone 12 Equalizer
mode). Signature is a coherent all-passthrough measurement:

  - frequency_response: all mags ≈ 0 dB AND all phases ≈ 0 (H1 = wet≈dry)
  - compression: unity curve (output == input)
  - gr_timeline: all GR ≈ 0 (or absent)
  - harmonic: tones PRESENT with a strong fundamental but THD ≈ 0

Contrast with a genuinely quiet plugin (no harmonic fundamental) — that is
data loss, not a passthrough; and with a real flat EQ (freq flat but
harmonic/compression normal) — that is a valid flat response, not passthrough.

Runs with pytest from the repo root:
    python -m pytest tools/test_passthrough.py -q
"""

import json
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))

from aggregate_report import analyze_plugin, detect_passthrough  # noqa: E402


# ---------------------------------------------------------------------------
# Fixture builders
# ---------------------------------------------------------------------------


def passthrough_doc(freq_mag=0.0, freq_phase=0.0, gr_db=0.0, thd=0.0,
                    fundamental_db=75.0, with_gr=True):
    """A dataset doc shaped like Ozone 12 Equalizer's measurement (all flat)."""
    doc = {
        "type": "dataset",
        "context": {"plugin": "Test EQ", "sample_rate": 48000},
        "frequency_response": {
            "raw": [{"f": f, "mag": freq_mag, "phase": freq_phase}
                    for f in (20.5, 200.0, 1000.0, 5000.0, 18858.4)],
            "smoothed_1_12": [],
        },
        "compression": {
            "curve": [{"input_db": d, "output_db": d, "gr_db": 0.0}
                      for d in (-43.01, -23.01, -9.03, 0.0)],
        },
        "harmonic": {
            "tones": [
                {"fundamental_hz": 100.3, "fundamental_db": fundamental_db,
                 "thd_percent": thd, "harmonics": [
                     {"order": 2, "freq": 197.8, "mag_db": -60.81, "percent": 0.0},
                     {"order": 3, "freq": 298.1, "mag_db": -78.17, "percent": 0.0},
                 ]},
                {"fundamental_hz": 200.3, "fundamental_db": fundamental_db,
                 "thd_percent": thd, "harmonics": []},
            ],
        },
    }
    if with_gr:
        doc["gr_timeline"] = {
            "gr": {"timeline": [{"t": 0.0, "gr_db": gr_db},
                                {"t": 0.01, "gr_db": gr_db}],
                   "sample_rate": 48000, "num_points": 2},
            "tau": {"attack_ms": 0.0, "release_ms": 0.0, "valid": False},
        }
    return doc


def real_eq_doc():
    """A REAL EQ: freq has an actual bell, compression unity (EQ has no
    dynamics), harmonic has tones with THD 0 (linear EQ). NOT passthrough —
    the freq curve carries processing information."""
    doc = passthrough_doc()
    doc["frequency_response"]["raw"] = [
        {"f": 100.0, "mag": 0.0, "phase": 0.0},
        {"f": 981.4, "mag": 6.0, "phase": 1.5},
        {"f": 5000.0, "mag": 0.5, "phase": 0.8},
    ]
    return doc


def quiet_doc():
    """A plugin that outputs nothing (host broken): no harmonic fundamental."""
    doc = passthrough_doc(fundamental_db=-90.0)
    return doc


def real_compressor_doc():
    """A REAL compressor: freq may be flat-ish, compression non-unity."""
    doc = passthrough_doc()
    doc["compression"]["curve"] = [
        {"input_db": -40.0, "output_db": -40.0, "gr_db": 0.0},
        {"input_db": -9.03, "output_db": -20.0, "gr_db": -10.97},
    ]
    doc["gr_timeline"]["tau"] = {"attack_ms": 5.0, "release_ms": 0.0,
                                 "valid": True}
    return doc


# ---------------------------------------------------------------------------
# detect_passthrough
# ---------------------------------------------------------------------------


class TestDetectPassthrough:
    def test_full_passthrough_detected(self):
        assert detect_passthrough(passthrough_doc()) is True

    def test_real_eq_not_passthrough(self):
        assert detect_passthrough(real_eq_doc()) is False

    def test_quiet_plugin_not_passthrough(self):
        # No harmonic fundamental = data loss, not a passthrough.
        assert detect_passthrough(quiet_doc()) is False

    def test_real_compressor_not_passthrough(self):
        assert detect_passthrough(real_compressor_doc()) is False

    def test_freq_flat_but_harmonic_thd_high_not_passthrough(self):
        # A saturation plugin with flat freq (no EQ) but real harmonics.
        doc = passthrough_doc()
        doc["harmonic"]["tones"][0]["thd_percent"] = 35.0
        assert detect_passthrough(doc) is False

    def test_missing_blocks_never_crash(self):
        doc = passthrough_doc()
        del doc["frequency_response"]
        assert detect_passthrough(doc) is False  # incomplete data is not a tell


# ---------------------------------------------------------------------------
# analyze_plugin integration
# ---------------------------------------------------------------------------


class TestAnalyzePluginPassthrough:
    def _write(self, tmp_path, doc, slug="test-eq"):
        d = tmp_path / slug
        d.mkdir(exist_ok=True)
        p = d / "dataset.json"
        p.write_text(json.dumps(doc, ensure_ascii=False))
        return str(p)

    def test_passthrough_plugin_status(self, tmp_path):
        path = self._write(tmp_path, passthrough_doc())
        row = analyze_plugin(path)
        assert row["status"] == "not-exercised"

    def test_real_eq_stays_ok(self, tmp_path):
        path = self._write(tmp_path, real_eq_doc(), "real-eq")
        row = analyze_plugin(path)
        assert row["status"] == "ok"

    def test_quiet_plugin_not_exercised(self, tmp_path):
        # No harmonic signal: treated as data loss, not passthrough.
        path = self._write(tmp_path, quiet_doc(), "quiet")
        row = analyze_plugin(path)
        assert row["status"] != "not-exercised"

    def test_real_compressor_stays_ok(self, tmp_path):
        path = self._write(tmp_path, real_compressor_doc(), "comp")
        row = analyze_plugin(path)
        assert row["status"] == "ok"


if __name__ == "__main__":
    sys.exit(pytest.main([__file__, "-v"]))