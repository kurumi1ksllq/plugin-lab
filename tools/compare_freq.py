#!/usr/bin/env python3
"""compare_freq.py — Compare two frequency_response JSON exports (stdlib-only).

Used for the real-machine MLS acceptance: measures the same plugin with the
sweep and the MLS excitation and checks that both estimate the same linear
response — mean |delta| < 0.5 dB over 100 Hz - 10 kHz.

Usage:
    python tools/compare_freq.py <mls.json> <sweep.json> [--limit-db 0.5]
"""

import argparse
import json
import sys
from pathlib import Path


def load_points(path):
    """Load the raw frequency-response points from an export JSON."""
    with open(path, encoding="utf-8") as f:
        doc = json.load(f)
    raw = doc.get("raw")
    if raw is None:
        # scan/dataset wrappers nest the result; accept a top-level "raw" only.
        raise ValueError(f"{path}: no top-level 'raw' point array")
    return raw, doc


def interp_mag(points, freq_hz):
    """Linear interpolation of magnitude (dB) at a frequency."""
    if not points:
        raise ValueError("empty point array")
    if freq_hz <= points[0]["f"]:
        return points[0]["mag"]
    if freq_hz >= points[-1]["f"]:
        return points[-1]["mag"]
    for i in range(1, len(points)):
        if points[i]["f"] >= freq_hz:
            f0, f1 = points[i - 1]["f"], points[i]["f"]
            t = (freq_hz - f0) / (f1 - f0) if f1 > f0 else 0.0
            return points[i - 1]["mag"] + t * (points[i]["mag"] - points[i - 1]["mag"])
    return points[-1]["mag"]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mls_json", help="frequency_response export measured with the MLS excitation")
    parser.add_argument("sweep_json", help="frequency_response export measured with the sweep excitation")
    parser.add_argument("--limit-db", type=float, default=0.5,
                        help="mean |delta| acceptance limit in dB (default 0.5)")
    parser.add_argument("--fmin", type=float, default=100.0)
    parser.add_argument("--fmax", type=float, default=10000.0)
    args = parser.parse_args()

    for p in (args.mls_json, args.sweep_json):
        if not Path(p).is_file():
            print(f"error: {p} not found", file=sys.stderr)
            sys.exit(2)

    mls_raw, mls_doc = load_points(args.mls_json)
    sweep_raw, sweep_doc = load_points(args.sweep_json)

    # The MLS export carries the excitation in context.measurement.
    mls_exc = mls_doc.get("measurement", {}).get("excitation", "sweep")
    print(f"mls export   : {args.mls_json} (excitation={mls_exc}, {len(mls_raw)} points)")
    print(f"sweep export : {args.sweep_json} (excitation={sweep_doc.get('measurement', {}).get('excitation', 'sweep')}, {len(sweep_raw)} points)")
    if mls_exc != "mls":
        print(f"warning: mls export does not carry excitation=mls", file=sys.stderr)

    # Compare on a common log-spaced grid so both curves are sampled equally.
    import math
    n = 200
    deltas = []
    worst = (0.0, 0.0)
    for i in range(n):
        f = args.fmin * (args.fmax / args.fmin) ** (i / (n - 1))
        d = interp_mag(mls_raw, f) - interp_mag(sweep_raw, f)
        deltas.append((f, d))
        if abs(d) > abs(worst[1]):
            worst = (f, d)

    mean_abs = sum(abs(d) for _, d in deltas) / len(deltas)
    mean = sum(d for _, d in deltas) / len(deltas)

    print(f"\ncomparison 100 Hz - 10 kHz, {n} log-spaced points:")
    print(f"  mean |delta| : {mean_abs:.4f} dB")
    print(f"  mean delta   : {mean:+.4f} dB")
    print(f"  worst point  : {worst[0]:.1f} Hz, delta {worst[1]:+.4f} dB")

    ok = mean_abs < args.limit_db
    print(f"\nresult: {'PASS' if ok else 'FAIL'} (limit {args.limit_db} dB)")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
