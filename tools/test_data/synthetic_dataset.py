"""
synthetic_dataset.py — Deterministic synthetic dataset.json fixture builder.

Constructs dataset.json-shaped documents (SPEC.md) carrying KNOWN plugin
parameters, so the reverse-derivation pipeline (tools/reverse_derive.py) can
be round-trip tested and its measured recovery errors calibrated:

    frequency_response -> {peak_hz, gain_db, q}  analytic -3 dB crossings,
                            bandwidth == peak_hz / q EXACTLY (f_high - f_low)
    compression         -> {threshold_db, ratio} exact hard-knee model,
                            explicit sample at input_db == threshold_db
    gr_timeline         -> {attack_sec, release_sec} exponential
                            attack/release trace + tau passthrough block

Stdlib only — no numpy/scipy/pandas. Fully deterministic (no randomness).
The round-trip suite lives in tools/test_synthetic_dataset.py.

Usage:
    python tools/test_data/synthetic_dataset.py --out-dir <dir> [--slug NAME]
        [--plugin NAME] [--peak-hz 1000] [--gain-db 6] [--q 1]
        [--threshold-db -30] [--ratio 4] [--attack-ms 1] [--release-ms 50]

Writes <out-dir>/<slug>/dataset.json (pretty-printed, ensure_ascii=False).

Exit code 0 = written, 1 = error (missing --out-dir / unwritable path).
"""
import argparse
import json
import math
import sys
from pathlib import Path

# ======================= Frequency response: analytic bell ===================
#
# mag(f) = gain_db - 3.0 * (abs(log(f / f0)) / log(bw_ratio))^2
#
# with bw_ratio = f_high / f0 = f0 / f_low and the analytic -3 dB crossings
#   f_low  = f0 * (sqrt(1 + 1/(4Q^2)) - 1/(2Q))
#   f_high = f0 * (sqrt(1 + 1/(4Q^2)) + 1/(2Q))
# so f_high - f_low == f0 / Q exactly. The bell peaks exactly at (f0, gain_db)
# and is clamped to a flat floor outside the crossings: 0 dB when the -3 dB
# points sit above the baseline (gain_db >= 3), otherwise the -3 dB level
# itself — which keeps the crossings (and therefore Q) recoverable for
# shallow/negative bells whose -3 dB points fall below 0 dB.
# ============================================================================

_F_MIN_HZ = 10.0
_F_MAX_HZ = 20000.0
# Margin below the -3 dB level for the flat floor of shallow/negative bells.
# derive_freq's parabolic peak interpolation can degrade to the raw peak
# sample (3-point fit flips sign on asymmetric windows), which shifts the
# derived peak by up to ~3*(h/2/log(bw_ratio))^2 dB — about 0.007 dB at Q = 3
# on the 512-point grid. The 0.05 dB margin keeps the -3 dB crossings (and
# therefore Q) recoverable for every combo in the round-trip suite.
_FLOOR_MARGIN_DB = 0.05


def make_freq_curve(peak_hz, gain_db, q, n_points=512):
    """Return [{f, mag, phase}...] of an analytic -3 dB bell.

    Log-spaced from ~10 Hz to ~20 kHz. -3 dB bandwidth == peak_hz / q exactly
    (analytic crossings, see module docstring); phase is always 0.0.
    """
    if n_points < 4:
        raise ValueError("n_points must be >= 4")
    if peak_hz <= 0.0 or q <= 0.0:
        raise ValueError("peak_hz and q must be positive")
    root = math.sqrt(1.0 + 1.0 / (4.0 * q * q))
    f_low = peak_hz * (root - 1.0 / (2.0 * q))
    f_high = peak_hz * (root + 1.0 / (2.0 * q))
    bw_ratio = f_high / peak_hz            # == f0 / f_low
    log_bw = math.log(bw_ratio)
    log_f0 = math.log(peak_hz)
    # Flat floor (see module docstring): 0 dB baseline for boosts whose -3 dB
    # points sit above it; for shallower/negative bells the floor sits
    # _FLOOR_MARGIN_DB under the -3 dB level so the crossings stay
    # recoverable even when the derivation falls back to the raw peak sample.
    floor = min(gain_db - 3.0 - _FLOOR_MARGIN_DB, 0.0)

    points = []
    for i in range(n_points):
        f = _F_MIN_HZ * (_F_MAX_HZ / _F_MIN_HZ) ** (i / (n_points - 1))
        x = abs(math.log(f) - log_f0) / log_bw
        mag = gain_db - 3.0 * x * x
        if mag < floor:
            mag = floor
        points.append({"f": f, "mag": mag, "phase": 0.0})
    return points


# ======================= Compression: exact hard-knee model ==================


def _input_offsets(n_points):
    """Deterministic log-ish input offsets (dB) around the threshold.

    Below-threshold points halve their spacing toward the knee, then the knee
    itself (offset 0) and two above-threshold steps; spans threshold-40 dB to
    threshold+20 dB and always includes 0 (== threshold).
    """
    n_below = max(n_points - 3, 1)
    below = []
    spacing = 40.0
    for _ in range(n_below):
        below.append(-spacing)
        spacing *= 0.5
    return (below + [0.0, 5.0, 20.0])[:n_points]


def make_compression_curve(threshold_db, ratio, n_points=9):
    """Return [{input_db, output_db, gr_db}...] of the hard-knee model:

        out = in                       for in <= threshold
        out = threshold + (in - thr)/r for in  >  threshold   (r = ratio)

    Inputs spread log-ish from threshold-40 to threshold+20 dB and include an
    explicit sample at input_db == threshold_db (derive_compression snaps the
    threshold to an input level, which makes recovery exact). gr_db = in - out.
    """
    if ratio <= 1.0:
        raise ValueError("ratio must be > 1")
    if n_points < 3:
        raise ValueError("n_points must be >= 3")
    curve = []
    for off in _input_offsets(n_points):
        in_db = threshold_db + off
        if in_db <= threshold_db:
            out_db = in_db
        else:
            out_db = threshold_db + (in_db - threshold_db) / ratio
        curve.append({"input_db": in_db, "output_db": out_db,
                      "gr_db": in_db - out_db})
    return curve


# ======================= GR timeline: exponential attack/release =============

_GR_SAMPLE_RATE = 48000.0
_GR_NUM_POINTS = 2000
_GR_TARGET_DB = 12.0           # deterministic GR depth of the step input
_GR_ATTACK_WINDOW_MULT = 5.0   # attack phase length, in attack time constants


def make_gr_timeline(attack_sec, release_sec):
    """Return a gr_timeline body: {"gr": {...trace...}, "tau": {...}}.

    Non-trivial exponential trace (useful for later curve comparison): flat 0
    before a step input, attack with time constant `attack_sec`, then release
    with time constant `release_sec`. The step sits at 15% of the window — a
    2000-point trace at 48 kHz is only ~42 ms, so a fixed t=0.1 s step would
    never be reached. tau.valid is always True (tau is a passthrough).
    """
    if attack_sec <= 0.0 or release_sec <= 0.0:
        raise ValueError("attack_sec and release_sec must be positive")
    duration = _GR_NUM_POINTS / _GR_SAMPLE_RATE
    step_t = 0.15 * duration
    attack_end = step_t + _GR_ATTACK_WINDOW_MULT * attack_sec
    gr_peak = _GR_TARGET_DB * (1.0 - math.exp(-_GR_ATTACK_WINDOW_MULT))

    timeline = []
    for i in range(_GR_NUM_POINTS):
        t = i / _GR_SAMPLE_RATE
        if t < step_t:
            gr_db = 0.0
        elif t < attack_end:
            gr_db = _GR_TARGET_DB * (1.0 - math.exp(-(t - step_t) / attack_sec))
        else:
            gr_db = gr_peak * math.exp(-(t - attack_end) / release_sec)
        timeline.append({"t": t, "gr_db": gr_db})

    return {
        "gr": {"sample_rate": _GR_SAMPLE_RATE, "num_points": _GR_NUM_POINTS,
               "timeline": timeline},
        "tau": {"attack_sec": attack_sec, "release_sec": release_sec,
                "valid": True, "attack_by_level": [], "release_by_level": []},
    }


# ======================= Harmonic tone set: THD-consistent ===================
#
# For each tone, harmonic partials 2..8 get weights that halve per order,
# normalized so THD% == sqrt(sum(percent^2)) — percent being each partial's
# amplitude as % of the fundamental. mag_db is that percent re-expressed in dB
# relative to the fundamental (floored at -200 dB when percent == 0).
# ============================================================================

_HARMONIC_ORDERS = (2, 3, 4, 5, 6, 7, 8)
_HARMONIC_WEIGHT_DECAY = 0.5


def make_harmonic(tones):
    """Fill each tone with a deterministic, THD-consistent harmonics list.

    Each input tone: {"fundamental_hz", "fundamental_db", "thd_percent"}.
    Each output tone adds "harmonics": [{"order", "freq", "mag_db",
    "percent"}, ...].
    """
    weights = [_HARMONIC_WEIGHT_DECAY ** (order - 2)
               for order in _HARMONIC_ORDERS]
    norm = math.sqrt(sum(w * w for w in weights))
    built = []
    for tone in tones:
        thd = float(tone["thd_percent"])
        harmonics = []
        for order, weight in zip(_HARMONIC_ORDERS, weights):
            percent = thd * weight / norm if thd > 0.0 else 0.0
            if percent > 0.0:
                mag_db = tone["fundamental_db"] + 20.0 * math.log10(percent / 100.0)
            else:
                mag_db = -200.0          # effectively silent, valid JSON
            harmonics.append({"order": order,
                              "freq": tone["fundamental_hz"] * order,
                              "mag_db": mag_db,
                              "percent": percent})
        built.append({"fundamental_hz": tone["fundamental_hz"],
                      "fundamental_db": tone["fundamental_db"],
                      "thd_percent": thd,
                      "harmonics": harmonics})
    return built


# ======================= Dataset assembly ====================================


def make_dataset(slug, plugin, *, peak_hz, gain_db, q, threshold_db, ratio,
                 attack_sec, release_sec, tones=None):
    """Return a full dataset.json-shaped dict with the given known parameters.

    Sections: gr_timeline (exponential trace + tau), frequency_response
    (raw bell; smoothed lists empty), harmonic (tones; empty when tones=None),
    compression (hard-knee curve + known fitted block).
    """
    return {
        "type": "dataset",
        "id": slug,
        "context": {
            "plugin": plugin,
            "class_id": "synthetic",
            "latency_samples": 0,
            "sample_rate": 48000,
            "measurement": {"sample_rate": 48000, "block_size": 512},
            "parameter_snapshot": {},
            "source": {"type": "dynamic"},
        },
        "gr_timeline": make_gr_timeline(attack_sec, release_sec),
        "frequency_response": {
            "raw": make_freq_curve(peak_hz, gain_db, q),
            "smoothed_1_12": [],
            "smoothed_1_3": [],
        },
        "harmonic": {"tones": make_harmonic(tones or [])},
        "compression": {
            "curve": make_compression_curve(threshold_db, ratio),
            "fitted": {"ratio": ratio, "threshold_db": threshold_db,
                       "knee_db": 0.0},
        },
    }


# ======================= CLI =================================================


def main():
    parser = argparse.ArgumentParser(
        description="Write a synthetic dataset.json with known parameters "
                    "(for reverse-derive round-trip calibration)")
    parser.add_argument("--out-dir", required=True,
                        help="root directory; <out-dir>/<slug>/dataset.json is written")
    parser.add_argument("--slug", default="synthetic")
    parser.add_argument("--plugin", default="Synthetic")
    parser.add_argument("--peak-hz", type=float, default=1000.0)
    parser.add_argument("--gain-db", type=float, default=6.0)
    parser.add_argument("--q", type=float, default=1.0)
    parser.add_argument("--threshold-db", type=float, default=-30.0)
    parser.add_argument("--ratio", type=float, default=4.0)
    parser.add_argument("--attack-ms", type=float, default=1.0)
    parser.add_argument("--release-ms", type=float, default=50.0)
    args = parser.parse_args()

    data = make_dataset(
        args.slug, args.plugin,
        peak_hz=args.peak_hz, gain_db=args.gain_db, q=args.q,
        threshold_db=args.threshold_db, ratio=args.ratio,
        attack_sec=args.attack_ms / 1000.0, release_sec=args.release_ms / 1000.0,
        tones=[{"fundamental_hz": 440.0, "fundamental_db": -12.0,
                "thd_percent": 1.0}])

    out_dir = Path(args.out_dir) / args.slug
    try:
        out_dir.mkdir(parents=True, exist_ok=True)
    except OSError as e:
        print(f"ERROR: cannot create {out_dir}: {e}", file=sys.stderr)
        sys.exit(1)
    out_file = out_dir / "dataset.json"
    try:
        with open(out_file, "w", encoding="utf-8") as f:
            json.dump(data, f, indent=2, ensure_ascii=False)
    except OSError as e:
        print(f"ERROR: cannot write {out_file}: {e}", file=sys.stderr)
        sys.exit(1)
    print(f"Wrote {out_file}")
    sys.exit(0)


if __name__ == "__main__":
    main()
