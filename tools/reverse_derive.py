"""
reverse_derive.py — Reverse-derive plugin parameters from PluginLab export JSON.

Consumes the JSON documents produced by PluginLab's Export layer
(docs/data-schema.md) and reverses them back into the plugin parameters
that most plausibly produced the measurement:

  - frequency_response / scan(frequency_response) / dataset.scan
        raw (or smoothed_1_12) points  ->  peak {freq_hz, gain_db, q}
        Q from the -3 dB bandwidth around the peak (verify_export.py method,
        extended with parabolic peak interpolation + linear -3 dB crossings)
  - compression_curve / compression_family / dataset.compression_family
        curve[].{input_db, output_db}  ->  {threshold_db, ratio}
        piecewise-linear fit: unity slope below threshold, slope = 1/ratio
        above; threshold + ratio chosen by least squares (pure Python)
  - gr_timeline / dataset.gr_timeline
        tau.{attack_sec, release_sec}  ->  {attack_ms, release_ms}

Stdlib only — no numpy/scipy.

Usage:
    python tools/reverse_derive.py <json_file>
    python tools/reverse_derive.py <json_file> --expected-freq 1000 \
        --expected-gain 6 --expected-q 1 --tol-freq 5 --tol-gain 0.5
    python tools/reverse_derive.py <json_file> --expected-threshold -30 \
        --expected-ratio 4 --expected-attack-ms 1 --expected-release-ms 50

Exit code 0 = success (all checks within tolerance), 1 = failure.
"""
import argparse
import json
import math
import sys
from pathlib import Path

# ---------------------------------------------------------------------------
# Frequency response: peak + Q
# ---------------------------------------------------------------------------


def _parabolic_peak(freqs, mags, idx):
    """Sub-bin peak location via 3-point parabolic fit in log-frequency space.

    Returns (peak_freq_hz, peak_gain_db) or None if interpolation is not
    possible (peak at the array edge).
    """
    if idx <= 0 or idx >= len(freqs) - 1:
        return None
    x = [math.log(f) for f in freqs[idx - 1: idx + 2]]
    y = mags[idx - 1: idx + 2]
    denom = (x[0] - x[1]) * (x[0] - x[2]) * (x[1] - x[2])
    if abs(denom) < 1e-12:
        return None
    a = ((x[2] - x[1]) * y[0] + (x[0] - x[2]) * y[1] + (x[1] - x[0]) * y[2]) / denom
    b = ((x[1] - x[2]) * x[0] ** 2 * y[0] + (x[2] - x[0]) * x[1] ** 2 * y[1]
         + (x[0] - x[1]) * x[2] ** 2 * y[2]) / denom
    if a >= 0:                      # not a maximum
        return None
    x_star = -b / (2.0 * a)
    f_star = math.exp(x_star)
    g_star = a * x_star * x_star + b * x_star + (
        (x[1] * x[2] * (x[2] - x[1]) * y[0] + x[2] * x[0] * (x[0] - x[2]) * y[1]
         + x[0] * x[1] * (x[1] - x[0]) * y[2]) / denom)
    return f_star, g_star


def _crossing_log_freq(freqs, mags, i0, i1, level):
    """Linear interpolation (log-f / linear-mag) of the crossing of `level`
    between points i0 and i1. Returns Hz or None when not bracketed."""
    m0, m1 = mags[i0], mags[i1]
    if (m0 - level) * (m1 - level) > 0.0:
        return None
    if abs(m1 - m0) < 1e-9:
        return freqs[i1]
    t = (level - m0) / (m1 - m0)
    return math.exp(math.log(freqs[i0]) + t * (math.log(freqs[i1]) - math.log(freqs[i0])))


def derive_freq(points):
    """Derive {freq_hz, gain_db, q} from [{f, mag, phase}...] (dB magnitudes)."""
    if not points:
        raise ValueError("empty frequency response")
    freqs = [p["f"] for p in points]
    mags = [p["mag"] for p in points]

    peak_idx = max(range(len(mags)), key=lambda i: mags[i])

    interp = _parabolic_peak(freqs, mags, peak_idx)
    if interp is not None:
        peak_freq, peak_gain = interp
    else:
        peak_freq, peak_gain = freqs[peak_idx], mags[peak_idx]

    # -3 dB crossings on both sides of the (interpolated) peak.
    level = peak_gain - 3.0
    f_low = f_high = None

    for i in range(peak_idx, 0, -1):
        if mags[i - 1] <= level <= mags[i] or mags[i] <= level <= mags[i - 1]:
            f_low = _crossing_log_freq(freqs, mags, i - 1, i, level)
            break
    if f_low is None and mags[0] <= level:
        f_low = freqs[0]

    for i in range(peak_idx, len(mags) - 1):
        if mags[i] <= level <= mags[i + 1] or mags[i + 1] <= level <= mags[i]:
            f_high = _crossing_log_freq(freqs, mags, i, i + 1, level)
            break
    if f_high is None and mags[-1] <= level:
        f_high = freqs[-1]

    q = None
    if f_low is not None and f_high is not None and f_high > f_low > 0.0:
        q = peak_freq / (f_high - f_low)

    return {"freq_hz": peak_freq, "gain_db": peak_gain, "q": q,
            "f_low": f_low, "f_high": f_high}


def _freq_body(result):
    """Extract the best point list from a frequency-response result body."""
    raw = result.get("raw") or []
    s12 = result.get("smoothed_1_12") or []
    return raw if raw else s12


# ---------------------------------------------------------------------------
# Compression curve: threshold + ratio (piecewise-linear, pure Python)
# ---------------------------------------------------------------------------


def derive_compression(curve):
    """Derive {threshold_db, ratio} from [{input_db, output_db, gr_db}...].

    Model: out = in                       for in <= threshold
           out = threshold + (in - thr)/r for in  >  threshold   (r = ratio)
    Exhaustive threshold search over the measured input levels; the ratio is
    the least-squares slope^{-1} of output vs input above the candidate
    threshold. Best (threshold, ratio) minimises the total squared error.
    """
    if len(curve) < 3:
        raise ValueError("compression curve too short")
    pts = sorted((p["input_db"], p["output_db"]) for p in curve)

    best = None
    for thr in sorted(set(i for i, _ in pts)):
        above = [(i, o) for i, o in pts if i > thr]
        if len(above) < 2:
            continue
        mean_x = sum(i for i, _ in above) / len(above)
        mean_y = sum(o for _, o in above) / len(above)
        cov = sum((i - mean_x) * (o - mean_y) for i, o in above)
        var = sum((i - mean_x) ** 2 for i, _ in above)
        if var < 1e-9 or cov <= 0.0:
            continue
        slope = cov / var                     # d(out)/d(in) above threshold
        ratio = 1.0 / slope
        if ratio < 1.0 or ratio > 100.0:
            continue
        resid = 0.0
        for i, o in pts:
            model = i if i <= thr else thr + (i - thr) / ratio
            resid += (o - model) ** 2
        cand = (resid, thr, ratio)
        if best is None or cand[0] < best[0]:
            best = cand

    if best is None:
        raise ValueError("could not fit compression curve")
    return {"threshold_db": best[1], "ratio": best[2]}


def _compression_body(entry):
    """Extract (curve, fitted) from a compression body/family entry."""
    curve = entry.get("curve") or []
    fitted = entry.get("fitted") or {}
    return curve, fitted


# ---------------------------------------------------------------------------
# GR timeline: attack / release
# ---------------------------------------------------------------------------


def derive_gr_tau(tau):
    """Derive {attack_ms, release_ms} from a tau block."""
    attack_s = float(tau.get("attack_sec") or 0.0)
    release_s = float(tau.get("release_sec") or 0.0)
    valid = bool(tau.get("valid", attack_s > 0.0 or release_s > 0.0))
    return {"attack_ms": attack_s * 1000.0, "release_ms": release_s * 1000.0,
            "valid": valid}


# ---------------------------------------------------------------------------
# Document parsing (standalone + dataset layouts)
# ---------------------------------------------------------------------------


def _parse_freq_doc(data, out):
    if "raw" in data:
        body = {"raw": data.get("raw") or [],
                "smoothed_1_12": data.get("smoothed_1_12") or [],
                "smoothed_1_3": data.get("smoothed_1_3") or []}
        out["freq"].append({"round_label": data.get("plugin", "?"),
                            "result": body})


def _parse_compression_doc(data, out):
    if "curve" in data:
        curve, fitted = _compression_body(data)
        out["compression"].append({"round_label": data.get("plugin", "?"),
                                   "curve": curve, "fitted": fitted})


def _parse_gr_doc(data, out):
    if "tau" in data:
        out["gr"].append({"round_label": data.get("plugin", "?"),
                          "tau": data["tau"]})


def _parse_scan_doc(data, out):
    # scanToJSON places "family" at the top level (observed from Pro-Q 4
    # real export); the dataset nests the same layout under "scan".
    scan = data.get("scan") or {}
    family = scan.get("family") or data.get("family") or []
    if not family:
        return
    param_name = scan.get("param_name") or scan.get("param_id") or "?"
    texts = scan.get("param_texts") or []
    for i, entry in enumerate(family):
        result = entry.get("result") or {}
        if result.get("raw") or result.get("smoothed_1_12"):
            label = f"{param_name}@{texts[i]}" if i < len(texts) else f"{param_name}[{i}]"
            out["freq"].append({"round_label": label, "result": result})
        if result.get("curve"):
            curve, fitted = _compression_body(result)
            label = f"{param_name}@{texts[i]}" if i < len(texts) else f"{param_name}[{i}]"
            out["compression"].append({"round_label": label,
                                       "curve": curve, "fitted": fitted})


def _parse_compression_family(data, out):
    # Standalone compression_family nests "family" at top level; the dataset
    # nests the same layout under "compression_family".
    fam = data.get("compression_family") or data
    entries = fam.get("family") or []
    for i, entry in enumerate(entries):
        curve, fitted = _compression_body(entry)
        label = f"level={entry.get('input_level_db')}dB speed={entry.get('speed')}"
        out["compression"].append({"round_label": label, "curve": curve,
                                   "fitted": fitted})
        if entry.get("tau"):
            out["gr"].append({"round_label": label, "tau": entry["tau"]})


def _parse_gr_timeline(data, out):
    # Standalone gr_timeline carries "tau" at top level; the dataset nests
    # the same body under "gr_timeline".
    gt = data.get("gr_timeline") or data
    if gt.get("tau"):
        out["gr"].append({"round_label": data.get("plugin", "?"), "tau": gt["tau"]})


def parse_document(data):
    """Split any supported export into freq / compression / gr sections."""
    out = {"freq": [], "compression": [], "gr": []}
    doc_type = data.get("type", "unknown")

    if doc_type == "dataset":
        # The dataset nests each measurement body under a block key
        # (frequency_response / compression / gr_timeline); the flat top-level
        # calls below stay for any legacy layout.
        _parse_freq_doc(data.get("frequency_response") or {}, out)
        _parse_compression_doc(data.get("compression") or {}, out)
        _parse_freq_doc(data, out)            # dataset may carry flat freq body
        _parse_compression_doc(data, out)
        _parse_gr_doc(data, out)
        _parse_scan_doc(data, out)
        _parse_compression_family(data, out)
        _parse_gr_timeline(data, out)
    elif doc_type == "scan":
        _parse_scan_doc(data, out)
    elif doc_type == "compression_family":
        _parse_compression_family(data, out)
    elif doc_type == "gr_timeline":
        _parse_gr_timeline(data, out)
    else:                                     # frequency_response / compression_curve / ...
        _parse_freq_doc(data, out)
        _parse_compression_doc(data, out)
        _parse_gr_doc(data, out)
    return out


# ---------------------------------------------------------------------------
# Reporting + tolerance checks
# ---------------------------------------------------------------------------


def _check(name, got, expected, tol, mode):
    if expected is None:
        return True
    if got is None:
        print(f"  {name}: expected {expected} but nothing derived")
        return False
    if mode == "pct":
        err = abs(got - expected) / abs(expected) * 100.0 if expected else (
            abs(got - expected) * 100.0)
        ok = err <= tol
        print(f"  {name}: expected {expected}, got {got:.4g} "
              f"({err:.2f}% error) -> {'PASS' if ok else 'FAIL'}")
    else:
        err = abs(got - expected)
        ok = err <= tol
        print(f"  {name}: expected {expected}, got {got:.4g} "
              f"({err:.3f} error) -> {'PASS' if ok else 'FAIL'}")
    return ok


def main():
    parser = argparse.ArgumentParser(
        description="Reverse-derive plugin parameters from PluginLab export JSON")
    parser.add_argument("json_file", help="PluginLab export JSON (any supported type)")
    parser.add_argument("--expected-freq", type=float, default=None)
    parser.add_argument("--expected-gain", type=float, default=None)
    parser.add_argument("--expected-q", type=float, default=None)
    parser.add_argument("--expected-threshold", type=float, default=None)
    parser.add_argument("--expected-ratio", type=float, default=None)
    parser.add_argument("--expected-attack-ms", type=float, default=None)
    parser.add_argument("--expected-release-ms", type=float, default=None)
    parser.add_argument("--tol-freq", type=float, default=5.0,
                        help="freq tolerance in %% (default 5)")
    parser.add_argument("--tol-gain", type=float, default=0.5,
                        help="gain tolerance in dB (default 0.5)")
    parser.add_argument("--tol-q", type=float, default=20.0,
                        help="Q tolerance in %% (default 20)")
    parser.add_argument("--tol-threshold", type=float, default=1.0,
                        help="threshold tolerance in dB (default 1.0)")
    parser.add_argument("--tol-ratio", type=float, default=20.0,
                        help="ratio tolerance in %% (default 20)")
    parser.add_argument("--tol-attack", type=float, default=20.0,
                        help="attack tau tolerance in %% (default 20)")
    parser.add_argument("--tol-release", type=float, default=20.0,
                        help="release tau tolerance in %% (default 20)")
    parser.add_argument("--round-idx", type=int, default=None,
                        help="scan family round index for freq/compression "
                             "(default: all rounds)")
    args = parser.parse_args()

    path = Path(args.json_file)
    if not path.exists():
        print(f"ERROR: file not found: {path}")
        sys.exit(1)

    try:
        with open(path, "r", encoding="utf-8") as f:
            data = json.load(f)
    except (OSError, json.JSONDecodeError) as e:
        print(f"ERROR: cannot read/parse {path}: {e}")
        sys.exit(1)

    doc_type = data.get("type", "unknown")
    print(f"File:   {path}")
    print(f"Type:   {doc_type}")
    print(f"Plugin: {data.get('plugin') or data.get('context', {}).get('plugin', '?')}")
    print()

    sections = parse_document(data)
    if args.round_idx is not None:
        if args.round_idx < len(sections["freq"]):
            sections["freq"] = [sections["freq"][args.round_idx]]
        if args.round_idx < len(sections["compression"]):
            sections["compression"] = [sections["compression"][args.round_idx]]

    passed = True

    # --- frequency response ---
    for i, s in enumerate(sections["freq"]):
        try:
            r = derive_freq(_freq_body(s["result"]))
        except (ValueError, KeyError, TypeError) as e:
            print(f"FREQ[{i}] ({s['round_label']}): derivation failed: {e}")
            passed = False
            continue
        q_str = f"{r['q']:.2f}" if r["q"] is not None else "N/A"
        print(f"FREQ[{i}] ({s['round_label']}): "
              f"{{freq_hz: {r['freq_hz']:.2f}, gain_db: {r['gain_db']:.2f}, "
              f"q: {q_str}}}")
        if i == 0 or len(sections["freq"]) == 1:
            passed &= _check("freq_hz", r["freq_hz"], args.expected_freq,
                             args.tol_freq, "pct")
            passed &= _check("gain_db", r["gain_db"], args.expected_gain,
                             args.tol_gain, "abs")
            passed &= _check("q", r["q"], args.expected_q, args.tol_q, "pct")
    if not sections["freq"]:
        print("FREQ: no frequency-response data in document")

    # --- compression ---
    for i, s in enumerate(sections["compression"]):
        try:
            r = derive_compression(s["curve"])
        except (ValueError, KeyError, TypeError) as e:
            print(f"COMP[{i}] ({s['round_label']}): derivation failed: {e}")
            passed = False
            continue
        fitted = s["fitted"] or {}
        print(f"COMP[{i}] ({s['round_label']}): "
              f"{{threshold_db: {r['threshold_db']:.2f}, ratio: {r['ratio']:.2f}}}"
              + (f"  (json fitted: threshold={fitted.get('threshold_db')}, "
                 f"ratio={fitted.get('ratio')})" if fitted else ""))
        if i == 0 or len(sections["compression"]) == 1:
            passed &= _check("threshold_db", r["threshold_db"],
                             args.expected_threshold, args.tol_threshold, "abs")
            passed &= _check("ratio", r["ratio"], args.expected_ratio,
                             args.tol_ratio, "pct")
    if not sections["compression"]:
        print("COMP: no compression-curve data in document")

    # --- GR ---
    for i, s in enumerate(sections["gr"]):
        tau = s["tau"] or {}
        r = derive_gr_tau(tau)
        print(f"GR[{i}] ({s['round_label']}): "
              f"{{attack_ms: {r['attack_ms']:.2f}, release_ms: {r['release_ms']:.2f}, "
              f"valid: {r['valid']}}}")
        if i == 0 or len(sections["gr"]) == 1:
            if r["valid"]:
                passed &= _check("attack_ms", r["attack_ms"],
                                 args.expected_attack_ms, args.tol_attack, "pct")
                passed &= _check("release_ms", r["release_ms"],
                                 args.expected_release_ms, args.tol_release, "pct")
            else:
                print("  GR: tau not valid (no controlled edge) — skipping checks")
                if args.expected_attack_ms is not None or args.expected_release_ms is not None:
                    print("  GR: expected tau values given but tau invalid -> FAIL")
                    passed = False
    if not sections["gr"]:
        print("GR: no gr_timeline data in document")

    if not (sections["freq"] or sections["compression"] or sections["gr"]):
        print("ERROR: no derivable data in document")
        sys.exit(1)

    print()
    if passed:
        print("VERDICT: ALL CHECKS PASSED")
        sys.exit(0)
    print("VERDICT: SOME CHECKS FAILED")
    sys.exit(1)


if __name__ == "__main__":
    main()
