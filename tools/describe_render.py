"""describe_render.py — chain_doc JSON/markdown renderers.

Wave-1 Task T3 (ticket #26): pure formatters over the processing-chain
description document produced by describe_chain. No knowledge of how the
chain_doc was derived — only the pinned contract shape is consumed.

render_json returns a JSON-canonicalized deep copy: key insertion order is
preserved (deterministic for json.dumps), floats are rounded to 4 decimal
places to suppress float noise (39676.69 stays 39676.69; None stays None),
tuples are normalized to lists. The result round-trips through
json.dumps + json.loads preserving every contract field and type
(str/int/float/bool/None/list/dict).

render_markdown returns a per-plugin human verdict report with stable line
order: ## <slug> (<plugin>) header, Plugin/Type lines, then EQ, Dynamics
(compression + GR), Nonlinearity, Order and Spec-usable verdicts. Unknown
or None fields render as "n/a". Deterministic: the same chain_doc input
reproduces byte-identical output.

Stdlib only, no third-party dependencies.

Usage (import only):
    from describe_render import render_json, render_markdown
"""

# ---------------------------------------------------------------------------
# Value formatting helpers
# ---------------------------------------------------------------------------


def _fmt(value):
    """JSON canonicalization for scalars: floats rounded to 4dp (kept as
    floats, e.g. 39676.69 stays; 50.0 stays 50.0), None preserved,
    everything else (str/int/bool) passed through unchanged."""
    if isinstance(value, float):
        return round(value, 4)
    return value


def _fmt_text(value):
    """Markdown display of a scalar: None -> "n/a", floats rounded to 4dp
    (str of the rounded value: 39676.69 stays, no trailing-zero noise),
    everything else stringified."""
    if value is None:
        return "n/a"
    if isinstance(value, float):
        return str(round(value, 4))
    return str(value)


# ---------------------------------------------------------------------------
# JSON canonicalization
# ---------------------------------------------------------------------------


def _canon(value):
    """Deep canonical copy: dict key order preserved, tuples -> lists,
    floats rounded via _fmt."""
    if isinstance(value, dict):
        return {key: _canon(child) for key, child in value.items()}
    if isinstance(value, list):
        return [_canon(child) for child in value]
    if isinstance(value, tuple):
        return [_canon(child) for child in value]
    return _fmt(value)


def render_json(chain_doc):
    """Return a JSON-canonicalized deep copy of chain_doc.

    Deterministic key ordering (insertion order preserved from the input),
    floats rounded to 4dp (no trailing-zero noise), None preserved. The
    result round-trips through json.dumps + json.loads preserving all
    contract fields and their types.
    """
    return _canon(chain_doc)


# ---------------------------------------------------------------------------
# Markdown rendering
# ---------------------------------------------------------------------------


def _eq_line(eq):
    """EQ verdict line (plus per-section bullets for the clean case)."""
    eq = eq or {}
    overall = eq.get("overall")
    sections = eq.get("sections") or []
    if overall == "clean":
        lines = ["EQ: {} plausible section(s)".format(len(sections))]
        for section in sections:
            lines.append(
                "- freq {} Hz / gain {} dB / Q {}".format(
                    _fmt_text(section.get("freq_hz")),
                    _fmt_text(section.get("gain_db")),
                    _fmt_text(section.get("q")),
                )
            )
        return "\n".join(lines)
    if overall == "artifact":
        bad = next((s for s in sections if not s.get("plausible", True)), None)
        if bad is not None:
            reason = bad.get("reason") or bad.get("flag") or "implausible"
        else:
            reason = "implausible"
        return "EQ: {} section(s), but {} -> artifact".format(len(sections), reason)
    if overall == "none":
        return "EQ: none"
    return "EQ: n/a"


def _compression_line(dynamics):
    """Compression verdict: single derived fit, or both fits on conflict."""
    compression = (dynamics or {}).get("compression")
    if not compression:
        return "Compression: n/a"
    line = "Compression: threshold {} dB / ratio {}".format(
        _fmt_text(compression.get("threshold_derived")),
        _fmt_text(compression.get("ratio_derived")),
    )
    if compression.get("conflict"):
        line += " (derived) vs {}/{} (doc fit) — conflict".format(
            _fmt_text(compression.get("threshold_json")),
            _fmt_text(compression.get("ratio_json")),
        )
    return line


def _gr_line(dynamics):
    """GR verdict: attack/release, flagged when release is implausible."""
    gr = (dynamics or {}).get("gr")
    if not gr:
        return "GR: n/a"
    line = "GR: attack {} ms / release {} ms".format(
        _fmt_text(gr.get("attack_ms")),
        _fmt_text(gr.get("release_ms")),
    )
    if gr.get("release_plausible") is False:
        line += " — release implausible"
    return line


def _nonlinearity_line(nonlinearity):
    """Nonlinearity verdict: THD range, artifact reason, or not measured."""
    nonlinearity = nonlinearity or {}
    verdict = nonlinearity.get("verdict")
    if verdict == "clean":
        thd_range = nonlinearity.get("thd_range_pct")
        if isinstance(thd_range, (list, tuple)) and len(thd_range) == 2:
            return "Nonlinearity: THD {}-{}%".format(
                _fmt_text(thd_range[0]), _fmt_text(thd_range[1]))
        return "Nonlinearity: THD n/a"
    if verdict == "artifact":
        return "Nonlinearity: not measured (artifact: {})".format(
            _fmt_text(nonlinearity.get("reason")))
    return "Nonlinearity: not measured"


def _order_line(processing_order):
    """Processing order verdict; unknown order carries the canonical
    suggestion."""
    order = (processing_order or {}).get("order")
    if not order or order == "unknown":
        return "Order: unknown (suggestion: eq -> dyn -> eq — canonical, not measured)"
    return "Order: {} (confidence {})".format(
        _fmt_text(order), _fmt_text((processing_order or {}).get("confidence")))


def _spec_line(plugin):
    """Spec-usable verdict with comma-joined why_not_spec reasons."""
    usable = plugin.get("usable_as_spec")
    if usable is True:
        return "Spec-usable: yes"
    if usable is False:
        reasons = plugin.get("why_not_spec") or []
        if reasons:
            return "Spec-usable: no ({})".format(", ".join(_fmt_text(r) for r in reasons))
        return "Spec-usable: no"
    return "Spec-usable: n/a"


def _render_plugin(plugin):
    """One markdown section per plugin, fixed line order."""
    plugin_name = _fmt_text(plugin.get("plugin"))
    plugin_type = plugin.get("plugin_type") or {}
    lines = [
        "## {} ({})".format(_fmt_text(plugin.get("slug")), plugin_name),
        "Plugin: {}".format(plugin_name),
        "Type: {} (confidence {})".format(
            _fmt_text(plugin_type.get("kind")),
            _fmt_text(plugin_type.get("confidence"))),
        _eq_line(plugin.get("eq")),
        _compression_line(plugin.get("dynamics")),
        _gr_line(plugin.get("dynamics")),
        _nonlinearity_line(plugin.get("nonlinearity")),
        _order_line(plugin.get("processing_order")),
        _spec_line(plugin),
    ]
    return "\n".join(lines)


def render_markdown(chain_doc):
    """Return a deterministic per-plugin markdown report.

    Plugins are rendered in the order they appear in chain_doc["plugins"];
    each block is ## <slug> (<plugin>) followed by fixed-order verdict
    lines. Unknown/None fields render as "n/a".
    """
    return "\n\n".join(_render_plugin(plugin) for plugin in chain_doc["plugins"]) + "\n"
