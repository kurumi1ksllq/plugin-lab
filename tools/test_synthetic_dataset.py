"""
test_synthetic_dataset.py — Round-trip tests for the synthetic dataset builder.

Builds `dataset.json`-shaped documents with KNOWN plugin parameters
(tools/test_data/synthetic_dataset.py) and feeds them back through the EXISTING
reverse-derivation pipeline (tools/reverse_derive.py: parse_document +
derive_freq / derive_compression / derive_gr_tau), asserting the known
parameters are recovered within tolerances:

    freq        5%  (relative)
    gain        0.5 dB (absolute)
    Q           20% (relative)
    threshold   1.0 dB (absolute)
    ratio       20% (relative)
    attack/     EXACT (tau is a passthrough through reverse_derive)
    release

The measured deviations printed by this suite are the calibration seed for
the aggregate-report tolerances (ticket #24, batch reverse-derive
aggregation) — they are printed, not just asserted.

Usage:
    python -m pytest tools/test_synthetic_dataset.py -q
    python -m pytest tools/test_synthetic_dataset.py -q -s   # show deviations

Exit code 0 = all round-trips within tolerance, 1 = failure.
"""
import math
import sys
from pathlib import Path

import pytest

# No packages, no __init__.py (repo rule) — put the directories on sys.path
# explicitly so both modules resolve regardless of the invoking working dir.
TOOLS_DIR = Path(__file__).resolve().parent
TEST_DATA_DIR = TOOLS_DIR / "test_data"
for _d in (str(TOOLS_DIR), str(TEST_DATA_DIR)):
    if _d not in sys.path:
        sys.path.insert(0, _d)

import reverse_derive      # noqa: E402  (existing module, imported read-only)
import synthetic_dataset   # noqa: E402  (module under test)

# ---------------------------------------------------------------------------
# Round-trip tolerance table (calibration seed for aggregate_report ticket)
# ---------------------------------------------------------------------------

# Known-parameter recovery tolerances, per ticket #24 T1-A.
TOL_FREQ_PCT = 5.0       # recovered peak frequency, % of expected
TOL_GAIN_DB = 0.5        # recovered peak gain, dB absolute
TOL_Q_PCT = 20.0         # recovered Q, % of expected
TOL_THRESHOLD_DB = 1.0   # recovered threshold, dB absolute
TOL_RATIO_PCT = 20.0     # recovered ratio, % of expected
# attack/release: no tolerance — tau is a passthrough, deviation must be 0.0

# Parameter combos: peak 1000Hz/6dB/Q1, peak 2200Hz/-3dB/Q2.5, thr -30/r4,
# thr -20/r8, attack 1ms/rel 50ms, attack 10ms/rel 200ms, plus extra coverage.
COMBOS = [
    dict(peak_hz=1000.0, gain_db=6.0, q=1.0, threshold_db=-30.0, ratio=4.0,
         attack_sec=0.001, release_sec=0.050),
    dict(peak_hz=2200.0, gain_db=-3.0, q=2.5, threshold_db=-20.0, ratio=8.0,
         attack_sec=0.010, release_sec=0.200),
    dict(peak_hz=500.0, gain_db=12.0, q=0.7, threshold_db=-40.0, ratio=2.0,
         attack_sec=0.003, release_sec=0.080),
    dict(peak_hz=3000.0, gain_db=2.0, q=3.0, threshold_db=-15.0, ratio=10.0,
         attack_sec=0.002, release_sec=0.030),
    dict(peak_hz=800.0, gain_db=9.0, q=1.5, threshold_db=-25.0, ratio=6.0,
         attack_sec=0.007, release_sec=0.150),
    dict(peak_hz=1500.0, gain_db=4.0, q=2.0, threshold_db=-35.0, ratio=3.0,
         attack_sec=0.015, release_sec=0.250),
]

COMBO_IDS = [
    "f1000_g+6_q1_thr-30_r4_a1ms_r50ms",
    "f2200_g-3_q2.5_thr-20_r8_a10ms_r200ms",
    "f500_g+12_q0.7_thr-40_r2_a3ms_r80ms",
    "f3000_g+2_q3_thr-15_r10_a2ms_r30ms",
    "f800_g+9_q1.5_thr-25_r6_a7ms_r150ms",
    "f1500_g+4_q2_thr-35_r3_a15ms_r250ms",
]

# ---------------------------------------------------------------------------
# Round-trip helper: build -> derive -> print deviations -> return them
# ---------------------------------------------------------------------------


def _roundtrip(params):
    """Build one synthetic dataset, derive all three sections, print and
    return the measured deviations from the known parameters."""
    data = synthetic_dataset.make_dataset("roundtrip", "Synthetic", **params)

    sections = reverse_derive.parse_document(data)
    assert sections["freq"], "parse_document produced no freq section"
    assert sections["compression"], "parse_document produced no compression section"
    assert sections["gr"], "parse_document produced no gr section"

    # --- frequency response ---
    d = reverse_derive.derive_freq(sections["freq"][0]["result"]["raw"])
    dev_freq_pct = abs(d["freq_hz"] - params["peak_hz"]) / params["peak_hz"] * 100.0
    dev_gain_db = abs(d["gain_db"] - params["gain_db"])
    dev_q_pct = abs(d["q"] - params["q"]) / params["q"] * 100.0

    # --- compression curve ---
    c = reverse_derive.derive_compression(sections["compression"][0]["curve"])
    dev_thr_db = abs(c["threshold_db"] - params["threshold_db"])
    dev_ratio_pct = abs(c["ratio"] - params["ratio"]) / params["ratio"] * 100.0

    # --- GR timeline (tau passthrough) ---
    t = reverse_derive.derive_gr_tau(sections["gr"][0]["tau"])
    exp_attack_ms = params["attack_sec"] * 1000.0
    exp_release_ms = params["release_sec"] * 1000.0
    dev_attack_ms = abs(t["attack_ms"] - exp_attack_ms)
    dev_release_ms = abs(t["release_ms"] - exp_release_ms)

    print(f"combo {params['peak_hz']:g}Hz/{params['gain_db']:g}dB/Q{params['q']:g} "
          f"thr {params['threshold_db']:g}/r{params['ratio']:g} "
          f"atk {exp_attack_ms:g}ms/rel {exp_release_ms:g}ms")
    print(f"  freq {d['freq_hz']:.4f} (dev {dev_freq_pct:.4f}%)   "
          f"gain {d['gain_db']:.4f} dB (dev {dev_gain_db:.4f} dB)   "
          f"q {d['q']:.4f} (dev {dev_q_pct:.4f}%)")
    print(f"  thr {c['threshold_db']:.4f} dB (dev {dev_thr_db:.4f} dB)   "
          f"ratio {c['ratio']:.4f} (dev {dev_ratio_pct:.4f}%)   "
          f"attack {t['attack_ms']:.6f} ms (dev {dev_attack_ms:.3e})   "
          f"release {t['release_ms']:.6f} ms (dev {dev_release_ms:.3e})")

    return {"freq_pct": dev_freq_pct, "gain_db": dev_gain_db, "q_pct": dev_q_pct,
            "threshold_db": dev_thr_db, "ratio_pct": dev_ratio_pct,
            "attack_ms": dev_attack_ms, "release_ms": dev_release_ms}


# ---------------------------------------------------------------------------
# Round-trip recovery (the calibration seed for aggregate tolerances)
# ---------------------------------------------------------------------------


@pytest.mark.parametrize("params", COMBOS, ids=COMBO_IDS)
def test_roundtrip_recovers_known_parameters(params):
    """Synthetic dataset -> reverse_derive must recover every known parameter."""
    dev = _roundtrip(params)
    assert dev["freq_pct"] <= TOL_FREQ_PCT
    assert dev["gain_db"] <= TOL_GAIN_DB
    assert dev["q_pct"] <= TOL_Q_PCT
    assert dev["threshold_db"] <= TOL_THRESHOLD_DB
    assert dev["ratio_pct"] <= TOL_RATIO_PCT
    assert dev["attack_ms"] == 0.0, "attack tau is a passthrough — must be exact"
    assert dev["release_ms"] == 0.0, "release tau is a passthrough — must be exact"


def test_make_dataset_parses_into_nonempty_sections():
    """make_dataset output must parse via parse_document into all three
    non-empty sections (freq / compression / gr)."""
    data = synthetic_dataset.make_dataset(
        "parse-check", "Synthetic", peak_hz=1000.0, gain_db=6.0, q=1.0,
        threshold_db=-30.0, ratio=4.0, attack_sec=0.001, release_sec=0.050)
    assert data["type"] == "dataset"
    assert data["context"]["class_id"] == "synthetic"
    assert data["context"]["plugin"] == "Synthetic"
    assert data["frequency_response"]["raw"]
    assert data["compression"]["curve"]
    assert data["gr_timeline"]["tau"]
    assert data["harmonic"]["tones"] == []
    sections = reverse_derive.parse_document(data)
    assert sections["freq"]
    assert sections["compression"]
    assert sections["gr"]


# ---------------------------------------------------------------------------
# Builder structural properties
# ---------------------------------------------------------------------------


def test_make_freq_curve_structure_and_determinism():
    """Log-spaced 10 Hz..20 kHz bell, phase 0, peak exactly at (f0, gain)."""
    a = synthetic_dataset.make_freq_curve(1000.0, 6.0, 1.0)
    b = synthetic_dataset.make_freq_curve(1000.0, 6.0, 1.0)
    assert a == b, "deterministic builder must return identical curves"
    assert len(a) == 512
    assert all(p["phase"] == 0.0 for p in a)
    peak = max(a, key=lambda p: p["mag"])
    # The nearest log-spaced sample sits within ~0.75% of f0 (analytic peak
    # itself is recovered by derive_freq's parabolic interpolation, tested by
    # the round-trip); its magnitude is the bell value at that sample.
    assert peak["f"] == pytest.approx(1000.0, rel=0.02)
    assert peak["mag"] == pytest.approx(6.0, abs=0.05)
    assert 1.0 < a[0]["f"] < 20.0          # ~10 Hz start
    assert a[-1]["f"] > 10000.0            # ~20 kHz end


def test_make_freq_curve_bandwidth_exact():
    """-3 dB bandwidth must equal peak_hz / q EXACTLY (analytic crossings).

    Independent check: interpolate the -3 dB crossings straight from the
    curve samples (log-f / linear-mag) and require f_high - f_low == f0 / Q.
    """
    f0, gain, q = 1000.0, 6.0, 1.0
    points = synthetic_dataset.make_freq_curve(f0, gain, q)
    level = gain - 3.0
    crossings = []
    for i in range(len(points) - 1):
        m0, m1 = points[i]["mag"], points[i + 1]["mag"]
        if (m0 - level) * (m1 - level) <= 0.0 and m1 != m0:
            t = (level - m0) / (m1 - m0)
            f = math.exp(math.log(points[i]["f"])
                         + t * (math.log(points[i + 1]["f"]) - math.log(points[i]["f"])))
            crossings.append(f)
    assert len(crossings) == 2
    bandwidth = max(crossings) - min(crossings)
    assert bandwidth == pytest.approx(f0 / q, rel=1e-3)


def test_make_compression_curve_structure():
    """Hard-knee model, 9 points, explicit threshold sample included."""
    thr, ratio = -30.0, 4.0
    curve = synthetic_dataset.make_compression_curve(thr, ratio)
    assert len(curve) == 9
    inputs = [p["input_db"] for p in curve]
    assert thr in inputs, "must include an explicit sample at input_db == threshold_db"
    assert min(inputs) == pytest.approx(thr - 40.0, abs=1e-9)
    assert max(inputs) == pytest.approx(thr + 20.0, abs=1e-9)
    for p in curve:
        assert p["gr_db"] == pytest.approx(p["input_db"] - p["output_db"], abs=1e-9)
        if p["input_db"] <= thr:
            model = p["input_db"]
        else:
            model = thr + (p["input_db"] - thr) / ratio
        assert p["output_db"] == pytest.approx(model, abs=1e-9)


def test_make_gr_timeline_structure_and_determinism():
    """Tau block passthrough + a non-trivial exponential attack/release trace."""
    a = synthetic_dataset.make_gr_timeline(0.001, 0.050)
    b = synthetic_dataset.make_gr_timeline(0.001, 0.050)
    assert a == b, "deterministic builder must return identical timelines"
    tau = a["tau"]
    assert tau["attack_sec"] == 0.001
    assert tau["release_sec"] == 0.050
    assert tau["valid"] is True
    assert tau["attack_by_level"] == []
    assert tau["release_by_level"] == []
    gr = a["gr"]
    assert gr["sample_rate"] == 48000.0
    assert gr["num_points"] == 2000
    assert len(gr["timeline"]) == 2000
    db = [p["gr_db"] for p in gr["timeline"]]
    assert db[0] == 0.0                      # flat before the step
    assert max(db) > 0.0                     # something happens
    assert any(l < r for l, r in zip(db, db[1:]))     # attack rises
    assert any(l > r for l, r in zip(db, db[1:]))     # release falls
