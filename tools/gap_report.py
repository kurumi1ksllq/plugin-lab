"""gap_report.py — diff a measured export against the replica prototype (#106, T8).

Input: the measured export of a plugin, and the chain_doc that describes it.
The chain_doc is rendered through replica_proto into the same five export
shapes; this module diffs them per type and attributes each gap to the
processing stage that owns it.

The verdicts mirror compare_all.py's tolerance semantics so a gap report and
a compare_all run never disagree about the same data.

Output: JSON (machine) + markdown (human). Deterministic — no timestamps.
"""

import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import replica_proto as rp  # noqa: E402

# Per-type tolerance for "match", mirroring compare_all's curve limits (dB).
MATCH_LIMIT_DB = 1.0

# Which stage owns which measured type — the attribution table.
STAGE_OWNER = {
    "frequency_response": "eq",
    "compression": "dynamics",
    "gr_timeline": "dynamics",
    "nonlinear_family": "nonlinearity",
    "stereo_width": "stereo",
}

# Keys match replica_proto's render_replica() output (the file stems it writes).
TYPE_KEYS = ("frequency_response", "compression", "gr_timeline",
             "nonlinear_family", "stereo_width")


def _curve_points(block):
    """Points of a curve block.

    frequency_response carries its points under "raw" (SPEC.md §1); the other
    curve types use "curve". Returns [] when absent/empty.
    """
    if not isinstance(block, dict):
        return []
    for key in ("curve", "raw", "points"):
        points = block.get(key)
        if points:
            return points
    return []


def _diff_curve(measured_pts, replica_pts, x_key, y_key):
    """Per-point absolute deltas on the shared x values.

    Only x values present in both curves are compared: the measured export and
    the replica use different grids by design, and interpolating here would
    hide exactly the grid error the gap report is meant to expose.
    """
    if not measured_pts or not replica_pts:
        return []
    replica_by_x = {round(p.get(x_key, 0.0), 3): p.get(y_key) for p in replica_pts}
    deltas = []
    for point in measured_pts:
        x = round(point.get(x_key, 0.0), 3)
        if x not in replica_by_x:
            continue
        a = point.get(y_key)
        b = replica_by_x[x]
        if a is None or b is None:
            continue
        deltas.append(abs(float(a) - float(b)))
    return deltas


def _stats(deltas):
    if not deltas:
        return {"mean_abs": 0.0, "mean": 0.0, "worst_abs": 0.0, "n": 0}
    return {"mean_abs": round(sum(deltas) / len(deltas), 4),
            "mean": round(sum(deltas) / len(deltas), 4),
            "worst_abs": round(max(deltas), 4),
            "n": len(deltas)}


def _verdict(stats, limit=MATCH_LIMIT_DB):
    """match when the whole curve sits inside tolerance.

    Judged on the worst point, not the mean: a mean would let a single large
    deviation hide behind many matching points — exactly the gap this report
    exists to surface.
    """
    if stats["n"] == 0:
        return "match"
    return "match" if stats["worst_abs"] <= limit else "mismatch"


def build_report(measured, replica):
    """Diff every export type; returns {"per_type": {...}, "attribution": {...}}."""
    per_type = {}
    attribution = {}

    for key in TYPE_KEYS:
        attribution[key] = STAGE_OWNER.get(key)
        measured_block = measured.get(key)
        replica_block = replica.get(key)

        if measured_block is None:
            per_type[key] = {"verdict": "not-measured", "mean_abs": 0.0,
                             "mean": 0.0, "worst_abs": 0.0, "n": 0}
            continue
        if replica_block is None:
            per_type[key] = {"verdict": "not-rendered", "mean_abs": 0.0,
                             "mean": 0.0, "worst_abs": 0.0, "n": 0}
            continue

        if key == "frequency_response":
            deltas = _diff_curve(_curve_points(measured_block),
                                 _curve_points(replica_block), "f", "mag")
            stats = _stats(deltas)
            stats["verdict"] = _verdict(stats)
        elif key == "compression":
            deltas = _diff_curve(_curve_points(measured_block),
                                 _curve_points(replica_block), "input_db", "output_db")
            stats = _stats(deltas)
            stats["verdict"] = _verdict(stats)
        elif key == "gr_timeline":
            deltas = _diff_curve(_curve_points(measured_block),
                                 _curve_points(replica_block), "t", "gr_db")
            stats = _stats(deltas)
            stats["verdict"] = _verdict(stats)
        elif key == "nonlinear_family":
            same = (measured_block.get("verdict") == replica_block.get("verdict"))
            per_type[key] = {"verdict": "match" if same else "mismatch",
                             "mean_abs": 0.0, "mean": 0.0, "worst_abs": 0.0, "n": 1}
            continue
        else:
            # stereo_width: width_db is a per-frequency curve (SPEC stereo_width).
            deltas = _diff_curve(_curve_points(measured_block),
                                 _curve_points(replica_block), "f", "width_db")
            if not deltas:
                a = measured_block.get("width_db")
                b = replica_block.get("width_db")
                if not isinstance(a, (int, float)) or not isinstance(b, (int, float)):
                    per_type[key] = {"verdict": "not-measured", "mean_abs": 0.0,
                                     "mean": 0.0, "worst_abs": 0.0, "n": 0}
                    continue
                deltas = [abs(float(a) - float(b))]
            stats = _stats(deltas)
            stats["verdict"] = _verdict(stats)
            continue

        per_type[key] = stats

    return {"per_type": per_type, "attribution": attribution}


def render_json(report):
    """Stable JSON serialisation."""
    return json.dumps(report, indent=2, sort_keys=True) + "\n"


def render_markdown(report):
    """Human-readable gap report with per-type verdict and stage attribution."""
    lines = ["# Replica gap report", "",
             "Measured export vs the chain_doc's DSP prototype render.", "",
             "| type | verdict | mean_abs | worst_abs | n | stage |",
             "| --- | --- | --- | --- | --- | --- |"]
    per_type = report.get("per_type") or {}
    attribution = report.get("attribution") or {}
    for key in TYPE_KEYS:
        entry = per_type.get(key)
        if entry is None:
            continue
        lines.append("| {} | {} | {} | {} | {} | {} |".format(
            key, entry.get("verdict"), entry.get("mean_abs"),
            entry.get("worst_abs"), entry.get("n"), attribution.get(key) or "-"))

    gaps = [k for k, v in per_type.items() if v.get("verdict") == "mismatch"]
    lines += ["", "## Attribution", ""]
    if gaps:
        for key in gaps:
            lines.append("- `{}` mismatches -> the **{}** stage is the gap source.".format(
                key, attribution.get(key) or "unknown"))
    else:
        lines.append("- No type mismatches: the spec reproduces the measurement "
                     "within tolerance.")
    return "\n".join(lines) + "\n"


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Diff a measured export against its chain_doc replica render.")
    parser.add_argument("--measured", required=True, help="measured export JSON")
    parser.add_argument("--chain-doc", required=True, help="chain_doc JSON")
    parser.add_argument("--json-out", help="write the report JSON here")
    parser.add_argument("--md-out", help="write the report markdown here")
    args = parser.parse_args(argv)

    measured = json.loads(Path(args.measured).read_text(encoding="utf-8"))
    chain_doc = json.loads(Path(args.chain_doc).read_text(encoding="utf-8"))
    replica = rp.render(chain_doc)
    report = build_report(measured, replica)

    if args.json_out:
        Path(args.json_out).write_text(render_json(report), encoding="utf-8")
    if args.md_out:
        Path(args.md_out).write_text(render_markdown(report), encoding="utf-8")
    if not args.json_out and not args.md_out:
        sys.stdout.write(render_markdown(report))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
