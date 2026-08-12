"""aggregate_report.py — Pure-function layer for batch reverse-derive reports.

Wave-1 T1-B (ticket #24): harmonic extraction/summary and
measurement-validity predicates consumed by the later-wave aggregation
engine.

Wave-2 T1-C (ticket #24): aggregation engine — plugin discovery
(discover_plugins), per-plugin analysis rows (analyze_plugin) and
ground-truth tolerance calibration (LOCKED_TOLERANCES, verify_against).

Wave-3 T1-D (ticket #24): report writers — write_markdown (human-readable
per-plugin report) and write_json (machine-readable mirror). Both are
deterministic (plugins sorted by slug) and idempotent: re-running with the
same rows/meta reproduces byte-identical output except the generated_at
timestamp. CLI wiring belongs to Wave 4 T1-E; none here.

Stdlib only, no third-party dependencies.

Usage (import only; consumed by the aggregation engine):
    from aggregate_report import load_harmonic, harmonic_summary, ...
"""
import json
import math
from pathlib import Path

from reverse_derive import derive_compression, derive_freq, derive_gr_tau

# ---------------------------------------------------------------------------
# Ground-truth tolerance calibration
# ---------------------------------------------------------------------------

# Locked pass/fail tolerances for verify_against. Seeded from the measured
# calibration seed of T1-A (synthetic fixtures round-tripped through
# reverse_derive): freq_hz worst 0.68% error, gain_db worst 0.0051 dB,
# Q worst 2.767%, compression threshold/ratio and GR attack/release bit-exact
# (0.0000 error). Each tolerance carries ~10x headroom over the measured
# error, matching issue #24 acceptance (freq mean |delta| < 0.5 dB).
LOCKED_TOLERANCES = {
    "freq_pct": 5.0,        # percent, e.g. 1000 Hz -> [950, 1050]
    "gain_db": 0.5,         # absolute dB
    "q_pct": 20.0,          # percent
    "threshold_db": 1.0,    # absolute dB
    "ratio_pct": 20.0,      # percent
    "tau_pct": 20.0,        # percent, attack_ms / release_ms
}

# Check-name -> (LOCKED_TOLERANCES key, error mode). Modes mirror
# reverse_derive._check: "pct" for freq/q/ratio/tau, "abs" for
# gain_db/threshold_db. The check "unit" labels the ERROR value: "%" for
# pct mode, "dB" for abs mode.
_CHECK_SPECS = {
    "freq_hz": ("freq_pct", "pct"),
    "gain_db": ("gain_db", "abs"),
    "q": ("q_pct", "pct"),
    "threshold_db": ("threshold_db", "abs"),
    "ratio": ("ratio_pct", "pct"),
    "attack_ms": ("tau_pct", "pct"),
    "release_ms": ("tau_pct", "pct"),
}

# ---------------------------------------------------------------------------
# Harmonic extraction
# ---------------------------------------------------------------------------


def load_harmonic(data):
    """Extract harmonic tone measurements from an export doc.

    Accepts both layouts used by PluginLab exports (SPEC.md):

      - dataset doc: the harmonic block is nested under ``data["harmonic"]``
      - standalone / body doc: tones live directly at the top level

    Missing or empty blocks yield []. Defensive fallback mirrors
    reverse_derive.py's ``_parse_*`` helpers (``data.get("harmonic") or
    data``).

    Returns a list of {"fundamental_hz", "fundamental_db", "thd_percent",
    "harmonics": [{"order", "freq", "mag_db", "percent"}, ...]}.
    """
    body = data.get("harmonic") or data
    return body.get("tones") or []


def harmonic_summary(tones):
    """Summarize each harmonic tone measurement into one compact dict.

    For every tone: {"fundamental_hz", "thd_percent", "dominant_order",
    "dominant_mag_db"}, where the dominant harmonic is the one with the
    maximum mag_db; both dominant fields are None when the tone has no
    harmonics. No tones in, no summaries out.
    """
    summary = []
    for tone in tones:
        harmonics = tone.get("harmonics") or []
        if harmonics:
            dominant = max(harmonics, key=lambda h: h.get("mag_db", -math.inf))
            dominant_order = dominant.get("order")
            dominant_mag_db = dominant.get("mag_db")
        else:
            dominant_order = None
            dominant_mag_db = None
        summary.append({"fundamental_hz": tone.get("fundamental_hz"),
                        "thd_percent": tone.get("thd_percent"),
                        "dominant_order": dominant_order,
                        "dominant_mag_db": dominant_mag_db})
    return summary


# ---------------------------------------------------------------------------
# Measurement-validity predicates
# ---------------------------------------------------------------------------


def is_eq_flat(mags):
    """True when the frequency-response magnitudes are degenerate-flat.

    All magnitudes are ≈ 0 dB: max(mags) - min(mags) < 0.1. Real historical
    captures are flat (all zeros). An empty capture counts as flat (nothing
    was measured).
    """
    if not mags:
        return True
    return max(mags) - min(mags) < 0.1


def is_compression_unity(curve):
    """True when the compression curve is degenerate-unity (no compression).

    All |gr_db| < 0.05, or the curve is empty.
    """
    if not curve:
        return True
    return all(abs(point.get("gr_db", 0.0)) < 0.05 for point in curve)


def is_harmonic_empty(tones):
    """True when no harmonic tone measurements were captured."""
    return not tones


# ---------------------------------------------------------------------------
# Plugin discovery
# ---------------------------------------------------------------------------


def discover_plugins(out_dir):
    """Scan out_dir one level deep for per-plugin measurement directories.

    Each direct child directory is reported as {"slug", "path",
    "has_dataset"}, sorted by slug. Directories WITHOUT a dataset.json are
    still listed (has_dataset=False — reported as no-data, never an error).
    summary.json is ignored entirely; out_dir may be relative and may not
    exist (empty result).
    """
    root = Path(out_dir)
    plugins = []
    if root.is_dir():
        for child in sorted(root.iterdir()):
            if not child.is_dir():
                continue
            plugins.append({"slug": child.name, "path": str(child),
                            "has_dataset": (child / "dataset.json").is_file()})
    return plugins


# ---------------------------------------------------------------------------
# Per-plugin analysis
# ---------------------------------------------------------------------------


def _derivation_failed_row(slug, plugin="?"):
    """Row shape used when the dataset file itself is unreadable/unparseable:
    every section derivation-failed (never a crash), overall verdict no-data.
    """
    return {"slug": slug, "plugin": plugin,
            "has_freq": False, "has_compression": False,
            "has_gr": False, "has_harmonic": False,
            "freq": {"freq_hz": None, "gain_db": None, "q": None,
                     "status": "derivation-failed"},
            "compression": {"threshold_db": None, "ratio": None,
                            "status": "derivation-failed", "json_fitted": {}},
            "gr": {"attack_ms": None, "release_ms": None, "valid": False,
                   "status": "derivation-failed"},
            "harmonic": {"tones_count": 0, "summary": [],
                         "status": "derivation-failed"},
            "status": "no-data"}


def _analyze_freq(data):
    """Frequency-response section row + presence flag.

    Mirrors reverse_derive._freq_body: raw points when non-empty, else
    smoothed_1_12. Flat magnitudes -> "degenerate" (no derivation attempt);
    any derivation exception -> "derivation-failed".
    """
    row = {"freq_hz": None, "gain_db": None, "q": None, "status": None}
    try:
        body = data.get("frequency_response") or {}
        points = (body.get("raw") or body.get("smoothed_1_12") or [])
        if not points:
            row["status"] = "no-data"
            return row, False
        if is_eq_flat([p["mag"] for p in points]):
            row["status"] = "degenerate"
            return row, True
        derived = derive_freq(points)
    except (ValueError, KeyError, TypeError):
        row["status"] = "derivation-failed"
        return row, True
    row.update(freq_hz=derived["freq_hz"], gain_db=derived["gain_db"],
               q=derived["q"], status="ok")
    return row, True


def _analyze_compression(data):
    """Compression section row + presence flag.

    Unity curve -> "degenerate" (no derivation attempt). The fitted block
    from the doc is carried through as json_fitted (may be empty).
    """
    row = {"threshold_db": None, "ratio": None,
           "status": None, "json_fitted": {}}
    try:
        body = data.get("compression") or {}
        curve = body.get("curve") or []
        row["json_fitted"] = body.get("fitted") or {}
        if not curve:
            row["status"] = "no-data"
            return row, False
        if is_compression_unity(curve):
            row["status"] = "degenerate"
            return row, True
        derived = derive_compression(curve)
    except (ValueError, KeyError, TypeError):
        row["status"] = "derivation-failed"
        return row, True
    row.update(threshold_db=derived["threshold_db"],
               ratio=derived["ratio"], status="ok")
    return row, True


def _analyze_gr(data):
    """GR timeline section row + presence flag.

    Tau values are passthrough; status is "ok" only when tau.valid, else
    "degenerate".
    """
    row = {"attack_ms": None, "release_ms": None, "valid": False,
           "status": None}
    try:
        tau = (data.get("gr_timeline") or {}).get("tau") or {}
        if not tau:
            row["status"] = "no-data"
            return row, False
        derived = derive_gr_tau(tau)
    except (ValueError, KeyError, TypeError):
        row["status"] = "derivation-failed"
        return row, True
    row.update(attack_ms=derived["attack_ms"], release_ms=derived["release_ms"],
               valid=derived["valid"])
    row["status"] = "ok" if derived["valid"] else "degenerate"
    return row, True


def _analyze_harmonic(data):
    """Harmonic section row + presence flag (tones extracted, summarized)."""
    row = {"tones_count": 0, "summary": [], "status": None}
    try:
        tones = load_harmonic(data)
    except (ValueError, KeyError, TypeError):
        row["status"] = "derivation-failed"
        return row, False
    row["tones_count"] = len(tones)
    row["summary"] = harmonic_summary(tones)
    row["status"] = "ok" if tones else "no-data"
    return row, bool(tones)


def analyze_plugin(dataset_path):
    """Analyze one plugin's dataset.json into a single report row.

    Per-section verdicts: "ok" (derived), "degenerate" (data present but
    flat/unity/invalid), "derivation-failed" (exception), "no-data" (block
    absent). A bad block never crashes the report: derive_* exceptions are
    caught per section. Overall verdict precedence: any "ok" -> "ok";
    else any "degenerate" -> "degenerate"; else "no-data".
    """
    path = Path(dataset_path)
    slug = path.parent.name

    try:
        with open(path, "r", encoding="utf-8") as f:
            data = json.load(f)
        if not isinstance(data, dict):
            raise ValueError("document is not a JSON object")
    except (OSError, ValueError, json.JSONDecodeError):
        return _derivation_failed_row(slug)

    try:
        plugin = (data.get("context") or {}).get("plugin") or "?"
    except (AttributeError, TypeError):
        plugin = "?"

    freq, has_freq = _analyze_freq(data)
    compression, has_compression = _analyze_compression(data)
    gr, has_gr = _analyze_gr(data)
    harmonic, has_harmonic = _analyze_harmonic(data)

    statuses = [freq["status"], compression["status"], gr["status"],
                harmonic["status"]]
    if "ok" in statuses:
        overall = "ok"
    elif "degenerate" in statuses:
        overall = "degenerate"
    else:
        overall = "no-data"

    return {"slug": slug, "plugin": plugin,
            "has_freq": has_freq, "has_compression": has_compression,
            "has_gr": has_gr, "has_harmonic": has_harmonic,
            "freq": freq, "compression": compression, "gr": gr,
            "harmonic": harmonic, "status": overall}


# ---------------------------------------------------------------------------
# Ground-truth verification
# ---------------------------------------------------------------------------


def verify_against(derived_row, known_params):
    """Check a derived row against known ground-truth parameters.

    Only parameters with BOTH a derived value and a known expected value are
    checked (error semantics mirror reverse_derive._check: pct for
    freq_hz/q/ratio/tau, abs for gain_db/threshold_db). GR timings are
    skipped when the tau block is invalid. Returns {"checks": {name:
    {"expected", "got", "error", "unit", "ok"}}, "all_ok"} — all_ok is False
    when nothing was checked.
    """
    derived = {
        "freq_hz": derived_row["freq"]["freq_hz"],
        "gain_db": derived_row["freq"]["gain_db"],
        "q": derived_row["freq"]["q"],
        "threshold_db": derived_row["compression"]["threshold_db"],
        "ratio": derived_row["compression"]["ratio"],
        "attack_ms": derived_row["gr"]["attack_ms"],
        "release_ms": derived_row["gr"]["release_ms"],
    }
    gr_valid = derived_row["gr"]["valid"]
    checks = {}
    for name, (tol_key, mode) in _CHECK_SPECS.items():
        expected = known_params.get(name)
        if expected is None:
            continue
        got = derived[name]
        if got is None or (name in ("attack_ms", "release_ms") and not gr_valid):
            continue
        if mode == "pct":
            err = (abs(got - expected) / abs(expected) * 100.0 if expected
                   else abs(got - expected) * 100.0)
            unit = "%"
        else:
            err = abs(got - expected)
            unit = "dB"
        checks[name] = {"expected": expected, "got": got, "error": err,
                        "unit": unit, "ok": err <= LOCKED_TOLERANCES[tol_key]}
    return {"checks": checks,
            "all_ok": bool(checks) and all(c["ok"] for c in checks.values())}


# ---------------------------------------------------------------------------
# Report writers (T1-D)
# ---------------------------------------------------------------------------

# Locked tolerance key -> unit suffix rendered in the markdown summary.
_TOLERANCE_UNITS = {"freq_pct": "%", "gain_db": " dB", "q_pct": "%",
                    "threshold_db": " dB", "ratio_pct": "%", "tau_pct": "%"}


def _count_rows(rows):
    """Aggregate counts for the report summary.

    with_data = at least one section carried data (even degenerate);
    no_data = no section carried data; degenerate = overall verdict
    degenerate; derivation_failed = any section status derivation-failed
    (e.g. an unreadable dataset file). with_data + no_data == total.
    """
    with_data = 0
    degenerate = 0
    derivation_failed = 0
    for row in rows:
        if any((row["has_freq"], row["has_compression"],
                row["has_gr"], row["has_harmonic"])):
            with_data += 1
        if row["status"] == "degenerate":
            degenerate += 1
        statuses = (row["freq"]["status"], row["compression"]["status"],
                    row["gr"]["status"], row["harmonic"]["status"])
        if "derivation-failed" in statuses:
            derivation_failed += 1
    return {"total": len(rows), "with_data": with_data,
            "no_data": len(rows) - with_data, "degenerate": degenerate,
            "derivation_failed": derivation_failed}


def _fmt_value(value):
    """Compact deterministic value rendering for markdown: floats via :g
    (no trailing zeros), None as n/a."""
    if value is None:
        return "n/a"
    if isinstance(value, float):
        return f"{value:g}"
    return str(value)


def _row_markdown(row):
    """One plugin's markdown section: header, identity, status and a
    per-type table (derived values + validity + status)."""
    freq = row["freq"]
    compression = row["compression"]
    gr = row["gr"]
    harmonic = row["harmonic"]
    validity = "valid" if gr["valid"] else "invalid"
    return ["## " + row["slug"], "",
            "Plugin: " + row["plugin"],
            "Status: " + row["status"], "",
            "| Section | Details | Status |",
            "| --- | --- | --- |",
            "| freq | freq_hz={}, gain_db={}, q={} | {} |".format(
                _fmt_value(freq["freq_hz"]), _fmt_value(freq["gain_db"]),
                _fmt_value(freq["q"]), freq["status"]),
            "| compression | threshold_db={}, ratio={} | {} |".format(
                _fmt_value(compression["threshold_db"]),
                _fmt_value(compression["ratio"]), compression["status"]),
            "| gr | attack_ms={}, release_ms={} ({}) | {} |".format(
                _fmt_value(gr["attack_ms"]), _fmt_value(gr["release_ms"]),
                validity, gr["status"]),
            "| harmonic | tones={} | {} |".format(harmonic["tones_count"],
                                                  harmonic["status"]), ""]


def _summary_markdown(counts, generated_at):
    """Trailing summary table: counts, locked tolerances, generated_at."""
    tolerances = ", ".join(f"{key}={value}{_TOLERANCE_UNITS[key]}"
                           for key, value in LOCKED_TOLERANCES.items())
    return ["## Summary", "",
            "| Metric | Value |",
            "| --- | --- |",
            f"| Total plugins | {counts['total']} |",
            f"| With data | {counts['with_data']} |",
            f"| No data | {counts['no_data']} |",
            f"| Degenerate | {counts['degenerate']} |",
            f"| Derivation failed | {counts['derivation_failed']} |",
            f"| Locked tolerances | {tolerances} |",
            f"| Generated at | {generated_at} |"]


def write_markdown(rows, meta, path):
    """Write a human-readable per-plugin aggregation report.

    Plugins are sorted by slug (defensive; discover_plugins already yields
    them sorted). The output is deterministic and idempotent: re-running
    with the same rows reproduces byte-identical output except the
    generated_at value from meta. Returns the path written; OSError from
    the filesystem is propagated to the caller (never swallowed).
    """
    sorted_rows = sorted(rows, key=lambda r: r["slug"])
    lines = ["# Plugin aggregation report", ""]
    for row in sorted_rows:
        lines.extend(_row_markdown(row))
    lines.extend(_summary_markdown(_count_rows(sorted_rows),
                                   meta["generated_at"]))
    out_path = Path(path)
    out_path.write_text("\n".join(lines) + "\n", encoding="utf-8")
    return out_path


def write_json(rows, meta, path):
    """Write a machine-readable mirror of the aggregation report.

    Top-level keys: generated_at, out_dir, tolerances (LOCKED_TOLERANCES),
    counts and plugins (sorted by slug). Well-formed JSON via json.dumps
    (indent=2, ensure_ascii=False) plus a trailing newline, mirroring the
    batch_collect summary convention. Returns the path written; OSError
    from the filesystem is propagated to the caller (never swallowed).
    """
    sorted_rows = sorted(rows, key=lambda r: r["slug"])
    doc = {"generated_at": meta["generated_at"],
           "out_dir": meta["out_dir"],
           "tolerances": LOCKED_TOLERANCES,
           "counts": _count_rows(sorted_rows),
           "plugins": sorted_rows}
    out_path = Path(path)
    out_path.write_text(json.dumps(doc, indent=2, ensure_ascii=False) + "\n",
                        encoding="utf-8")
    return out_path
