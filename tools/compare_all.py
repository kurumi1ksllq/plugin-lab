#!/usr/bin/env python3
"""compare_all.py — Compare two PluginLab export JSON documents (stdlib-only).

Pure comparison functions for the four non-scan measurement types:

  - frequency_response : raw magnitude curves on a shared log grid
  - compression        : input/output curves + fitted {threshold, ratio}
  - gr_timeline        : GR vs time curves + fitted {attack, release} tau
  - harmonic           : tone sets matched by nearest fundamental

Every compare_* function takes two export doc dicts (standalone export or
dataset.json block, both accepted) and returns a delta summary:
{"points": [{"x", "delta"}...], "mean_abs", "mean", "worst"} plus a
"params" field where the export carries fitted parameters.

CLI (exit 0 = PASS, 1 = FAIL, 2 = file missing / invalid input):

    python tools/compare_all.py freq        a.json b.json [--limit-db 0.5] [--fmin 100.0] [--fmax 10000.0] [--points 200]
    python tools/compare_all.py compression a.json b.json [--limit-output-db 0.5] [--tol-threshold 1.0] [--tol-ratio-pct 20.0]
    python tools/compare_all.py gr_timeline a.json b.json [--limit-gr-db 0.5] [--tol-tau-pct 20.0]
    python tools/compare_all.py harmonic    a.json b.json [--limit-thd-pct 20.0]

A run passes when the curve mean |delta| is below its limit AND all fitted
param tolerance checks pass.  Missing fitted params (compression), invalid
tau (gr_timeline) and unmatched harmonic tones are reported as SKIP or
warnings and never fail a run.  When the harmonic comparison matches no
tones the thd mean |delta| is 0.0 (vacuous) and counts as PASS — unmatched
tones are warnings, not failures.

Library usage:
    from compare_all import load_points, compare_freq, verdict
    points_a, doc_a = load_points("a.json", "freq")
"""

import argparse
import json
import sys
from pathlib import Path


# ---------------------------------------------------------------------------
# Document parsing (standalone + dataset layouts)
# ---------------------------------------------------------------------------


def _points_for(doc, mtype, label):
    """Return the point list for `mtype` from a doc dict.

    Standalone exports carry the body at top level; dataset.json nests the
    same layout under the block key (reverse_derive.py defensive pattern:
    `doc.get(block) or doc`).  `label` names the document in the ValueError
    raised when no point array is present.
    """
    if mtype == "freq":
        block = doc.get("frequency_response") or doc
        points = block.get("raw") or block.get("smoothed_1_12")
    elif mtype == "compression":
        block = doc.get("compression") or doc
        points = block.get("curve")
    elif mtype == "gr_timeline":
        block = doc.get("gr_timeline") or doc
        points = (block.get("gr") or {}).get("timeline")
    elif mtype == "harmonic":
        block = doc.get("harmonic") or doc
        points = block.get("tones")
    else:
        raise ValueError(f"{label}: unknown measurement type '{mtype}'")
    if not points:
        raise ValueError(f"{label}: no '{mtype}' point array "
                         f"(neither the '{mtype}' block nor the top-level "
                         f"key carries one)")
    return points


def load_points(path, mtype):
    """Load the point list for `mtype` from an export JSON file.

    Returns (points, doc).  `mtype` is one of
    "freq" / "compression" / "gr_timeline" / "harmonic".  Accepts both
    standalone exports (top-level keys) and dataset.json nesting (block
    keys).  Raises ValueError with the path when the point array is missing.
    """
    with open(path, encoding="utf-8") as f:
        doc = json.load(f)
    return _points_for(doc, mtype, path), doc


# ---------------------------------------------------------------------------
# Interpolation
# ---------------------------------------------------------------------------


def interp_linear(points, x, x_key, y_key):
    """Linear interpolation of y_key at x over a point array.

    Requires monotonically increasing x (compare_freq.interp_mag port,
    generalised to arbitrary keys); x outside the array is clamped to the
    nearest edge point.
    """
    if not points:
        raise ValueError("empty point array")
    if x <= points[0][x_key]:
        return points[0][y_key]
    if x >= points[-1][x_key]:
        return points[-1][y_key]
    for i in range(1, len(points)):
        if points[i][x_key] >= x:
            x0, x1 = points[i - 1][x_key], points[i][x_key]
            t = (x - x0) / (x1 - x0) if x1 > x0 else 0.0
            return points[i - 1][y_key] + t * (points[i][y_key] - points[i - 1][y_key])
    return points[-1][y_key]


# ---------------------------------------------------------------------------
# Curve comparison on a shared grid
# ---------------------------------------------------------------------------


def compare_curve(a_points, b_points, x_key, y_key, grid,
                  n=200, fmin=100.0, fmax=10000.0):
    """Interpolate both curves on a shared grid and return per-point deltas.

    delta(x) = a(x) - b(x) (compare_freq convention).  Grid choices:
      "log"          n log-spaced points between fmin and fmax
                     (compare_freq default: 200 points, 100 Hz - 10 kHz)
      "sorted-union" the sorted union of both curves' x values
    """
    if not a_points:
        raise ValueError("empty a point array")
    if not b_points:
        raise ValueError("empty b point array")
    if grid == "log":
        xs = [fmin * (fmax / fmin) ** (i / (n - 1)) for i in range(n)]
    elif grid == "sorted-union":
        xs = sorted({p[x_key] for p in a_points} | {p[x_key] for p in b_points})
    else:
        raise ValueError(f"unknown grid '{grid}'")

    points = []
    total = 0.0
    total_abs = 0.0
    worst = (0.0, 0.0)  # (x, delta) with the largest |delta|
    for x in xs:
        d = interp_linear(a_points, x, x_key, y_key) \
            - interp_linear(b_points, x, x_key, y_key)
        points.append({"x": x, "delta": d})
        total += d
        total_abs += abs(d)
        if abs(d) > abs(worst[1]):
            worst = (x, d)
    m = len(points)
    return {
        "points": points,
        "mean_abs": total_abs / m,
        "mean": total / m,
        "worst": {"x": worst[0], "delta": worst[1]},
    }


# ---------------------------------------------------------------------------
# Per-type comparisons
# ---------------------------------------------------------------------------


def compare_freq(a, b, fmin=100.0, fmax=10000.0, n=200):
    """Compare frequency_response raw curves on a shared log grid.

    Reproduces compare_freq.py's algorithm: mean |delta| over n log-spaced
    points between fmin and fmax (defaults 100 Hz - 10 kHz, 200 points).
    """
    return compare_curve(_points_for(a, "freq", "a"), _points_for(b, "freq", "b"),
                         "f", "mag", "log", n=n, fmin=fmin, fmax=fmax)


def _pct_delta(x, y):
    """Relative |x - y| in percent of x (None when x is zero)."""
    return abs(x - y) / x * 100.0 if x else None


def compare_compression(a, b):
    """Compare compression curves plus fitted {threshold, ratio} params.

    Curve delta on the sorted union of input_db values; params
    {"threshold_delta_db", "ratio_delta_pct"} when BOTH docs carry a
    `fitted` block, else params=None.
    """
    result = compare_curve(_points_for(a, "compression", "a"),
                           _points_for(b, "compression", "b"),
                           "input_db", "output_db", "sorted-union")
    fa = (a.get("compression") or a).get("fitted")
    fb = (b.get("compression") or b).get("fitted")
    if fa is not None and fb is not None:
        result["params"] = {
            "threshold_delta_db": abs(fa["threshold_db"] - fb["threshold_db"]),
            "ratio_delta_pct": _pct_delta(fa["ratio"], fb["ratio"]),
        }
    else:
        result["params"] = None
    return result


def compare_gr(a, b):
    """Compare gr_timeline curves plus fitted {attack, release} tau params.

    Curve delta on the sorted union of t values; params
    {"attack_delta_ms", "release_delta_ms"} when BOTH docs carry a tau
    block with valid=True (tau_sec converted to ms), else params=None —
    invalid tau is a SKIP condition, not a failure.
    """
    result = compare_curve(_points_for(a, "gr_timeline", "a"),
                           _points_for(b, "gr_timeline", "b"),
                           "t", "gr_db", "sorted-union")
    ta = (a.get("gr_timeline") or a).get("tau")
    tb = (b.get("gr_timeline") or b).get("tau")
    if ta and tb and ta.get("valid") and tb.get("valid"):
        result["params"] = {
            "attack_delta_ms": abs(ta["attack_sec"] - tb["attack_sec"]) * 1000.0,
            "release_delta_ms": abs(ta["release_sec"] - tb["release_sec"]) * 1000.0,
        }
    else:
        result["params"] = None
    return result


def _match_tones(tones_a, tones_b, frac=0.10):
    """Greedy nearest-fundamental matching of tone lists.

    Each tone of `a` is paired with the nearest unmatched tone of `b`
    whose relative frequency distance is within `frac` (default 10 %);
    returns (pairs, unmatched_fundamentals).
    """
    pairs = []
    used_b = set()
    matched_a = set()
    order_a = sorted(range(len(tones_a)),
                     key=lambda i: tones_a[i]["fundamental_hz"])
    for i in order_a:
        ta = tones_a[i]
        best_i, best_d = None, None
        for j, tb in enumerate(tones_b):
            if j in used_b:
                continue
            d = abs(ta["fundamental_hz"] - tb["fundamental_hz"])
            if best_d is None or d < best_d:
                best_i, best_d = j, d
        rel = best_d / ta["fundamental_hz"] if best_d is not None \
            and ta["fundamental_hz"] else 0.0
        if best_i is not None and rel <= frac:
            pairs.append((ta, tones_b[best_i]))
            used_b.add(best_i)
            matched_a.add(i)
    unmatched_a = [tones_a[i]["fundamental_hz"]
                   for i in range(len(tones_a)) if i not in matched_a]
    unmatched_b = [tb["fundamental_hz"]
                   for j, tb in enumerate(tones_b) if j not in used_b]
    return pairs, sorted(set(unmatched_a) | set(unmatched_b))


def compare_harmonic(a, b):
    """Compare harmonic tone sets, matched by nearest fundamental_hz.

    Per matched tone: |delta thd_percent| (x = fundamental_hz); per matched
    harmonic order: |delta mag_db| aggregated in "harmonics".  Tones of a or
    b without a match within 10 % land in "unmatched_tones" as warnings,
    never failures.
    """
    tones_a = _points_for(a, "harmonic", "a")
    tones_b = _points_for(b, "harmonic", "b")
    pairs, unmatched = _match_tones(tones_a, tones_b)

    points = []
    order_deltas = {}
    for ta, tb in pairs:
        d = abs(ta["thd_percent"] - tb["thd_percent"])
        points.append({"x": ta["fundamental_hz"], "delta": d})
        mag_a = {h["order"]: h["mag_db"] for h in (ta.get("harmonics") or [])}
        mag_b = {h["order"]: h["mag_db"] for h in (tb.get("harmonics") or [])}
        for order in sorted(set(mag_a) & set(mag_b)):
            order_deltas.setdefault(order, []).append(abs(mag_a[order] - mag_b[order]))

    m = len(points)
    if m:
        mean = sum(p["delta"] for p in points) / m
        mean_abs = sum(abs(p["delta"]) for p in points) / m
        worst = max(points, key=lambda p: abs(p["delta"]))
    else:
        mean = mean_abs = 0.0
        worst = {"x": None, "delta": 0.0}

    return {
        "points": points,
        "mean_abs": mean_abs,
        "mean": mean,
        "worst": worst,
        "harmonics": {order: {"mean_abs": sum(ds) / len(ds), "n": len(ds)}
                      for order, ds in order_deltas.items()},
        "unmatched_tones": unmatched,
    }


# ---------------------------------------------------------------------------
# Verdict
# ---------------------------------------------------------------------------


def verdict(mean_abs, limit):
    """(ok, text) — PASS when mean_abs < limit.

    Strict less-than, mirroring compare_freq.py's `mean_abs < limit_db`.
    """
    ok = mean_abs < limit
    return ok, f"{'PASS' if ok else 'FAIL'} (limit {limit} dB)"


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------


def _print_stats(res, unit):
    """Print mean |delta| / mean / worst-point lines for a compare result."""
    print(f"  mean |delta| : {res['mean_abs']:.4f} {unit}")
    print(f"  mean delta   : {res['mean']:+.4f} {unit}")
    x = res["worst"]["x"]
    x_str = f"{x:.3f}" if x is not None else "n/a"
    print(f"  worst point  : x={x_str}, delta {res['worst']['delta']:+.4f} {unit}")


def _curve_verdict(res, limit, unit, label="curve"):
    """Print the curve mean |delta| verdict; returns (ok, failure_line).

    failure_line is None on pass, else a one-line description of the check
    that failed (collected as the run's summary).
    """
    ok, text = verdict(res["mean_abs"], limit)
    print(f"  {label:<8s}: {text}")
    if not ok:
        return False, (f"{label} mean |delta| {res['mean_abs']:.4f} {unit} "
                       f">= limit {limit} {unit}")
    return True, None


def _run_freq(doc_a, doc_b, args):
    """freq run: log-grid curve comparison only."""
    res = compare_freq(doc_a, doc_b, fmin=args.fmin, fmax=args.fmax, n=args.points)
    print(f"comparison {args.fmin:.0f} Hz - {args.fmax:.0f} Hz, "
          f"{args.points} log-spaced points:")
    _print_stats(res, "dB")
    ok, fail = _curve_verdict(res, args.limit_db, "dB")
    failures = [] if fail is None else [fail]
    text = f"{'PASS' if ok else 'FAIL'} (limit {args.limit_db} dB)"
    return ok, text, failures


def _run_compression(doc_a, doc_b, args):
    """compression run: curve verdict + fitted {threshold, ratio} checks."""
    res = compare_compression(doc_a, doc_b)
    print(f"comparison on the sorted union of input_db values "
          f"({len(res['points'])} points):")
    _print_stats(res, "dB")
    curve_ok, curve_fail = _curve_verdict(res, args.limit_output_db, "dB")
    failures = [] if curve_fail is None else [curve_fail]

    params = res["params"]
    if params is None:
        print("  params  : SKIP (fitted missing in one or both docs)")
        ok = curve_ok
    else:
        for name, value, tol, unit in (
                ("threshold", params["threshold_delta_db"],
                 args.tol_threshold, "dB"),
                ("ratio", params["ratio_delta_pct"],
                 args.tol_ratio_pct, "%")):
            if value is None:
                print(f"  params  : {name} SKIP (a-side value 0)")
                continue
            ok_check = value <= tol
            print(f"  params  : {name} {'ok' if ok_check else 'FAIL'} "
                  f"(delta {value:.3f} {unit} vs tol {tol:.3f} {unit})")
            if not ok_check:
                failures.append(f"params {name} delta {value:.3f} {unit} "
                                f"> tol {tol:.3f} {unit}")
        ok = curve_ok and not any(f.startswith("params") for f in failures)

    text = f"{'PASS' if ok else 'FAIL'} (limit {args.limit_output_db} dB)"
    return ok, text, failures


def _run_gr(doc_a, doc_b, args):
    """gr_timeline run: curve verdict + fitted {attack, release} tau checks.

    Tau deltas are compared against --tol-tau-pct as a percent of the a-side
    tau value (pct = delta_ms / a_tau_ms * 100); a zero a-side tau is a SKIP.
    """
    res = compare_gr(doc_a, doc_b)
    print(f"comparison on the sorted union of t values "
          f"({len(res['points'])} points):")
    _print_stats(res, "dB")
    curve_ok, curve_fail = _curve_verdict(res, args.limit_gr_db, "dB")
    failures = [] if curve_fail is None else [curve_fail]

    params = res["params"]
    if params is None:
        print("  tau     : SKIP (invalid tau in one or both docs)")
        ok = curve_ok
    else:
        ta = (doc_a.get("gr_timeline") or doc_a).get("tau") or {}
        for name, delta_ms, a_key in (
                ("attack", params["attack_delta_ms"], "attack_sec"),
                ("release", params["release_delta_ms"], "release_sec")):
            a_ms = (ta.get(a_key) or 0.0) * 1000.0
            if a_ms == 0.0:
                print(f"  tau     : {name} SKIP (a-side {a_key} == 0)")
                continue
            pct = delta_ms / a_ms * 100.0
            ok_check = pct <= args.tol_tau_pct
            print(f"  tau     : {name} {'ok' if ok_check else 'FAIL'} "
                  f"(delta {delta_ms:.3f} ms = {pct:.2f} % of a-side "
                  f"{a_ms:.3f} ms, tol {args.tol_tau_pct:.2f} %)")
            if not ok_check:
                failures.append(f"tau {name} delta {pct:.2f} % of a-side "
                                f"> tol {args.tol_tau_pct:.2f} %")
        ok = curve_ok and not any(f.startswith("tau") for f in failures)

    text = f"{'PASS' if ok else 'FAIL'} (limit {args.limit_gr_db} dB)"
    return ok, text, failures


def _run_harmonic(doc_a, doc_b, args):
    """harmonic run: thd mean |delta| verdict; unmatched tones are warnings.

    With zero matched tones the thd mean |delta| is 0.0 (vacuous) and counts
    as PASS — unmatched tones are warnings, never failures (see docstring).
    """
    res = compare_harmonic(doc_a, doc_b)
    matched = len(res["points"])
    print(f"comparison of {matched} matched tone(s) by nearest fundamental:")
    _print_stats(res, "%")
    for f0 in res["unmatched_tones"]:
        print(f"  warning : unmatched tone at {f0:.1f} Hz "
              f"(warning, not a failure)")
    if matched == 0:
        print("  warning : no matched tones — thd mean |delta| is vacuous "
              "(0.0) and counts as PASS; unmatched tones are warnings, "
              "not failures")
    ok, _ = verdict(res["mean_abs"], args.limit_thd_pct)
    text = f"{'PASS' if ok else 'FAIL'} (limit {args.limit_thd_pct:g} %)"
    print(f"  thd     : {text}")
    if ok:
        return True, text, []
    return False, text, [f"thd mean |delta| {res['mean_abs']:.4f} % "
                         f">= limit {args.limit_thd_pct:g} %"]


def main():
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("type",
                        choices=("freq", "compression", "gr_timeline", "harmonic"),
                        help="measurement type to compare")
    parser.add_argument("a_json", help="first export JSON (the 'a' document)")
    parser.add_argument("b_json", help="second export JSON (the 'b' document)")
    parser.add_argument("--limit-db", type=float, default=0.5,
                        help="freq: curve mean |delta| limit in dB (default 0.5)")
    parser.add_argument("--limit-output-db", type=float, default=0.5,
                        help="compression: curve mean |delta| limit in dB (default 0.5)")
    parser.add_argument("--limit-gr-db", type=float, default=0.5,
                        help="gr_timeline: curve mean |delta| limit in dB (default 0.5)")
    parser.add_argument("--limit-thd-pct", type=float, default=20.0,
                        help="harmonic: thd mean |delta| limit in %% (default 20.0)")
    parser.add_argument("--tol-threshold", type=float, default=1.0,
                        help="compression: fitted threshold delta limit in dB (default 1.0)")
    parser.add_argument("--tol-ratio-pct", type=float, default=20.0,
                        help="compression: fitted ratio delta limit in %% (default 20.0)")
    parser.add_argument("--tol-tau-pct", type=float, default=20.0,
                        help="gr_timeline: fitted tau delta limit in %% (default 20.0)")
    parser.add_argument("--fmin", type=float, default=100.0,
                        help="freq: grid lower bound in Hz (default 100.0)")
    parser.add_argument("--fmax", type=float, default=10000.0,
                        help="freq: grid upper bound in Hz (default 10000.0)")
    parser.add_argument("--points", type=int, default=200,
                        help="freq: number of log-spaced grid points (default 200)")
    args = parser.parse_args()

    for path in (args.a_json, args.b_json):
        if not Path(path).is_file():
            print(f"error: {path} not found", file=sys.stderr)
            sys.exit(2)

    # json.JSONDecodeError subclasses ValueError, so this catches both
    # unreadable JSON and a missing point array for the requested type.
    try:
        points_a, doc_a = load_points(args.a_json, args.type)
        points_b, doc_b = load_points(args.b_json, args.type)
    except ValueError as err:
        print(f"error: {err}", file=sys.stderr)
        sys.exit(2)

    print(f"a export   : {args.a_json} ({len(points_a)} points)")
    print(f"b export   : {args.b_json} ({len(points_b)} points)")
    print(f"type       : {args.type}")

    if args.type == "freq":
        ok, text, failures = _run_freq(doc_a, doc_b, args)
    elif args.type == "compression":
        ok, text, failures = _run_compression(doc_a, doc_b, args)
    elif args.type == "gr_timeline":
        ok, text, failures = _run_gr(doc_a, doc_b, args)
    else:
        ok, text, failures = _run_harmonic(doc_a, doc_b, args)

    for line in failures:
        print(f"  failed   : {line}")
    print(f"\nVERDICT: {text}")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
