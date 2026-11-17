"""describe_schema.py — chain_doc contract constants + structural validator.

Wave-2 Task T5 of issue #26. Two jobs:

  1. Single source of truth for the chain-description document contract:
     CONTRACT_VERSION (the anchor the T4 consumer keys on), the vocabulary
     sets (verdicts, plugin kinds, confidence) and the shared measurement
     thresholds (imported from describe_quality.py so downstream consumers
     never redefine them).
  2. `validate_chain_doc`, a pure structural validator for the full
     chain_doc shape built by describe_render.py / describe_chain.py:

        {contract_version: "3", generated_at,
         source:{aggregate_report, dataset_dir, report_generated_at},
         fusion?:{inputs:[{alias, path, slug, plugin}],
                  picks:[{alias, stage}]},
         plugins:[{slug, plugin,
                   plugin_type:{kind,confidence,basis[]},
                   eq:{present, overall, sections[], notes[]},
                   dynamics:{present, compression:{...}, gr:{...},
                             notes[]},
                   nonlinearity:{verdict, ...},
                   processing_order:{order, confidence, basis[], ...},
                   stages:[{kind, params, provenance:{source, basis[]}}],
                   usable_as_spec, why_not_spec[]}]}

v3 (issue #105) added the per-plugin `stages[]` decomposition (eq /
dynamics / nonlinearity, each with provenance) and the optional
top-level `fusion` block that stage_fusion.py writes. A document without
`contract_version` is validated under the v2 rules, so pre-v3 artifacts
keep validating; an unknown version is a single deterministic error.

Validation is structural only — presence, type and vocabulary of every
contract field — never a plausibility judgement (that is
describe_quality's job). It never raises: malformed input (missing keys,
None in required slots, wrong types, out-of-vocab values) is collected
into deterministic error strings. Optional fields accept None and are
only checked when present; extra keys are ignored.

Stdlib only.

Usage:
    import describe_schema as ds
    result = ds.validate_chain_doc(chain_doc)   # {"ok": bool, "errors": [str]}
"""

from describe_quality import (SANE_Q_MIN, SANE_Q_MAX, TAU_MIN_MS,
                              TAU_MAX_MS, THD_CLEAN_MAX_PCT)

# ---------------------------------------------------------------------------
# Contract constants (issue #26 chain_doc)
# ---------------------------------------------------------------------------

# Anchor for the T4 consumer: bump whenever the chain_doc contract changes.
# v3 (issue #105): top-level contract_version + per-plugin stages[] (+ the
# optional top-level fusion block written by stage_fusion.py).
CONTRACT_VERSION = "3"

# Versions this validator understands. A document without a
# contract_version is a pre-v3 (v2) document — validated under v2 rules so
# existing artifacts keep validating. Anything else is a deterministic
# error, never a silent pass.
KNOWN_CONTRACT_VERSIONS = ("2", "3")

VERDICT_OVERALL = {"none", "clean", "artifact"}
VERDICT_NL = {"not-measured", "clean", "artifact"}
KIND_VOCAB = {"analyzer", "compressor", "dynamics-only", "eq-dynamics",
              "eq-only", "processor", "unknown"}
CONFIDENCE = {"high", "low"}

# v3 stage vocabulary: the processing segments a plugin can be decomposed
# into (issue #105). A fused spec picks one stage per alias.
STAGE_KINDS = {"eq", "dynamics", "nonlinearity"}

# Canonical, never-asserted processing order suggestion (eq -> dyn -> eq).
# Shared by describe_chain (document) and stage_fusion (fused document) so the
# two can never drift apart.
SUGGESTED_ORDER = "eq -> dyn -> eq"
SUGGESTION_NOTE = "canonical, not measured"

# ---------------------------------------------------------------------------
# validate_chain_doc
# ---------------------------------------------------------------------------

_MISSING = object()


def _is_num(value):
    """Number check for float|None slots: accepts int, rejects bool."""
    return isinstance(value, (int, float)) and not isinstance(value, bool)


def _ok(value, kind):
    """Type predicate for the 'kind' tags used by _required/_optional."""
    if kind == "str":
        return isinstance(value, str)
    if kind == "bool":
        return isinstance(value, bool)
    if kind == "list":
        return isinstance(value, list)
    if kind == "dict":
        return isinstance(value, dict)
    if kind == "num":
        return _is_num(value)
    raise ValueError(f"unknown kind {kind!r}")


def _fmt_vocab(vocab):
    """Deterministic vocab rendering (set iteration order is not stable)."""
    return "{" + ", ".join(sorted(vocab)) + "}"


def _required(errors, path, d, key, kind, field=None, none_ok=False):
    """One required-key check: missing -> "missing key", wrong type -> error.
    none_ok=True allows None in the slot (float|None, str|None)."""
    name = field if field is not None else key
    if key not in d:
        errors.append(f"{path}: missing key '{name}'")
        return
    value = d[key]
    if value is None:
        if not none_ok:
            errors.append(f"{path}: {name} must be {kind}")
        return
    if not _ok(value, kind):
        errors.append(f"{path}: {name} must be {kind}")


def _optional(errors, path, d, key, kind, field=None):
    """Optional field: absent or None is fine; wrong type is an error."""
    name = field if field is not None else key
    value = d.get(key, _MISSING)
    if value is _MISSING or value is None:
        return
    if not _ok(value, kind):
        errors.append(f"{path}: {name} must be {kind}")


def _vocab_check(errors, path, d, key, vocab, field=None, label=None):
    """Required vocab member: missing / non-str / out-of-vocab -> error."""
    name = field if field is not None else key
    if key not in d:
        errors.append(f"{path}: missing key '{name}'")
        return
    value = d[key]
    if value is None or not isinstance(value, str) or value not in vocab:
        errors.append(f"{path}: {name} {value!r} not in "
                      f"{label if label is not None else _fmt_vocab(vocab)}")


def _check_eq_section(errors, path, section):
    if not isinstance(section, dict):
        errors.append(f"{path}: must be a dict")
        return
    _required(errors, path, section, "freq_hz", "num", none_ok=True)
    _required(errors, path, section, "gain_db", "num", none_ok=True)
    _required(errors, path, section, "q", "num", none_ok=True)
    _required(errors, path, section, "plausible", "bool")
    _optional(errors, path, section, "flag", "str")
    _optional(errors, path, section, "reason", "str")


def _check_stage(errors, path, stage, idx):
    """One v3 stage: {kind, params, provenance:{source, basis[]}}."""
    if not isinstance(stage, dict):
        errors.append(f"{path}: stages[{idx}] must be a dict")
        return
    _vocab_check(errors, path, stage, "kind", STAGE_KINDS,
                 field=f"stages[{idx}].kind")
    _required(errors, path, stage, "params", "dict",
              field=f"stages[{idx}].params")
    _required(errors, path, stage, "provenance", "dict",
              field=f"stages[{idx}].provenance")
    provenance = stage.get("provenance")
    if isinstance(provenance, dict):
        _required(errors, path, provenance, "source", "str",
                  field=f"stages[{idx}].provenance.source")
        _required(errors, path, provenance, "basis", "list",
                  field=f"stages[{idx}].provenance.basis")
    _required(errors, path, stage, "blockers", "list",
              field=f"stages[{idx}].blockers")
    _required(errors, path, stage, "usable", "bool",
              field=f"stages[{idx}].usable")


def _check_fusion(errors, path, fusion):
    """Optional v3 fusion block: {inputs:[{alias,...}], picks:[{alias,stage}]}."""
    _required(errors, path, fusion, "inputs", "list", field="fusion.inputs")
    inputs = fusion.get("inputs")
    if isinstance(inputs, list):
        for idx, item in enumerate(inputs):
            if not isinstance(item, dict):
                errors.append(f"{path}: fusion.inputs[{idx}] must be a dict")
                continue
            _required(errors, path, item, "alias", "str",
                      field=f"fusion.inputs[{idx}].alias")
            _optional(errors, path, item, "path", "str",
                      field=f"fusion.inputs[{idx}].path")
            _optional(errors, path, item, "slug", "str",
                      field=f"fusion.inputs[{idx}].slug")
            _optional(errors, path, item, "plugin", "str",
                      field=f"fusion.inputs[{idx}].plugin")
    aliases = set()
    if isinstance(inputs, list):
        aliases = {item.get("alias") for item in inputs
                   if isinstance(item, dict) and isinstance(item.get("alias"),
                                                            str)}
    _required(errors, path, fusion, "picks", "list", field="fusion.picks")
    picks = fusion.get("picks")
    if isinstance(picks, list):
        for idx, item in enumerate(picks):
            if not isinstance(item, dict):
                errors.append(f"{path}: fusion.picks[{idx}] must be a dict")
                continue
            _required(errors, path, item, "alias", "str",
                      field=f"fusion.picks[{idx}].alias")
            _vocab_check(errors, path, item, "stage", STAGE_KINDS,
                         field=f"fusion.picks[{idx}].stage")
            alias = item.get("alias")
            # A pick must name one of the declared inputs (dangling aliases
            # would make the fusion block unreadable as evidence).
            if isinstance(alias, str) and aliases and alias not in aliases:
                errors.append(f"{path}: fusion.picks[{idx}].alias {alias!r} "
                              f"names no fusion input")


def _check_plugin(errors, path, plugin, version="2"):
    if not isinstance(plugin, dict):
        errors.append(f"{path}: must be a dict")
        return

    _required(errors, path, plugin, "slug", "str")
    _required(errors, path, plugin, "plugin", "str")

    # plugin_type --------------------------------------------------------
    _required(errors, path, plugin, "plugin_type", "dict")
    if isinstance(plugin.get("plugin_type"), dict):
        pt = plugin["plugin_type"]
        _vocab_check(errors, path, pt, "kind", KIND_VOCAB,
                     field="plugin_type.kind", label="vocab")
        _vocab_check(errors, path, pt, "confidence", CONFIDENCE,
                     field="plugin_type.confidence")
        _required(errors, path, pt, "basis", "list",
                  field="plugin_type.basis")

    # eq ------------------------------------------------------------------
    _required(errors, path, plugin, "eq", "dict")
    if isinstance(plugin.get("eq"), dict):
        eq = plugin["eq"]
        _required(errors, path, eq, "present", "bool", field="eq.present")
        _vocab_check(errors, path, eq, "overall", VERDICT_OVERALL,
                     field="eq.overall")
        _required(errors, path, eq, "sections", "list", field="eq.sections")
        sections = eq.get("sections")
        if isinstance(sections, list):
            for idx, section in enumerate(sections):
                _check_eq_section(errors, f"{path}: eq.sections[{idx}]",
                                  section)
        _required(errors, path, eq, "notes", "list", field="eq.notes")

    # dynamics --------------------------------------------------------------
    _required(errors, path, plugin, "dynamics", "dict")
    if isinstance(plugin.get("dynamics"), dict):
        dyn = plugin["dynamics"]
        _required(errors, path, dyn, "present", "bool",
                  field="dynamics.present")
        _required(errors, path, dyn, "compression", "dict",
                  field="dynamics.compression")
        if isinstance(dyn.get("compression"), dict):
            comp = dyn["compression"]
            _required(errors, path, comp, "threshold_derived", "num",
                      field="dynamics.compression.threshold_derived",
                      none_ok=True)
            _required(errors, path, comp, "ratio_derived", "num",
                      field="dynamics.compression.ratio_derived",
                      none_ok=True)
            _optional(errors, path, comp, "threshold_json", "num",
                      field="dynamics.compression.threshold_json")
            _optional(errors, path, comp, "ratio_json", "num",
                      field="dynamics.compression.ratio_json")
            _optional(errors, path, comp, "knee_json", "num",
                      field="dynamics.compression.knee_json")
            _required(errors, path, comp, "conflict", "bool",
                      field="dynamics.compression.conflict")
            _optional(errors, path, comp, "conflict_note", "str",
                      field="dynamics.compression.conflict_note")
        _required(errors, path, dyn, "gr", "dict", field="dynamics.gr")
        if isinstance(dyn.get("gr"), dict):
            gr = dyn["gr"]
            _required(errors, path, gr, "attack_ms", "num",
                      field="dynamics.gr.attack_ms", none_ok=True)
            _required(errors, path, gr, "release_ms", "num",
                      field="dynamics.gr.release_ms", none_ok=True)
            _required(errors, path, gr, "attack_plausible", "bool",
                      field="dynamics.gr.attack_plausible")
            _required(errors, path, gr, "release_plausible", "bool",
                      field="dynamics.gr.release_plausible")
            _optional(errors, path, gr, "release_flag", "str",
                      field="dynamics.gr.release_flag")
            _optional(errors, path, gr, "note", "str",
                      field="dynamics.gr.note")
        _required(errors, path, dyn, "notes", "list", field="dynamics.notes")

    # nonlinearity ----------------------------------------------------------
    _required(errors, path, plugin, "nonlinearity", "dict")
    if isinstance(plugin.get("nonlinearity"), dict):
        nl = plugin["nonlinearity"]
        _vocab_check(errors, path, nl, "verdict", VERDICT_NL,
                     field="nonlinearity.verdict")
        range_pct = nl.get("thd_range_pct", _MISSING)
        if range_pct is not _MISSING and range_pct is not None:
            if (not isinstance(range_pct, list) or len(range_pct) != 2
                    or not all(_is_num(v) for v in range_pct)):
                errors.append(f"{path}: nonlinearity.thd_range_pct "
                              f"must be [float, float]")
        _optional(errors, path, nl, "description", "str",
                  field="nonlinearity.description")
        _optional(errors, path, nl, "reason", "str",
                  field="nonlinearity.reason")

    # processing_order -------------------------------------------------------
    _required(errors, path, plugin, "processing_order", "dict")
    if isinstance(plugin.get("processing_order"), dict):
        po = plugin["processing_order"]
        _required(errors, path, po, "order", "str",
                  field="processing_order.order")
        _vocab_check(errors, path, po, "confidence", CONFIDENCE,
                     field="processing_order.confidence")
        _required(errors, path, po, "basis", "list",
                  field="processing_order.basis")
        _optional(errors, path, po, "suggested", "str",
                  field="processing_order.suggested")
        _optional(errors, path, po, "suggestion_note", "str",
                  field="processing_order.suggestion_note")

    _required(errors, path, plugin, "usable_as_spec", "bool")
    _required(errors, path, plugin, "why_not_spec", "list")

    # stages ---------------------------------------------------------------
    # Required in v3, optional in v2 — but whenever present it is validated
    # (backward compatibility means "may be absent", not "may be junk").
    if version == "3":
        _required(errors, path, plugin, "stages", "list")
    stages = plugin.get("stages", _MISSING)
    if stages is not _MISSING and stages is not None:
        if not isinstance(stages, list):
            errors.append(f"{path}: stages must be list")
        elif not stages:
            # A plugin whose measurements yielded no usable segment
            # legitimately carries stages [] — but it must not claim to
            # be a usable spec then (issue #105 contract invariant).
            if plugin.get("usable_as_spec") is True:
                errors.append(f"{path}: stages must not be empty when "
                              f"usable_as_spec is true")
        else:
            seen = []
            for idx, stage in enumerate(stages):
                _check_stage(errors, path, stage, idx)
                kind = stage.get("kind") if isinstance(stage, dict) else None
                if kind in seen:
                    # One segment per kind: two "eq" stages would describe two
                    # different bands while the eq block holds only one.
                    errors.append(f"{path}: stages[{idx}].kind {kind!r} "
                                  f"duplicated (one stage per segment kind)")
                seen.append(kind)


def validate_chain_doc(doc):
    """Validate a chain_doc against the issue #26 contract.

    Structural only: presence, type and vocabulary of every contract
    field, recursively. Never raises — malformed input (non-dict doc,
    missing keys, None in required slots, wrong types, out-of-vocab
    values) is reported as deterministic error strings. Optional fields
    accept None and are only checked when present; extra keys are ignored.

    Returns {"ok": bool, "errors": [str]}.
    """
    errors = []

    if not isinstance(doc, dict):
        return {"ok": False, "errors": ["doc must be a dict"]}

    # -- contract version gate ------------------------------------------
    # A missing contract_version is a pre-v3 document (v2 rules). An
    # unsupported one is reported once, deterministically: validating a
    # shape we do not know would only produce noise.
    raw_version = doc.get("contract_version", _MISSING)
    if raw_version is _MISSING or raw_version is None:
        version = "2"
    elif isinstance(raw_version, str) and raw_version in KNOWN_CONTRACT_VERSIONS:
        version = raw_version
    else:
        return {"ok": False,
                "errors": [f"doc: contract_version {raw_version!r} "
                           f"unsupported (known: "
                           f"{', '.join(KNOWN_CONTRACT_VERSIONS)})"]}

    # -- top level ------------------------------------------------------
    _required(errors, "doc", doc, "generated_at", "str")
    _required(errors, "doc", doc, "source", "dict")
    if isinstance(doc.get("source"), dict):
        src = doc["source"]
        _required(errors, "source", src, "aggregate_report", "str")
        _required(errors, "source", src, "dataset_dir", "str", none_ok=True)
        _required(errors, "source", src, "report_generated_at", "str")

    _required(errors, "doc", doc, "plugins", "list")
    plugins = doc.get("plugins")
    if isinstance(plugins, list):
        if not plugins:
            errors.append("doc: plugins must not be empty")
        else:
            for idx, plugin in enumerate(plugins):
                _check_plugin(errors, f"plugins[{idx}]", plugin, version)

    # -- optional fusion block (v3, stage_fusion.py output) --------------
    fusion = doc.get("fusion", _MISSING)
    if fusion is not _MISSING and fusion is not None:
        if not isinstance(fusion, dict):
            errors.append("doc: fusion must be dict")
        else:
            _check_fusion(errors, "doc", fusion)

    return {"ok": not errors, "errors": errors}
