"""test_compare_all.py — Tests for compare_all.py (pure comparison functions).

Runs with pytest from the repo root:
    python -m pytest tools/test_compare_all.py -q

Fixture builders are inline (no import of other ticket files): each builds
a minimal SPEC.md-shaped export doc dict with KNOWN parameters, so the
tests can assert that identical known params produce ~zero deltas and
perturbed params produce non-zero deltas.
"""

import json
import sys
from pathlib import Path

import pytest

# The tools/ scripts are not a package (no __init__.py); import by path.
sys.path.insert(0, str(Path(__file__).resolve().parent))

import compare_all as ca  # noqa: E402


# ---------------------------------------------------------------------------
# Inline fixture builders (SPEC.md-shaped doc dicts with known params)
# ---------------------------------------------------------------------------


def freq_doc(gain_db=0.0, f=None):
    """frequency_response doc: flat response at gain_db over the given freqs."""
    if f is None:
        f = [100.0, 200.0, 500.0, 1000.0, 2000.0, 5000.0, 10000.0, 20000.0]
    raw = [{"f": ff, "mag": gain_db, "phase": 0.0} for ff in f]
    return {"type": "frequency_response", "raw": raw,
            "smoothed_1_12": [dict(p) for p in raw]}


def compression_doc(threshold_db=-30.0, ratio=4.0, knee_db=3.0,
                    input_db=None, with_fitted=True):
    """compression_curve doc: unity below threshold, slope 1/ratio above."""
    if input_db is None:
        input_db = [-60.0, -50.0, -40.0, -35.0, -30.0, -25.0, -20.0,
                    -15.0, -10.0, -5.0, 0.0]
    curve = []
    for i in input_db:
        out = i if i <= threshold_db else threshold_db + (i - threshold_db) / ratio
        curve.append({"input_db": i, "output_db": out, "gr_db": out - i})
    doc = {"type": "compression_curve", "curve": curve}
    if with_fitted:
        doc["fitted"] = {"ratio": ratio, "threshold_db": threshold_db,
                         "knee_db": knee_db}
    return doc


def gr_doc(attack_sec=0.001, release_sec=0.05, valid=True, gr_db=-10.0):
    """gr_timeline doc: constant GR step, tau block with known attack/release."""
    t = [0.0, 0.01, 0.02, 0.03, 0.05, 0.08, 0.10, 0.15, 0.20]
    return {
        "type": "gr_timeline",
        "gr": {"timeline": [{"t": tt, "gr_db": gr_db} for tt in t]},
        "tau": {"attack_sec": attack_sec, "release_sec": release_sec,
                "valid": valid},
    }


def harmonic_doc(tones=None):
    """harmonic_analysis doc: tones as (fundamental_hz, thd_percent, mags[])."""
    if tones is None:
        tones = [(1000.0, 0.5, [-40.0, -50.0]), (2000.0, 0.3, [-42.0, -52.0])]
    return {
        "type": "harmonic_analysis",
        "tones": [
            {
                "fundamental_hz": f0,
                "fundamental_db": -6.0,
                "thd_percent": thd,
                "harmonics": [
                    {"order": order, "freq": f0 * order, "mag_db": mag_db,
                     "percent": 0.0}
                    for order, mag_db in enumerate(mags, start=2)
                ],
            }
            for f0, thd, mags in tones
        ],
    }


# ---------------------------------------------------------------------------
# Interpolation
# ---------------------------------------------------------------------------


def test_interp_linear_mid_segment():
    pts = [{"x": 0.0, "y": 0.0}, {"x": 2.0, "y": 10.0}, {"x": 4.0, "y": 20.0}]
    assert ca.interp_linear(pts, 1.0, "x", "y") == 5.0


def test_interp_linear_exact_point():
    pts = [{"x": 0.0, "y": 0.0}, {"x": 2.0, "y": 10.0}, {"x": 4.0, "y": 20.0}]
    assert ca.interp_linear(pts, 2.0, "x", "y") == 10.0


def test_interp_linear_clamps_edges():
    pts = [{"x": 0.0, "y": 0.0}, {"x": 2.0, "y": 10.0}, {"x": 4.0, "y": 20.0}]
    assert ca.interp_linear(pts, -1.0, "x", "y") == 0.0   # below first
    assert ca.interp_linear(pts, 99.0, "x", "y") == 20.0  # above last


def test_interp_linear_empty_raises():
    with pytest.raises(ValueError):
        ca.interp_linear([], 1.0, "x", "y")


# ---------------------------------------------------------------------------
# frequency_response
# ---------------------------------------------------------------------------


def test_freq_identical_docs_pass():
    res = ca.compare_freq(freq_doc(), freq_doc())
    assert res["mean_abs"] == pytest.approx(0.0, abs=1e-12)
    assert res["mean"] == pytest.approx(0.0, abs=1e-12)
    ok, text = ca.verdict(res["mean_abs"], 0.5)
    assert ok and text.startswith("PASS")


def test_freq_gain_perturbation_fails():
    res = ca.compare_freq(freq_doc(gain_db=0.0), freq_doc(gain_db=3.0))
    assert res["mean_abs"] > 0.5
    ok, text = ca.verdict(res["mean_abs"], 0.5)
    assert not ok and text.startswith("FAIL")


def test_freq_result_fields_and_log_grid():
    res = ca.compare_freq(freq_doc(), freq_doc())
    assert set(res) == {"points", "mean_abs", "mean", "worst"}
    assert len(res["points"]) == 200
    assert res["points"][0]["x"] == pytest.approx(100.0)
    assert res["points"][-1]["x"] == pytest.approx(10000.0)
    assert set(res["worst"]) == {"x", "delta"}


def test_freq_ignores_out_of_band_differences():
    # +6 dB only below 100 Hz (outside the compare band) must not shift the
    # grid values: edge clamping keeps the in-band comparison clean.
    a = freq_doc(gain_db=0.0)
    b = freq_doc(gain_db=0.0, f=[20.0, 40.0, 100.0, 200.0, 500.0, 1000.0,
                                 2000.0, 5000.0, 10000.0, 20000.0])
    b["raw"] = [dict(p, mag=6.0) if p["f"] < 100.0 else p for p in b["raw"]]
    res = ca.compare_freq(a, b)
    assert res["mean_abs"] == pytest.approx(0.0, abs=1e-9)


# ---------------------------------------------------------------------------
# compression
# ---------------------------------------------------------------------------


def test_compression_identical_docs_pass():
    res = ca.compare_compression(compression_doc(), compression_doc())
    assert res["mean_abs"] == pytest.approx(0.0, abs=1e-12)
    assert res["params"]["threshold_delta_db"] == pytest.approx(0.0, abs=1e-12)
    assert res["params"]["ratio_delta_pct"] == pytest.approx(0.0, abs=1e-12)
    ok, text = ca.verdict(res["mean_abs"], 0.5)
    assert ok and text.startswith("PASS")


def test_compression_threshold_perturbation_fails():
    res = ca.compare_compression(compression_doc(threshold_db=-30.0),
                                 compression_doc(threshold_db=-40.0))
    assert res["mean_abs"] > 0.5
    assert res["params"]["threshold_delta_db"] == pytest.approx(10.0, abs=1e-9)
    ok, text = ca.verdict(res["mean_abs"], 0.5)
    assert not ok and text.startswith("FAIL")


def test_compression_ratio_perturbation_params():
    res = ca.compare_compression(compression_doc(ratio=4.0),
                                 compression_doc(ratio=8.0))
    assert res["params"]["ratio_delta_pct"] == pytest.approx(100.0, abs=1e-9)
    assert res["params"]["threshold_delta_db"] == pytest.approx(0.0, abs=1e-12)


def test_compression_missing_fitted_params_none():
    res = ca.compare_compression(compression_doc(), compression_doc(with_fitted=False))
    assert res["params"] is None


def test_compression_result_fields():
    res = ca.compare_compression(compression_doc(), compression_doc())
    assert set(res) == {"points", "mean_abs", "mean", "worst", "params"}
    assert set(res["params"]) == {"threshold_delta_db", "ratio_delta_pct"}


# ---------------------------------------------------------------------------
# gr_timeline
# ---------------------------------------------------------------------------


def test_gr_identical_docs_pass():
    res = ca.compare_gr(gr_doc(), gr_doc())
    assert res["mean_abs"] == pytest.approx(0.0, abs=1e-12)
    assert res["params"]["attack_delta_ms"] == pytest.approx(0.0, abs=1e-12)
    assert res["params"]["release_delta_ms"] == pytest.approx(0.0, abs=1e-12)
    ok, text = ca.verdict(res["mean_abs"], 0.5)
    assert ok and text.startswith("PASS")


def test_gr_attack_perturbation_params_nonzero():
    res = ca.compare_gr(gr_doc(attack_sec=0.001), gr_doc(attack_sec=0.002))
    assert res["params"]["attack_delta_ms"] == pytest.approx(1.0, abs=1e-9)
    # curve is unchanged by the tau perturbation: params carry the delta
    assert res["mean_abs"] == pytest.approx(0.0, abs=1e-12)


def test_gr_invalid_tau_skips_params():
    res = ca.compare_gr(gr_doc(), gr_doc(valid=False))
    assert res["params"] is None          # SKIP, not FAIL
    assert res["mean_abs"] == pytest.approx(0.0, abs=1e-12)  # curve still compared


def test_gr_result_fields():
    res = ca.compare_gr(gr_doc(), gr_doc())
    assert set(res) == {"points", "mean_abs", "mean", "worst", "params"}
    assert set(res["params"]) == {"attack_delta_ms", "release_delta_ms"}


# ---------------------------------------------------------------------------
# harmonic
# ---------------------------------------------------------------------------


def test_harmonic_identical_docs_pass():
    res = ca.compare_harmonic(harmonic_doc(), harmonic_doc())
    assert res["mean_abs"] == pytest.approx(0.0, abs=1e-12)
    assert res["unmatched_tones"] == []
    ok, text = ca.verdict(res["mean_abs"], 0.1)
    assert ok and text.startswith("PASS")


def test_harmonic_thd_perturbation_fails():
    b = harmonic_doc(tones=[(1000.0, 1.0, [-40.0, -50.0]), (2000.0, 0.3, [-42.0, -52.0])])
    res = ca.compare_harmonic(harmonic_doc(), b)
    assert res["mean_abs"] > 0.1
    assert res["worst"]["x"] == pytest.approx(1000.0)   # the perturbed tone
    assert res["worst"]["delta"] == pytest.approx(0.5, abs=1e-9)
    ok, text = ca.verdict(res["mean_abs"], 0.1)
    assert not ok and text.startswith("FAIL")


def test_harmonic_matches_nearby_fundamental():
    # 1000 -> 1050 Hz is a 5 % shift: within tolerance, still matched.
    b = harmonic_doc(tones=[(1050.0, 0.5, [-40.0, -50.0]), (2000.0, 0.3, [-42.0, -52.0])])
    res = ca.compare_harmonic(harmonic_doc(), b)
    assert res["unmatched_tones"] == []
    assert res["mean_abs"] == pytest.approx(0.0, abs=1e-12)


def test_harmonic_unmatched_extra_tone_warns():
    a = harmonic_doc()
    b = harmonic_doc()
    b["tones"].append({"fundamental_hz": 3000.0, "fundamental_db": -6.0,
                       "thd_percent": 0.2, "harmonics": []})
    res = ca.compare_harmonic(a, b)
    assert res["unmatched_tones"] == [3000.0]   # warning, not failure


def test_harmonic_unmatched_far_shift():
    # 1000 -> 1200 Hz is a 20 % shift: beyond tolerance, both sides unmatched.
    a = harmonic_doc()
    b = harmonic_doc(tones=[(1200.0, 0.5, [-40.0, -50.0]), (2000.0, 0.3, [-42.0, -52.0])])
    res = ca.compare_harmonic(a, b)
    assert res["unmatched_tones"] == [1000.0, 1200.0]


def test_harmonic_no_matches_no_crash():
    a = harmonic_doc()
    b = harmonic_doc(tones=[(2500.0, 0.5, [-40.0, -50.0]), (3500.0, 0.3, [-42.0, -52.0])])
    res = ca.compare_harmonic(a, b)
    assert res["mean_abs"] == 0.0
    assert res["unmatched_tones"] == [1000.0, 2000.0, 2500.0, 3500.0]


def test_harmonic_result_fields():
    res = ca.compare_harmonic(harmonic_doc(), harmonic_doc())
    assert set(res) == {"points", "mean_abs", "mean", "worst", "harmonics",
                        "unmatched_tones"}
    # both tones matched; per-order mag_db deltas aggregated for orders 2, 3
    assert set(res["harmonics"]) == {2, 3}


# ---------------------------------------------------------------------------
# load_points (standalone + dataset layouts)
# ---------------------------------------------------------------------------


def test_load_points_standalone(tmp_path):
    p = tmp_path / "freq.json"
    p.write_text(json.dumps(freq_doc()), encoding="utf-8")
    points, doc = ca.load_points(str(p), "freq")
    assert points == doc["raw"]


def test_load_points_standalone_all_types(tmp_path):
    builders = [
        ("freq", freq_doc(), "raw"),
        ("compression", compression_doc(), "curve"),
        ("gr_timeline", gr_doc(), ("gr", "timeline")),
        ("harmonic", harmonic_doc(), "tones"),
    ]
    for mtype, doc, key in builders:
        p = tmp_path / f"{mtype}.json"
        p.write_text(json.dumps(doc), encoding="utf-8")
        points, _ = ca.load_points(str(p), mtype)
        if isinstance(key, tuple):
            expected = doc[key[0]][key[1]]
        else:
            expected = doc[key]
        assert points == expected, mtype


def test_load_points_dataset_nested(tmp_path):
    dataset = {
        "type": "dataset",
        "frequency_response": freq_doc(),
        "compression": compression_doc(),
        "gr_timeline": gr_doc(),
        "harmonic": harmonic_doc(),
    }
    p = tmp_path / "dataset.json"
    p.write_text(json.dumps(dataset), encoding="utf-8")
    points, _ = ca.load_points(str(p), "freq")
    assert points == dataset["frequency_response"]["raw"]
    points, _ = ca.load_points(str(p), "compression")
    assert points == dataset["compression"]["curve"]
    points, _ = ca.load_points(str(p), "gr_timeline")
    assert points == dataset["gr_timeline"]["gr"]["timeline"]
    points, _ = ca.load_points(str(p), "harmonic")
    assert points == dataset["harmonic"]["tones"]


def test_load_points_smoothed_fallback(tmp_path):
    doc = freq_doc()
    doc["raw"] = []
    p = tmp_path / "smooth.json"
    p.write_text(json.dumps(doc), encoding="utf-8")
    points, _ = ca.load_points(str(p), "freq")
    assert points == doc["smoothed_1_12"]


def test_load_points_missing_raises_valueerror(tmp_path):
    p = tmp_path / "broken.json"
    p.write_text(json.dumps({"type": "frequency_response",
                             "smoothed_1_12": []}), encoding="utf-8")
    with pytest.raises(ValueError, match="broken.json"):
        ca.load_points(str(p), "freq")


# ---------------------------------------------------------------------------
# verdict
# ---------------------------------------------------------------------------


def test_verdict_strict_less_than():
    ok, text = ca.verdict(0.4, 0.5)
    assert ok and text.startswith("PASS")
    ok, text = ca.verdict(0.5, 0.5)
    assert not ok and text.startswith("FAIL")   # strict < : equal is FAIL
    ok, text = ca.verdict(0.6, 0.5)
    assert not ok and text.startswith("FAIL")
