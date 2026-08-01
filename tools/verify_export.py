"""
verify_export.py — Validate frequency_response JSON output from PluginLab.

Reads pluginlab_freq_response.json, locates the peak magnitude in the
response curve, computes approximate Q from the -3 dB bandwidth, and
optionally asserts against expected values.

Usage:
    python tools/verify_export.py [file.json]
    python tools/verify_export.py --expected-freq 1000 --expected-gain 6.0 [file.json]
"""
import json
import sys
import math
import argparse
from pathlib import Path


def find_peak(raw_points):
    """Find the point with the highest magnitude in the raw data."""
    if not raw_points:
        return None
    peak = max(raw_points, key=lambda p: p["mag"])
    return peak


def find_3db_bandwidth(points, peak_idx, peak_mag):
    """
    Find the -3 dB points around the peak.
    Searches left and right for the first points where magnitude
    drops below (peak_mag - 3.0) dB. Returns (f_low, f_high).
    """
    threshold = peak_mag - 3.0

    f_low = None
    for i in range(peak_idx, -1, -1):
        if points[i]["mag"] <= threshold:
            f_low = points[i]["f"]
            break
    if f_low is None and len(points) > 0:
        f_low = points[0]["f"]

    f_high = None
    for i in range(peak_idx, len(points)):
        if points[i]["mag"] <= threshold:
            f_high = points[i]["f"]
            break
    if f_high is None and len(points) > 0:
        f_high = points[-1]["f"]

    return f_low, f_high


def compute_q(peak_freq, f_low, f_high):
    """Q ≈ f0 / (f_high - f_low) for equal peak response."""
    bandwidth = f_high - f_low
    if bandwidth <= 0:
        return None
    return peak_freq / bandwidth


def analyze(json_path):
    """Read a JSON file and extract peak frequency, gain, and Q."""
    with open(json_path, "r", encoding="utf-8") as f:
        data = json.load(f)

    raw = data.get("raw", [])
    smoothed = data.get("smoothed_1_12", [])
    smoothed_13 = data.get("smoothed_1_3", [])

    if not raw:
        print("ERROR: No raw data found in JSON")
        sys.exit(1)

    # Strategy: find peak in smoothed_1_12 first (cleaner), then refine in raw
    search_data = smoothed if smoothed else raw

    peak_pt = find_peak(search_data)
    if peak_pt is None:
        print("ERROR: No peak found")
        sys.exit(1)

    # Find the corresponding index in the search data
    peak_idx_search = None
    for i, p in enumerate(search_data):
        if p["f"] == peak_pt["f"] and abs(p["mag"] - peak_pt["mag"]) < 0.001:
            peak_idx_search = i
            break

    if peak_idx_search is None:
        # Fallback: find nearest in search_data by frequency
        peak_idx_search = min(
            range(len(search_data)),
            key=lambda i: abs(search_data[i]["f"] - peak_pt["f"]),
        )

    # Now find the peak in raw data closest to the smoothed peak freq
    peak_idx_raw = min(
        range(len(raw)),
        key=lambda i: abs(raw[i]["f"] - peak_pt["f"]),
    )
    raw_peak = raw[peak_idx_raw]

    peak_freq = raw_peak["f"]
    peak_gain = raw_peak["mag"]

    # Compute bandwidth from the cleaner smoothed data or raw
    f_low, f_high = find_3db_bandwidth(search_data, peak_idx_search, peak_pt["mag"])
    q = compute_q(peak_freq, f_low, f_high) if (f_low and f_high) else None

    return {
        "freq_hz": peak_freq,
        "gain_db": peak_gain,
        "q": q,
        "f_low": f_low,
        "f_high": f_high,
        "data_points_raw": len(raw),
        "data_points_smoothed_1_12": len(smoothed),
        "data_points_smoothed_1_3": len(smoothed_13),
        "type": data.get("type", "unknown"),
        "plugin": data.get("plugin", "unknown"),
        "sample_rate": data.get("sample_rate", data.get("measurement", {}).get("sample_rate")),
    }


def main():
    parser = argparse.ArgumentParser(
        description="Verify PluginLab frequency_response JSON export"
    )
    parser.add_argument(
        "json_file",
        nargs="?",
        default="pluginlab_freq_response.json",
        help="Path to the JSON export file (default: pluginlab_freq_response.json)",
    )
    parser.add_argument(
        "--expected-freq",
        type=float,
        default=None,
        help="Expected peak frequency in Hz",
    )
    parser.add_argument(
        "--expected-gain",
        type=float,
        default=None,
        help="Expected peak gain in dB",
    )
    parser.add_argument(
        "--expected-q",
        type=float,
        default=None,
        help="Expected Q value (optional)",
    )
    args = parser.parse_args()

    result = analyze(args.json_file)

    print(f"Plugin:        {result['plugin']}")
    print(f"Type:          {result['type']}")
    print(f"Sample Rate:   {result['sample_rate']}")
    print(f"Data Points:   raw={result['data_points_raw']}, "
          f"s12={result['data_points_smoothed_1_12']}, "
          f"s13={result['data_points_smoothed_1_3']}")
    print()
    print(f"Peak Frequency: {result['freq_hz']:.2f} Hz")
    print(f"Peak Gain:      {result['gain_db']:.2f} dB")
    if result["q"] is not None:
        print(f"Q (estimated):  {result['q']:.2f}")
        print(f"  f_low:         {result['f_low']:.2f} Hz")
        print(f"  f_high:        {result['f_high']:.2f} Hz")
    else:
        print("Q (estimated):  N/A (could not determine -3 dB bandwidth)")

    # Tolerance checks
    passed = True

    if args.expected_freq is not None:
        freq_err_pct = abs(result["freq_hz"] - args.expected_freq) / args.expected_freq * 100
        freq_ok = freq_err_pct <= 5.0
        print()
        print(f"Frequency Check: expected {args.expected_freq:.1f} Hz, "
              f"got {result['freq_hz']:.2f} Hz ({freq_err_pct:.1f}% error)"
              f" → {'PASS' if freq_ok else 'FAIL'}")
        if not freq_ok:
            passed = False

    if args.expected_gain is not None:
        gain_err = abs(result["gain_db"] - args.expected_gain)
        gain_ok = gain_err <= 0.5
        print(f"Gain Check:      expected {args.expected_gain:.2f} dB, "
              f"got {result['gain_db']:.2f} dB ({gain_err:.2f} dB error)"
              f" → {'PASS' if gain_ok else 'FAIL'}")
        if not gain_ok:
            passed = False

    if args.expected_q is not None and result["q"] is not None:
        q_err = abs(result["q"] - args.expected_q)
        q_ok = q_err <= args.expected_q * 0.3  # 30% tolerance for Q
        print(f"Q Check:         expected {args.expected_q:.2f}, "
              f"got {result['q']:.2f} ({q_err:.2f} error)"
              f" → {'PASS' if q_ok else 'FAIL'}")
        if not q_ok:
            passed = False

    print()
    if passed:
        print("VERDICT: ALL CHECKS PASSED")
    else:
        print("VERDICT: SOME CHECKS FAILED")
        sys.exit(1)


if __name__ == "__main__":
    main()
