"""stage_fusion.py — blend picked processing stages from several chain_docs.

Wave-2 task T7 of issue #105. A chain_doc (describe_chain.py) decomposes
each measured plugin into stages[] (eq / dynamics / nonlinearity), each
carrying the provenance of the measurement behind it. This tool takes
stage picks from two or more chain_doc inputs and writes ONE fused
chain_doc — "A's EQ segment + B's compression segment" — which is the
spec shape a later DSP prototype (issue #106) reads.

    stage_fusion.py chain_a.json chain_b.json \
        --pick a:eq --pick b:dynamics --out fused.json [--markdown fused.md]

Inputs are named by position: a, b, c, ... Each input must be a single
plugin's chain_doc (validated against the v3 contract first); an ambiguous
multi-plugin input is refused rather than guessed. Picks are ordered as
given and every pick must name a stage its input actually measured — a
missing segment is an error, never an invented one.

Honesty rules inherited from describe_chain:

  - processing order is never asserted (always "unknown" + the canonical
    suggestion);
  - a blocker travels with its segment: picking an artifact segment marks
    the fused document not spec-usable and says "inherited from <alias>";
  - the output is deterministic: same inputs -> byte-identical document
    (generated_at is the newest input's stamp, never the wall clock).

Stdlib only. Exit codes: 0 = success; 2 = usage or input error (nothing
is written then).

Usage (CLI):
    python tools/stage_fusion.py A.json B.json --pick a:eq --pick b:dynamics
        --out fused.json [--markdown fused.md]
"""

import argparse
import copy
import json
import sys
from pathlib import Path

from describe_render import render_json, render_markdown
from describe_schema import (CONTRACT_VERSION, STAGE_KINDS, SUGGESTED_ORDER,
                             SUGGESTION_NOTE, validate_chain_doc)

# Stage kind -> the state a fused document reports for a segment that was
# not picked (explicitly empty, never a silent zero).
_EMPTY_BLOCKS = {
    "eq": lambda: {"present": False, "overall": "none", "sections": [],
                   "notes": ["not picked in fusion"]},
    "dynamics": lambda: {
        "present": False,
        "compression": {"threshold_derived": None, "ratio_derived": None,
                        "threshold_json": None, "ratio_json": None,
                        "knee_json": None, "conflict": False,
                        "conflict_note": None},
        # No measurement behind this segment: the plausibility flags must not
        # claim anything (a False here used to render as "release implausible"),
        # and section_usable keeps the document from treating it as evidence.
        "gr": {"attack_ms": None, "release_ms": None,
               "attack_plausible": True, "release_plausible": True,
               "release_flag": None, "note": "not picked in fusion",
               "section_usable": False},
        "notes": ["not picked in fusion"]},
    "nonlinearity": lambda: {"verdict": "not-measured",
                             "thd_range_pct": None,
                             "description": None,
                             "reason": "not picked in fusion"},
}


class InputError(Exception):
    """Usage or input problem: reported on stderr, exit code 2."""


# ---------------------------------------------------------------------------
# Input loading
# ---------------------------------------------------------------------------


def _alias(index):
    """Positional input alias: a, b, c, ... (26 inputs max)."""
    if index >= 26:
        raise InputError("too many inputs (max 26: a..z)")
    return chr(ord("a") + index)


def load_input(path, alias):
    """Load and validate one chain_doc input.

    Returns {alias, path, doc, plugin}; raises InputError when the file is
    missing/unparseable, fails the v3 contract, or describes zero or more
    than one plugin.
    """
    path = Path(path)
    if not path.is_file():
        raise InputError(f"input {alias} not found: {path}")
    try:
        doc = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise InputError(f"input {alias} is not readable JSON ({path}): "
                         f"{exc}")

    result = validate_chain_doc(doc)
    if not result["ok"]:
        first = "; ".join(result["errors"][:3])
        raise InputError(f"input {alias} is not a valid chain_doc "
                         f"({path}): {first}")

    plugins = doc.get("plugins") or []
    if len(plugins) != 1:
        raise InputError(f"input {alias} describes {len(plugins)} plugins; "
                         f"stage fusion requires exactly one plugin per input")
    return {"alias": alias, "path": str(path), "doc": doc,
            "plugin": plugins[0]}


def parse_pick(text, entries):
    """Validate one --pick alias:stage against the loaded inputs."""
    alias, _, stage = text.partition(":")
    known = {entry["alias"] for entry in entries}
    if not _ or not stage:
        raise InputError(f"pick {text!r} is not <alias>:<stage>")
    if alias not in known:
        raise InputError(f"pick {text!r} names no input (known: "
                         f"{', '.join(sorted(known))})")
    if stage not in STAGE_KINDS:
        raise InputError(f"pick {text!r}: unknown stage {stage!r} (known: "
                         f"{', '.join(sorted(STAGE_KINDS))})")
    entry = next(item for item in entries if item["alias"] == alias)
    stages = entry["plugin"].get("stages") or []
    if not any(item.get("kind") == stage for item in stages):
        raise InputError(f"pick {text!r}: input {alias} has no '{stage}' "
                         f"stage (measured: "
                         f"{', '.join(str(s.get('kind')) for s in stages) or 'none'})")
    return alias, stage


# ---------------------------------------------------------------------------
# Fusion
# ---------------------------------------------------------------------------


def _display_path(path):
    """Path as written when possible: never leak the absolute local layout."""
    resolved = Path(path).resolve()
    try:
        return str(resolved.relative_to(Path.cwd().resolve()))
    except ValueError:
        return str(path)


def _picked_stages(entries, picks):
    """Resolve picks into [(alias, stage, stage_copy)] in pick order."""
    by_alias = {entry["alias"]: entry for entry in entries}
    resolved = []
    for alias, stage in picks:
        entry = by_alias[alias]
        stage_doc = next(item for item in entry["plugin"]["stages"]
                         if item.get("kind") == stage)
        stage_copy = copy.deepcopy(stage_doc)
        # The provenance block is guaranteed by the v3 contract for stages that
        # describe_chain wrote; a hand-written document may omit it, so keep a
        # safe default instead of raising KeyError.
        provenance = stage_copy.setdefault("provenance",
                                           {"source": "unknown", "basis": []})
        provenance["input"] = {"alias": alias,
                               "slug": entry["plugin"]["slug"],
                               "path": _display_path(entry["path"])}
        resolved.append((alias, stage, stage_copy))
    return resolved


def _blockers(resolved):
    """Blockers inherited from the picked segments, in pick order.

    Read from the stage's own blockers[] (written by describe_chain from the
    plugin verdict) — never re-derived from params, which used to silently drop
    tau verdicts and the not-exercised verdict (issue #105 review, finding 1).
    """
    reasons = []
    for alias, _, stage_doc in resolved:
        for blocker in stage_doc.get("blockers") or []:
            reason = f"inherited from {alias}: {blocker}"
            if reason not in reasons:
                reasons.append(reason)
    return reasons


def fuse_chain_docs(entries, picks):
    """Blend the picked stages of the loaded inputs into one chain_doc.

    entries: [load_input(...)] in input order; picks: [(alias, stage)] in
    pick order (already validated). Returns the fused v3 document.
    """
    resolved = _picked_stages(entries, picks)
    picked = {stage for _, stage, _ in resolved}
    by_alias = {entry["alias"]: entry for entry in entries}

    blocks = {}
    for stage in ("eq", "dynamics", "nonlinearity"):
        if stage in picked:
            source = next(entry for entry in (
                by_alias[alias]["plugin"] for alias, picked_stage, _
                in resolved if picked_stage == stage))
            blocks[stage] = copy.deepcopy(source[stage])
        else:
            blocks[stage] = _EMPTY_BLOCKS[stage]()

    why_not_spec = _blockers(resolved)
    slug = "fused-" + "-".join(f"{alias}-{stage}" for alias, stage, _
                               in resolved)
    plugin_name = "Fused " + " + ".join(
        f"{by_alias[alias]['plugin']['plugin']}[{stage}]"
        for alias, stage, _ in resolved)
    generated_at = max(
        (entry["doc"].get("generated_at") or "unknown"
         for entry in entries)) or "unknown"

    return {
        "contract_version": CONTRACT_VERSION,
        "generated_at": generated_at,
        "source": {"aggregate_report": "stage_fusion.py",
                   "dataset_dir": None,
                   "report_generated_at": generated_at},
        "plugins": [{
            "slug": slug,
            "plugin": plugin_name,
            "plugin_type": {"kind": "processor", "confidence": "low",
                            "basis": ["fused document",
                                      f"{len(resolved)} stage(s) picked from "
                                      f"{len(entries)} input(s)"]},
            "eq": blocks["eq"],
            "dynamics": blocks["dynamics"],
            "nonlinearity": blocks["nonlinearity"],
            "processing_order": {"order": "unknown", "confidence": "low",
                                 "basis": ["fused document: processing order "
                                           "is never asserted from a fusion"],
                                 "suggested": SUGGESTED_ORDER,
                                 "suggestion_note": SUGGESTION_NOTE},
            "stages": [stage_doc for _, _, stage_doc in resolved],
            "usable_as_spec": not why_not_spec,
            "why_not_spec": why_not_spec,
        }],
        "fusion": {
            "inputs": [{"alias": entry["alias"],
                        "path": _display_path(entry["path"]),
                        "slug": entry["plugin"]["slug"],
                        "plugin": entry["plugin"]["plugin"]}
                       for entry in entries],
            "picks": [{"alias": alias, "stage": stage}
                      for alias, stage in picks],
        },
    }


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------


def main(argv=None):
    """CLI entry. Exit 0 = fused document written; 2 = usage/input error."""
    parser = argparse.ArgumentParser(
        description="Blend picked processing stages from several chain_doc "
                    "inputs into one fused chain_doc (issue #105)")
    parser.add_argument("inputs", nargs="+", metavar="chain_doc.json",
                        help="one chain_doc per input, named a, b, c, ... "
                             "in the order given")
    parser.add_argument("--pick", action="append", default=[],
                        metavar="ALIAS:STAGE",
                        help="stage to take from an input, e.g. a:eq "
                             "(repeatable; order is preserved)")
    parser.add_argument("--out", metavar="PATH", default="fused_chain.json",
                        help="fused chain_doc JSON path "
                             "(default: fused_chain.json)")
    parser.add_argument("--markdown", metavar="PATH", default=None,
                        help="optional markdown report path")
    args = parser.parse_args(argv)

    try:
        if not args.pick:
            raise InputError("nothing to fuse: pass at least one --pick "
                             "ALIAS:STAGE")
        entries = [load_input(path, _alias(index))
                   for index, path in enumerate(args.inputs)]
        picks = []
        for text in args.pick:
            pick = parse_pick(text, entries)
            if pick in picks:
                raise InputError(f"duplicate pick {text!r}")
            # One stage per segment kind: two picks of the same kind would put
            # two summaries in stages[] but only one source block in the doc.
            if any(stage == pick[1] for _, stage in picks):
                raise InputError(f"duplicate stage {pick[1]!r} (got {text!r}): "
                                 f"pick one stage per segment kind")
            picks.append(pick)
        fused = fuse_chain_docs(entries, picks)
        result = validate_chain_doc(fused)
        if not result["ok"]:
            raise InputError("fused document failed the v3 contract: "
                             + "; ".join(result["errors"][:3]))
    except InputError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2

    out_path = Path(args.out)
    markdown_path = Path(args.markdown) if args.markdown else None
    overwritten = out_path.exists()

    # Both outputs are rendered first, written inside the error contract: a
    # failed write is a usage/IO error (exit 2, no traceback, no half-written
    # pair). Markdown goes first so a JSON file always has its report beside it.
    try:
        payload = json.dumps(render_json(fused), indent=2,
                             ensure_ascii=False) + "\n"
        report = render_markdown(fused) if markdown_path is not None else None
        if markdown_path is not None:
            markdown_path.write_text(report, encoding="utf-8")
        out_path.write_text(payload, encoding="utf-8")
    except OSError as exc:
        print(f"error: cannot write output ({exc})", file=sys.stderr)
        return 2

    print("stages: " + ", ".join(f"{alias}:{stage}" for alias, stage in picks))
    print(f"spec-usable: "
          f"{'yes' if fused['plugins'][0]['usable_as_spec'] else 'no'}")
    print(f"reports: {out_path}"
          + (f", {markdown_path}" if markdown_path is not None else "")
          + (" (overwrote existing)" if overwritten else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main())
