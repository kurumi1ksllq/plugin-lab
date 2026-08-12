"""describe_chain.py — Processing-chain description generator.

Wave-2 Task T4 of issue #26. Given the aggregate_report rows
(tools/aggregate_report.py — one row per measured plugin) plus optional
per-plugin dataset.json enrichment, builds the chain_doc: a per-plugin
description of the processing chain the black-box measurements suggest,
judged for "usable as a VST development spec" (no artifacts, no fit
conflicts, plausible time constants).

Deterministic and honest: every verdict is a pure function of the inputs,
and processing ORDER is never asserted from measurements (always
"unknown" + the canonical eq -> dyn -> eq suggestion). Artifact judgments
delegate to describe_quality.py predicates; the chain_doc contract below
is pinned and consumed by describe_render.py (Wave-1 T3):

    {slug, plugin, plugin_type:{kind,confidence,basis[]},
     eq:{present,overall,sections[],notes[]},
     dynamics:{present,compression:{threshold_derived,ratio_derived,
             threshold_json?,ratio_json?,knee_json?,conflict,conflict_note?},
             gr:{attack_ms,release_ms,attack_plausible,release_plausible,
             release_flag?,note?},notes[]},
     nonlinearity:{verdict,thd_range_pct?,description?,reason?},
     processing_order:{order,confidence,basis[],suggested?,suggestion_note?},
     usable_as_spec:bool, why_not_spec[]}

Stdlib only. Imports from the same tools/ directory: describe_quality
(predicates), aggregate_report (LOCKED_TOLERANCES), describe_render
(JSON/markdown output).

Usage (CLI):
    python tools/describe_chain.py aggregate_report.json [--dataset-dir DIR]
        [--json PATH] [--markdown PATH] [--meta-report-generated-at ISO]
"""

import argparse
import hashlib
import json
import re
import sys
from pathlib import Path

from aggregate_report import LOCKED_TOLERANCES
import describe_quality as dq
from describe_render import render_json, render_markdown

# ---------------------------------------------------------------------------
# Enrichment
# ---------------------------------------------------------------------------

# dataset.json keys that mark a section as carrying no usable measurement.
_DEGENERATE_STATUSES = ("degenerate", "no-data")

# Keyword families for classify_plugin_type (deterministic key matching).
_ANALYZER_KEYS = ("FFT Size", "Hold Peaks", "Smoothing")
_BAND_KEY_RE = re.compile(r"Band \d+ (Used|Frequency|Q|Gain)")
_DYNAMICS_SUBSTRINGS = ("Threshold", "Ratio", "Attack", "Release", "Makeup",
                        "Gain")

# Canonical, never-asserted processing order suggestion (eq -> dyn -> eq).
_SUGGESTED_ORDER = "eq -> dyn -> eq"
_SUGGESTION_NOTE = "canonical, not measured"


def enrich_from_dataset(dataset_path):
    """Read the enrichment fields out of one plugin's dataset.json.

    dataset.json layout (SPEC.md exports): context.parameter_snapshot,
    context.sample_rate (fall back context.measurement.sample_rate),
    harmonic.tones (raw tone list) and the presence of a compression_family
    key (checked at top level and inside context). Defensive contract:
    a missing file, an unparseable file or any malformed shape yields
    {"parameter_snapshot": None, "harmonic_block": None, "sample_rate": None,
    "has_compression_family": False} — never raises.

    Returns {"parameter_snapshot": dict|None, "harmonic_block": list|None,
    "sample_rate": float|None, "has_compression_family": bool}.
    """
    empty = {"parameter_snapshot": None, "harmonic_block": None,
             "sample_rate": None, "has_compression_family": False}
    try:
        dataset = json.loads(Path(dataset_path).read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return empty
    if not isinstance(dataset, dict):
        return empty

    context = dataset.get("context")
    if not isinstance(context, dict):
        context = {}

    snapshot = context.get("parameter_snapshot")
    if not isinstance(snapshot, dict):
        snapshot = None

    harmonic = dataset.get("harmonic")
    tones = harmonic.get("tones") if isinstance(harmonic, dict) else None
    if not isinstance(tones, list):
        tones = None

    sample_rate = context.get("sample_rate")
    if sample_rate is None:
        measurement = context.get("measurement")
        if isinstance(measurement, dict):
            sample_rate = measurement.get("sample_rate")
    if isinstance(sample_rate, (int, float)) and not isinstance(sample_rate,
                                                                bool):
        sample_rate = float(sample_rate)
    else:
        sample_rate = None

    has_family = ("compression_family" in dataset
                  or "compression_family" in context)

    return {"parameter_snapshot": snapshot, "harmonic_block": tones,
            "sample_rate": sample_rate, "has_compression_family": has_family}


# ---------------------------------------------------------------------------
# Plugin-type classification
# ---------------------------------------------------------------------------


def _row_content_blocks(row):
    """Count measurement blocks in an aggregate row carrying real content.

    A block counts when its section status is usable (not degenerate /
    no-data; GR additionally requires valid=True). Used only by the
    "processor" heuristic for rows without a parameter snapshot.
    """
    count = 0
    if row:
        freq = row.get("freq")
        if isinstance(freq, dict) and freq.get("status") not in \
                _DEGENERATE_STATUSES:
            count += 1
        compression = row.get("compression")
        if isinstance(compression, dict) and compression.get("status") not in \
                _DEGENERATE_STATUSES:
            count += 1
        gr = row.get("gr")
        if (isinstance(gr, dict) and gr.get("valid", True) is True
                and gr.get("status") not in _DEGENERATE_STATUSES):
            count += 1
        harmonic = row.get("harmonic")
        if isinstance(harmonic, dict) and harmonic.get("status") != "no-data":
            count += 1
    return count


def _eq_band_keys(snapshot):
    """Keys matching the EQ-band pattern (r"Band \\d+ (Used|...|Gain)")."""
    return [key for key in snapshot if _BAND_KEY_RE.search(key)]


def _dynamics_keys(snapshot):
    """Keys suggesting dynamics processing, excluding EQ-band gain keys."""
    bands = _eq_band_keys(snapshot)
    return [key for key in snapshot
            if key not in bands
            and any(sub in key for sub in _DYNAMICS_SUBSTRINGS)]


def classify_plugin_type(snapshot, row=None):
    """Classify the plugin family from its parameter snapshot keys.

    Deterministic keyword rules, checked in this order:
      - analyzer:      any FFT Size / Hold Peaks / Smoothing key
      - eq-only:       EQ-band keys present AND no dynamics keys; basis
                       notes whether every "Band N Used" is 0.0 (all bands
                       unused) or not (eq-active)
      - dynamics:      any Threshold/Ratio/Attack/Release/Makeup/Gain key —
                       "compressor" when both Threshold and Ratio present,
                       else "dynamics-only"
      - processor:     snapshot empty/None BUT the aggregate row carries
                       real content in >= 2 blocks (synthetic/clean rows
                       without a parameter snapshot; heuristic, basis
                       explains) — only when `row` is given
      - unknown:       nothing matched (or no snapshot at all)

    Returns {"kind": str, "confidence": "high"|"low", "basis": [str]}.
    """
    if snapshot is None or not isinstance(snapshot, dict) or not snapshot:
        if row is not None and _row_content_blocks(row) >= 2:
            return {"kind": "processor", "confidence": "low",
                    "basis": ["no parameter snapshot",
                              f"row carries real content in "
                              f"{_row_content_blocks(row)} blocks "
                              f"(processor heuristic)"]}
        return {"kind": "unknown", "confidence": "low",
                "basis": ["no parameter snapshot"]}

    matched_analyzer = [key for key in _ANALYZER_KEYS if key in snapshot]
    if matched_analyzer:
        return {"kind": "analyzer", "confidence": "high",
                "basis": ["analyzer keys present: "
                          + ", ".join(matched_analyzer)]}

    bands = _eq_band_keys(snapshot)
    dynamics_keys = _dynamics_keys(snapshot)
    if bands and not dynamics_keys:
        used_keys = [key for key in bands if key.endswith(" Used")]
        unused = (bool(used_keys)
                  and all(snapshot.get(key) == 0.0 for key in used_keys))
        if unused:
            note = f"all {len(used_keys)} bands unused (Used == 0.0)"
        else:
            note = f"{len(bands)} EQ band keys, active usage"
        return {"kind": "eq-only", "confidence": "high",
                "basis": ["EQ band keys present", note]}

    if dynamics_keys:
        has_threshold = any("Threshold" in key for key in dynamics_keys)
        has_ratio = any("Ratio" in key for key in dynamics_keys)
        kind = "compressor" if has_threshold and has_ratio else "dynamics-only"
        return {"kind": kind, "confidence": "high",
                "basis": ["dynamics keys present: "
                          + ", ".join(sorted(dynamics_keys))]}

    return {"kind": "unknown", "confidence": "low",
            "basis": ["no recognized parameter keys"]}


# ---------------------------------------------------------------------------
# EQ block
# ---------------------------------------------------------------------------


def build_eq(freq_row, ctx):
    """Describe the EQ-style measurement block.

    freq_row is the aggregate row's freq section ({freq_hz, gain_db, q,
    status}); ctx carries the enriched sample_rate used as the nyquist
    bound for classify_freq_peak. A single-peak model: at most one
    resolvable band is reported. status "degenerate"/"no-data" or a
    missing row → present False / overall "none".

    Returns the eq contract dict: {present, overall, sections:[{freq_hz,
    gain_db, q, plausible, flag?, reason?}], notes:[str]}.
    """
    if freq_row is None or not isinstance(freq_row, dict):
        return {"present": False, "overall": "none", "sections": [],
                "notes": ["no frequency section"]}
    status = freq_row.get("status")
    if status in _DEGENERATE_STATUSES:
        return {"present": False, "overall": "none", "sections": [],
                "notes": [f"freq status {status!r}: no usable fit"]}

    verdict = dq.classify_freq_peak(freq_row,
                                    nyquist=(ctx or {}).get("sample_rate"))
    section = {"freq_hz": freq_row.get("freq_hz"),
               "gain_db": freq_row.get("gain_db"),
               "q": freq_row.get("q"),
               "plausible": verdict["plausible"],
               "flag": verdict["flag"],
               "reason": verdict["reason"]}
    notes = ["at most one resolvable band (single-peak model)"]
    if not verdict["plausible"]:
        notes.append(f"{verdict['flag']}: {verdict['reason']}")
    return {"present": True,
            "overall": "clean" if verdict["plausible"] else "artifact",
            "sections": [section], "notes": notes}


# ---------------------------------------------------------------------------
# Dynamics block
# ---------------------------------------------------------------------------


def build_dynamics(compression_row, gr_row, ctx):
    """Describe the compression + GR measurement blocks.

    compression: threshold_derived/ratio_derived come from the aggregate
    row; the json_fitted sub-block (when present) provides
    threshold_json/ratio_json/knee_json. conflict = derived vs json
    threshold differ by more than LOCKED_TOLERANCES["threshold_db"] (1.0
    dB) OR ratio percent difference exceeds
    LOCKED_TOLERANCES["ratio_pct"] (20%, error math mirrors
    aggregate_report._check: pct relative to the json fit). A conflict
    note names both fits so a reader can judge them.

    gr: attack/release plausibility via describe_quality.tau_sanity
    (locked TAU bounds + release extreme-mismatch flag).

    `ctx` (the enrichment dict) is accepted for signature uniformity with
    build_eq; it currently carries nothing dynamics needs.

    Returns the dynamics contract dict: {present, compression:{...},
    gr:{...}, notes:[str]}.
    """
    comp_ok = (isinstance(compression_row, dict)
               and compression_row.get("status") not in _DEGENERATE_STATUSES)
    gr_ok = (isinstance(gr_row, dict) and gr_row.get("valid", True) is True
             and gr_row.get("status") not in _DEGENERATE_STATUSES)
    notes = []
    if not comp_ok:
        notes.append("no usable compression section")
    if not gr_ok:
        notes.append("no usable GR section")

    threshold_derived = compression_row.get("threshold_db") \
        if isinstance(compression_row, dict) else None
    ratio_derived = compression_row.get("ratio") \
        if isinstance(compression_row, dict) else None
    threshold_json = ratio_json = knee_json = None
    conflict = False
    conflict_note = None
    if comp_ok:
        json_fitted = compression_row.get("json_fitted")
        if isinstance(json_fitted, dict) and json_fitted:
            threshold_json = json_fitted.get("threshold_db")
            ratio_json = json_fitted.get("ratio")
            knee_json = json_fitted.get("knee_db")
            parts = []
            if (threshold_derived is not None and threshold_json is not None
                    and abs(threshold_derived - threshold_json)
                    > LOCKED_TOLERANCES["threshold_db"]):
                parts.append(
                    f"derived vs json threshold differ: {threshold_derived:g} "
                    f"vs {threshold_json:g} dB")
            if (ratio_derived is not None and ratio_json is not None):
                if ratio_json:
                    ratio_pct = (abs(ratio_derived - ratio_json)
                                 / abs(ratio_json) * 100.0)
                else:
                    ratio_pct = abs(ratio_derived - ratio_json) * 100.0
                if ratio_pct > LOCKED_TOLERANCES["ratio_pct"]:
                    parts.append(
                        f"derived vs json ratio differ: {ratio_derived:g} "
                        f"vs {ratio_json:g} ({ratio_pct:g}%)")
            if parts:
                conflict = True
                conflict_note = "; ".join(parts)

    if isinstance(gr_row, dict):
        sanity = dq.tau_sanity(gr_row)
        attack_ms = gr_row.get("attack_ms")
        release_ms = gr_row.get("release_ms")
    else:
        sanity = {"attack_plausible": False, "release_plausible": False,
                  "flag": None, "note": "no GR section"}
        attack_ms = release_ms = None

    compression = {"threshold_derived": threshold_derived,
                   "ratio_derived": ratio_derived,
                   "threshold_json": threshold_json,
                   "ratio_json": ratio_json,
                   "knee_json": knee_json,
                   "conflict": conflict,
                   "conflict_note": conflict_note}
    gr = {"attack_ms": attack_ms, "release_ms": release_ms,
          "attack_plausible": sanity["attack_plausible"],
          "release_plausible": sanity["release_plausible"],
          "release_flag": sanity["flag"], "note": sanity["note"]}
    return {"present": comp_ok or gr_ok, "compression": compression,
            "gr": gr, "notes": notes}


# ---------------------------------------------------------------------------
# Nonlinearity block
# ---------------------------------------------------------------------------


def build_nonlinearity(harmonic_row, all_fingerprints):
    """Describe the harmonic-measurement block.

    Verdict from describe_quality.classify_harmonic (clean / artifact /
    not-measured); additionally an artifact when the plugin's raw harmonic
    block shares a fingerprint with other plugins (identical raw tones
    across plugins = same processing chain suspected). harmonic_row may
    carry "harmonic_raw" (the dataset.json harmonic.tones list, merged in
    by build_chain_doc) so the membership check can hash it; all_fingerprints
    is the set of sha1 fingerprints shared by >= 2 enriched plugins
    (describe_quality.detect_duplicate_fingerprints).

    Returns the nonlinearity contract dict: {verdict, thd_range_pct?
    [min,max]|None, description? str|None, reason? str|None}.
    """
    row = harmonic_row if isinstance(harmonic_row, dict) else {}
    verdict = dq.classify_harmonic(row)
    thd_range_pct = verdict["thd_range_pct"]
    reasons = list(verdict["reasons"])

    raw = row.get("harmonic_raw")
    if (verdict["verdict"] == "clean" and isinstance(raw, list)
            and all_fingerprints):
        canonical = json.dumps(raw, sort_keys=True, default=str).encode()
        fingerprint = hashlib.sha1(canonical).hexdigest()
        if fingerprint in all_fingerprints:
            verdict = {"verdict": "artifact", "thd_range_pct": thd_range_pct,
                       "reasons": reasons + [
                           "identical harmonic fingerprint shared across "
                           "plugins (shared chain suspected)"]}

    description = None
    if thd_range_pct is not None:
        tones_count = row.get("tones_count", 0)
        description = ("THD {min:g}-{max:g}% across {n} tones".format(
            min=thd_range_pct[0], max=thd_range_pct[1], n=tones_count))

    return {"verdict": verdict["verdict"],
            "thd_range_pct": thd_range_pct,
            "description": description,
            "reason": "; ".join(verdict["reasons"])
            if verdict["reasons"] else None}


# ---------------------------------------------------------------------------
# Processing-order block
# ---------------------------------------------------------------------------


def infer_order(plugin_type, eq, dynamics, snapshot):
    """Describe processing order — always honestly "unknown".

    Measurement data cannot prove insertion order (eq -> dyn -> eq is the
    canonical guess, never asserted as measured). basis always lists the
    evidence (plugin type, eq presence, dynamics presence); a low-confidence
    heuristic line is appended only when the parameter snapshot is decisive
    (e.g. a compressor snapshot with no EQ bands → possible dyn-only chain).

    Returns the processing_order contract dict: {order, confidence,
    basis[], suggested, suggestion_note}.
    """
    eq_present = bool(eq.get("present")) if isinstance(eq, dict) else False
    dynamics_present = bool(dynamics.get("present")) \
        if isinstance(dynamics, dict) else False
    kind = plugin_type.get("kind") if isinstance(plugin_type, dict) \
        else "unknown"
    basis = [f"plugin type: {kind}",
             f"eq: {'present' if eq_present else 'absent'}",
             f"dynamics: {'present' if dynamics_present else 'absent'}"]

    if isinstance(snapshot, dict) and snapshot:
        classified = classify_plugin_type(snapshot)
        if classified["kind"] == "compressor" and not _eq_band_keys(snapshot):
            basis.append("compressor snapshot, no EQ bands; "
                         "possible dyn-only chain")

    return {"order": "unknown", "confidence": "low", "basis": basis,
            "suggested": _SUGGESTED_ORDER,
            "suggestion_note": _SUGGESTION_NOTE}


# ---------------------------------------------------------------------------
# Chain document assembly
# ---------------------------------------------------------------------------


def _why_not_spec(eq, dynamics, nonlinearity):
    """Reasons a plugin's measurements cannot yet drive a spec, in a fixed
    order: eq artifact, fit conflicts, implausible time constants, harmonic
    artifact."""
    reasons = []
    if eq["overall"] == "artifact":
        reasons.append("eq artifact")
    if dynamics["compression"]["conflict"]:
        reasons.append("compression fit conflict")
    gr = dynamics["gr"]
    if not gr["attack_plausible"]:
        reasons.append("attack implausible")
    if not gr["release_plausible"]:
        reasons.append("release implausible")
    if nonlinearity["verdict"] == "artifact":
        reasons.append("harmonic artifact")
    return reasons


def build_chain_doc(rows, meta, dataset_dir=None):
    """Assemble the full chain_doc from aggregate_report rows + enrichment.

    For each row (input order preserved): enrich_from_dataset when
    dataset_dir is given (dataset_path = dataset_dir/<slug>/dataset.json;
    a missing file is a valid state → all-None enrichment), classify the
    plugin type, build eq/dynamics/nonlinearity (the shared duplicate-
    fingerprint set is computed ONCE across all enriched rows), infer the
    processing order and decide usable_as_spec (no artifacts, no fit
    conflicts, all GR time constants plausible) with why_not_spec reasons.

    Returns {"generated_at", "source": {aggregate_report, dataset_dir,
    report_generated_at}, "plugins": [per-plugin dicts]}.
    """
    enrichments = []
    for row in rows:
        if dataset_dir is not None:
            enrichments.append(
                enrich_from_dataset(Path(dataset_dir) / row["slug"]
                                    / "dataset.json"))
        else:
            enrichments.append({"parameter_snapshot": None,
                                "harmonic_block": None, "sample_rate": None,
                                "has_compression_family": False})

    shared_groups = dq.detect_duplicate_fingerprints([
        {"slug": row["slug"], "harmonic_raw": enrichment["harmonic_block"]}
        for row, enrichment in zip(rows, enrichments)])
    shared_fingerprints = set(shared_groups)

    plugins = []
    for row, enrichment in zip(rows, enrichments):
        snapshot = enrichment["parameter_snapshot"]
        plugin_type = classify_plugin_type(snapshot, row=row)
        eq = build_eq(row.get("freq"), enrichment)
        dynamics = build_dynamics(row.get("compression"), row.get("gr"),
                                  enrichment)
        harmonic_row = row.get("harmonic")
        if isinstance(harmonic_row, dict) \
                and enrichment["harmonic_block"] is not None:
            harmonic_row = dict(harmonic_row)
            harmonic_row["harmonic_raw"] = enrichment["harmonic_block"]
        nonlinearity = build_nonlinearity(harmonic_row, shared_fingerprints)
        processing_order = infer_order(plugin_type, eq, dynamics, snapshot)
        why_not_spec = _why_not_spec(eq, dynamics, nonlinearity)
        plugins.append({
            "slug": row["slug"],
            "plugin": row["plugin"],
            "plugin_type": plugin_type,
            "eq": eq,
            "dynamics": dynamics,
            "nonlinearity": nonlinearity,
            "processing_order": processing_order,
            "usable_as_spec": not why_not_spec,
            "why_not_spec": why_not_spec,
        })

    return {"generated_at": meta["generated_at"],
            "source": {"aggregate_report": meta["aggregate_report"],
                       "dataset_dir": dataset_dir,
                       "report_generated_at": meta["report_generated_at"]},
            "plugins": plugins}


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------


def main(argv=None):
    """CLI entry: build the chain_doc from an aggregate_report.json.

    Exit codes: 0 = success; 2 = aggregate report missing/unreadable/
    unparseable (mirrors aggregate_report.py's missing-input convention).
    OSError from the output writers propagates (never swallowed). Writes
    chain_description.json (indent=2, ensure_ascii=False, trailing
    newline — mirroring write_json) and chain_description.md (utf-8).
    """
    parser = argparse.ArgumentParser(
        description="Build a processing-chain description document from "
                    "an aggregate_report.json (issue #26)")
    parser.add_argument("aggregate_report_json", metavar="aggregate_report.json",
                        help="path to the aggregate report (write_json shape: "
                             "generated_at, out_dir, tolerances, counts, "
                             "plugins)")
    parser.add_argument("--dataset-dir", metavar="DIR", default=None,
                        help="directory of per-plugin dataset.json dirs "
                             "(default: no enrichment)")
    parser.add_argument("--json", metavar="PATH",
                        default="chain_description.json",
                        help="output chain_doc JSON path "
                             "(default: chain_description.json)")
    parser.add_argument("--markdown", metavar="PATH",
                        default="chain_description.md",
                        help="output markdown path "
                             "(default: chain_description.md)")
    parser.add_argument("--meta-report-generated-at", metavar="ISO",
                        default="unknown",
                        help="report_generated_at fallback when the report "
                             "carries no generated_at (default: unknown)")
    args = parser.parse_args(argv)

    report_path = Path(args.aggregate_report_json)
    if not report_path.is_file():
        print(f"error: aggregate report not found: {args.aggregate_report_json}",
              file=sys.stderr)
        return 2
    try:
        report = json.loads(report_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        print(f"error: cannot read aggregate report "
              f"{args.aggregate_report_json}: {exc}", file=sys.stderr)
        return 2
    if not isinstance(report, dict):
        print(f"error: aggregate report {args.aggregate_report_json} is not "
              f"a JSON object", file=sys.stderr)
        return 2

    generated_at = report.get("generated_at") \
        or args.meta_report_generated_at
    meta = {"generated_at": generated_at,
            "aggregate_report": args.aggregate_report_json,
            "report_generated_at": generated_at}
    chain_doc = build_chain_doc(report.get("plugins") or [], meta,
                                dataset_dir=args.dataset_dir)

    json_out = Path(args.json)
    json_out.write_text(
        json.dumps(render_json(chain_doc), indent=2, ensure_ascii=False)
        + "\n", encoding="utf-8")
    md_out = Path(args.markdown)
    md_out.write_text(render_markdown(chain_doc), encoding="utf-8")

    print(f"plugins: {len(chain_doc['plugins'])}")
    print(f"reports: {json_out}, {md_out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
