"""repro_check.py — 复现性检查（T3 #98）。

对比同一插件的两次 dataset.json 采集，跨 4 种测量类型（freq / compression /
gr_timeline / harmonic）用 compare_all 的纯函数计算 mean |delta|，判定是否
落在容差内。供 batch_collect --verify-repro 采集后调用，也可是独立 CLI。

用法:
    python tools/repro_check.py a_dataset.json b_dataset.json
    python tools/repro_check.py a.json b.json --limits '{"freq": 0.1}'

退出码: 0 = 复现性达标; 1 = 任一类型超差; 2 = 参数错误/文件缺失
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from typing import Any, Dict, Optional

sys.path.insert(0, str(Path(__file__).resolve().parent))

import compare_all as ca  # noqa: E402

# 默认容差（对齐 compare_all CLI 默认 + T3 基准 mean |Δ|=0 的宽松上限）
DEFAULT_LIMITS = {
    "freq": 0.5,          # dB（曲线 mean |Δ|）
    "compression": 0.5,   # dB（曲线 mean |Δ|）
    "gr_timeline": 0.5,   # dB（曲线 mean |Δ|）
    "harmonic": 20.0,     # %（THD mean |Δ|）
}

_TYPE_KEYS = {
    "freq": "frequency_response",
    "compression": "compression",
    "gr_timeline": "gr_timeline",
    "harmonic": "harmonic",
}


def _load_doc(path: str) -> dict:
    """Read a dataset JSON; raises ValueError on unreadable/invalid JSON."""
    with open(path, "r", encoding="utf-8") as fh:
        doc = json.load(fh)
    if not isinstance(doc, dict):
        raise ValueError(f"{path}: not a JSON object")
    return doc


def _type_present(doc: dict, type_name: str) -> bool:
    """True when the dataset carries the given measurement block."""
    key = _TYPE_KEYS[type_name]
    return key in doc and bool(doc[key])


def compare_datasets(a_path: str, b_path: str,
                     limits: Optional[Dict[str, float]] = None) -> dict:
    """Compare two dataset JSONs across the four measurement types.

    Returns:
        {
          "ok": bool,                     # all present types within limits
          "freq": {"mean_abs": float, "mean": float, "worst": {...}} | None,
          "compression": {...} | None,
          "gr_timeline": {...} | None,
          "harmonic": {...} | None,
          "failed": [type_name, ...],     # types that exceeded their limit
          "skipped": [type_name, ...],    # types absent from one/both docs
        }
    """
    limits = limits or DEFAULT_LIMITS
    doc_a = _load_doc(a_path)
    doc_b = _load_doc(b_path)

    results: Dict[str, Any] = {}
    failed: list[str] = []
    skipped: list[str] = []

    if _type_present(doc_a, "freq") and _type_present(doc_b, "freq"):
        results["freq"] = ca.compare_freq(doc_a["frequency_response"],
                                          doc_b["frequency_response"])
    else:
        results["freq"] = None
        skipped.append("freq")

    if _type_present(doc_a, "compression") and _type_present(doc_b, "compression"):
        results["compression"] = ca.compare_compression(doc_a["compression"],
                                                        doc_b["compression"])
    else:
        results["compression"] = None
        skipped.append("compression")

    if _type_present(doc_a, "gr_timeline") and _type_present(doc_b, "gr_timeline"):
        results["gr_timeline"] = ca.compare_gr(doc_a["gr_timeline"],
                                               doc_b["gr_timeline"])
    else:
        results["gr_timeline"] = None
        skipped.append("gr_timeline")

    if _type_present(doc_a, "harmonic") and _type_present(doc_b, "harmonic"):
        results["harmonic"] = ca.compare_harmonic(doc_a["harmonic"],
                                                  doc_b["harmonic"])
    else:
        results["harmonic"] = None
        skipped.append("harmonic")

    for type_name, result in results.items():
        if result is None:
            continue
        if result["mean_abs"] > limits[type_name]:
            failed.append(type_name)

    results.update({"ok": not failed, "failed": failed, "skipped": skipped})
    return results


def format_summary(report: dict) -> list:
    """Human-readable summary lines from a compare_datasets report."""
    lines = []
    for type_name in ("freq", "compression", "gr_timeline", "harmonic"):
        result = report.get(type_name)
        if result is None:
            lines.append(f"  {type_name:<12} skipped (missing in a/b)")
            continue
        verdict = "ok" if type_name not in report["failed"] else "FAIL"
        lines.append(f"  {type_name:<12} mean|d|={result['mean_abs']:.4f} "
                     f"worst=({result['worst']['x']:.1f}, "
                     f"{result['worst']['delta']:.3f}) [{verdict}]")
    if report["failed"]:
        lines.append(f"  REPRO FAILED: {', '.join(report['failed'])}")
    else:
        lines.append("  REPRO OK: all types within limits")
    return lines


def main(argv: list | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Compare two dataset JSONs for measurement reproducibility")
    parser.add_argument("a_json", help="first dataset.json")
    parser.add_argument("b_json", help="second dataset.json")
    parser.add_argument("--limits", metavar="JSON",
                        help='optional limits override, e.g. \'{"freq": 0.1}\'')
    args = parser.parse_args(argv)

    for path in (args.a_json, args.b_json):
        if not Path(path).is_file():
            print(f"error: {path} not found", file=sys.stderr)
            return 2

    limits = DEFAULT_LIMITS
    if args.limits:
        try:
            parsed = json.loads(args.limits)
            if not isinstance(parsed, dict):
                raise ValueError("limits must be a JSON object")
            limits = {**DEFAULT_LIMITS, **parsed}
        except ValueError as exc:
            print(f"error: invalid --limits: {exc}", file=sys.stderr)
            return 2

    try:
        report = compare_datasets(args.a_json, args.b_json, limits)
    except ValueError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2

    print(f"a dataset : {args.a_json}")
    print(f"b dataset : {args.b_json}")
    print("\n".join(format_summary(report)))
    return 0 if report["ok"] else 1


if __name__ == "__main__":
    sys.exit(main())