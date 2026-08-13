"""aggregate_report.py — Batch reverse-derive aggregation reports.

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
timestamp.

Wave-4 T1-E (ticket #24): CLI — scan --out-dir, aggregate every plugin
with a dataset.json, write both reports and print a console summary.

Wave-5 (issue #32): data-integrity checks — per-plugin slug<->context.plugin
consistency (check_slug_plugin_consistency, carried as a data_integrity
field in analyze_plugin rows), byte-level duplicate detection across the
plugin set (detect_duplicate_datasets), a top-level "integrity" list in
write_json and a "## Data integrity" section in write_markdown; main()
warns on issues without changing the exit code.

Stdlib only, no third-party dependencies.

Usage (import only):
    from aggregate_report import load_harmonic, harmonic_summary, ...

Usage (CLI):
    python tools/aggregate_report.py [--out-dir DIR] [--report-dir DIR]
        [--json PATH] [--markdown PATH]
"""
import argparse
import datetime
import hashlib
import json
import math
import sys
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
# ratio_pct deviates from the headroom rule on purpose: it is aligned to the
# documented real-plugin bound in STATUS.md:140 — real Pro-C 3 round-trips
# show 14.3-23.5% ratio error (soft knee + under-compressed burst) and 25%
# passes, while the synthetic headroom bound (20%) would reject a legit
# real-plugin round-trip at T5 acceptance.
# harmonic_thd_pct is post-#38 (single-tone THD fix, PR #48): clean identity
# plugins measure THD < 1%, so 20.0 is the real-plugin acceptance bound — a
# plugin above it is very nonlinear or the measurement is broken. T5 (#28)
# consumes it ("harmonic 按 T1 校准容差判定"); calibrate() applies it as a
# one-sided measurement-sanity bound.
LOCKED_TOLERANCES = {
    "freq_pct": 5.0,            # percent, e.g. 1000 Hz -> [950, 1050]
    "gain_db": 0.5,             # absolute dB
    "q_pct": 20.0,              # percent
    "threshold_db": 1.0,        # absolute dB
    "ratio_pct": 25.0,          # percent (STATUS.md:140 real Pro-C 3 bound)
    "tau_pct": 20.0,            # percent, attack_ms / release_ms
    "harmonic_thd_pct": 20.0,   # percent, one-sided bound on measured THD
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
# Data integrity (issue #32)
# ---------------------------------------------------------------------------


def _slugify(name):
    """Slug-normalize a plugin name: lowercase, non-alphanumeric chars to
    '-', consecutive separators collapsed, no leading/trailing '-' (e.g.
    'Pro-Q 4' -> 'pro-q-4')."""
    out = []
    last_sep = False
    for ch in name.lower():
        if ch.isalnum():
            out.append(ch)
            last_sep = False
        elif not last_sep:
            out.append("-")
            last_sep = True
    return "".join(out).strip("-")


def check_slug_plugin_consistency(slug, plugin_name):
    """Check a plugin's directory slug against its dataset's context.plugin.

    Normalizes plugin_name (slugify + case-insensitive exact match) and
    compares to the slug. Returns {"ok": bool, "note": str|None} — ok True
    with note None when consistent, ok False with a mismatch note when not.
    A None/empty plugin_name cannot be judged: ok True, note
    'no context.plugin' (never a false positive).
    """
    if not plugin_name:
        return {"ok": True, "note": "no context.plugin"}
    if _slugify(plugin_name) == slug or plugin_name.lower() == slug.lower():
        return {"ok": True, "note": None}
    return {"ok": False,
            "note": f"slug '{slug}' vs context.plugin '{plugin_name}' "
                    "mismatch — possible duplicate/stale data"}


def detect_duplicate_datasets(rows):
    """Detect byte-level duplicate dataset files across the plugin set.

    Rows carry 'dataset_path' (each a dataset.json path). Any two rows whose
    files have identical SHA-256 (hashlib.sha256 over the raw file bytes)
    yield one {"slug_a", "slug_b", "sha256"} pair; pairs are sorted by
    (slug_a, slug_b) for deterministic output.
    """
    by_hash = {}
    for row in rows:
        digest = hashlib.sha256(Path(row["dataset_path"]).read_bytes())
        by_hash.setdefault(digest.hexdigest(), []).append(row["slug"])
    dups = []
    for digest, slugs in by_hash.items():
        for i, slug_a in enumerate(slugs):
            for slug_b in slugs[i + 1:]:
                dups.append({"slug_a": slug_a, "slug_b": slug_b,
                             "sha256": digest})
    return sorted(dups, key=lambda d: (d["slug_a"], d["slug_b"]))


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
            "data_integrity": {"slug_ok": True, "note": "no context.plugin"},
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
        context_plugin = (data.get("context") or {}).get("plugin")
    except (AttributeError, TypeError):
        context_plugin = None
    plugin = context_plugin if context_plugin else "?"
    check = check_slug_plugin_consistency(slug, context_plugin)
    integrity = {"slug_ok": check["ok"], "note": check["note"]}

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
            "harmonic": harmonic, "data_integrity": integrity,
            "status": overall}


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
                    "threshold_db": " dB", "ratio_pct": "%", "tau_pct": "%",
                    "harmonic_thd_pct": "%"}


def _integrity_issues(rows):
    """Rows whose slug<->context.plugin consistency check failed (slug_ok
    False). Rows without a data_integrity key (hand-built) or with an ok
    check never count as issues."""
    return [r for r in rows
            if r.get("data_integrity") and not r["data_integrity"]["slug_ok"]]


def _integrity_markdown(issues):
    """'## Data integrity' section lines: one table row per failing row."""
    lines = ["## Data integrity", "",
             "| Slug | Plugin | Note |",
             "| --- | --- | --- |"]
    for row in issues:
        lines.append("| {} | {} | {} |".format(
            row["slug"], row["plugin"],
            row["data_integrity"]["note"]))
    return lines + [""]


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
    them sorted). A '## Data integrity' section is emitted between the
    per-plugin sections and the summary ONLY when at least one row fails the
    slug<->context.plugin check (all-consistent runs stay noise-free). The
    output is deterministic and idempotent: re-running with the same rows
    reproduces byte-identical output except the generated_at value from
    meta. Returns the path written; OSError from the filesystem is
    propagated to the caller (never swallowed).
    """
    sorted_rows = sorted(rows, key=lambda r: r["slug"])
    lines = ["# Plugin aggregation report", ""]
    for row in sorted_rows:
        lines.extend(_row_markdown(row))
    issues = _integrity_issues(sorted_rows)
    if issues:
        lines.extend(_integrity_markdown(issues))
    lines.extend(_summary_markdown(_count_rows(sorted_rows),
                                   meta["generated_at"]))
    out_path = Path(path)
    out_path.write_text("\n".join(lines) + "\n", encoding="utf-8")
    return out_path


def write_json(rows, meta, path):
    """Write a machine-readable mirror of the aggregation report.

    Top-level keys: generated_at, out_dir, tolerances (LOCKED_TOLERANCES),
    counts, plugins (sorted by slug) and integrity — a list of
    {slug, plugin, slug_ok, note} for every row failing the slug<->context
    .plugin check, [] when all consistent (always present, deterministic).
    Well-formed JSON via json.dumps (indent=2, ensure_ascii=False) plus a
    trailing newline, mirroring the batch_collect summary convention.
    Returns the path written; OSError from the filesystem is propagated to
    the caller (never swallowed).
    """
    sorted_rows = sorted(rows, key=lambda r: r["slug"])
    doc = {"generated_at": meta["generated_at"],
           "out_dir": meta["out_dir"],
           "tolerances": LOCKED_TOLERANCES,
           "counts": _count_rows(sorted_rows),
           "plugins": sorted_rows,
           "integrity": [{"slug": r["slug"], "plugin": r["plugin"],
                          "slug_ok": r["data_integrity"]["slug_ok"],
                          "note": r["data_integrity"]["note"]}
                         for r in _integrity_issues(sorted_rows)]}
    out_path = Path(path)
    out_path.write_text(json.dumps(doc, indent=2, ensure_ascii=False) + "\n",
                        encoding="utf-8")
    return out_path


# ---------------------------------------------------------------------------
# CLI (T1-E)
# ---------------------------------------------------------------------------


def _utc_now_iso():
    """Stable ISO-8601 UTC timestamp (seconds precision) for generated_at."""
    return datetime.datetime.now(datetime.timezone.utc).isoformat(
        timespec="seconds")


def main(argv=None):
    """CLI entry: scan --out-dir, aggregate, write both reports, summarize.

    Exit codes: 0 = success; 2 = --out-dir missing entirely (mirrors
    compare_freq's missing-file convention). Plugins WITHOUT a dataset.json
    are skipped by discovery (never an error); the stale out/summary.json is
    not a plugin. OSError from the report writers propagates (never
    swallowed, matching write_markdown/write_json contract).
    """
    parser = argparse.ArgumentParser(
        description="Aggregate per-plugin analysis reports from a "
                    "PluginLab out directory")
    parser.add_argument("--out-dir", default="out", metavar="DIR",
                        help="directory to scan for per-plugin dataset dirs "
                             "(default: out)")
    parser.add_argument("--report-dir", default=".", metavar="DIR",
                        help="directory for the default-named reports "
                             "(default: current dir)")
    parser.add_argument("--json", metavar="PATH", default=None,
                        help="explicit aggregate_report.json path "
                             "(overrides --report-dir)")
    parser.add_argument("--markdown", metavar="PATH", default=None,
                        help="explicit aggregate_report.md path "
                             "(overrides --report-dir)")
    args = parser.parse_args(argv)

    out_dir = Path(args.out_dir)
    if not out_dir.is_dir():
        print(f"error: out dir not found: {args.out_dir}", file=sys.stderr)
        return 2

    plugins = discover_plugins(out_dir)
    dataset_paths = [Path(p["path"]) / "dataset.json"
                     for p in plugins if p["has_dataset"]]
    rows = [analyze_plugin(p) for p in dataset_paths]
    for row, path in zip(rows, dataset_paths):
        row["dataset_path"] = str(path)
    meta = {"generated_at": _utc_now_iso(),
            "out_dir": str(out_dir.resolve())}

    report_dir = Path(args.report_dir)
    report_dir.mkdir(parents=True, exist_ok=True)
    md_path = write_markdown(rows, meta,
                             args.markdown or report_dir / "aggregate_report.md")
    json_path = write_json(rows, meta,
                           args.json or report_dir / "aggregate_report.json")

    counts = _count_rows(rows)
    ok_count = counts["total"] - counts["degenerate"] - counts["no_data"]
    print(f"out dir: {out_dir.resolve()}")
    print(f"plugins: {counts['total']} (ok={ok_count}, "
          f"degenerate={counts['degenerate']}, "
          f"no-data={counts['no_data']}, "
          f"derivation-failed={counts['derivation_failed']})")
    print(f"reports: {md_path}, {json_path}")
    integrity_issues = _integrity_issues(rows)
    if integrity_issues:
        noun = "issue" if len(integrity_issues) == 1 else "issues"
        details = "; ".join(f"{r['slug']} <-> {r['plugin']}"
                            for r in integrity_issues)
        print(f"WARNING: {len(integrity_issues)} data-integrity {noun} "
              f"({details})")
    for dup in detect_duplicate_datasets(rows):
        print(f"WARNING: duplicate dataset: {dup['slug_a']} <-> {dup['slug_b']} "
              f"(identical dataset.json, SHA-256 {dup['sha256'][:12]})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
