"""Tests for describe_render.py — chain_doc JSON/markdown renderers.

Wave-1 Task T3 (ticket #26): pure formatters over the processing-chain
description document. Fixtures are INLINE chain_doc literals matching the
pinned contract (no imports from other agents' modules).

Usage:
    python -m pytest tools/test_describe_render.py -q
"""
import json

from describe_render import render_json, render_markdown

# ---------------------------------------------------------------------------
# Inline chain_doc fixtures (pinned contract shape)
# ---------------------------------------------------------------------------

# Clean plugin: EQ clean with 2 plausible sections, compression without
# conflict, plausible GR, clean nonlinearity, unknown order, spec-usable.
CLEAN_DOC = {
    "generated_at": "2026-08-13T10:00:00Z",
    "source": {
        "aggregate_report": "out/aggregate_report.md",
        "dataset_dir": "datasets/pro-c-2",
        "report_generated_at": "2026-08-13T09:00:00Z",
    },
    "plugins": [
        {
            "slug": "pro-c-2",
            "plugin": "Pro-C 2",
            "plugin_type": {
                "kind": "compressor",
                "confidence": 0.95,
                "basis": ["compression", "gr_timeline"],
            },
            "eq": {
                "present": True,
                "overall": "clean",
                "sections": [
                    {"freq_hz": 1000.0, "gain_db": 3.0, "q": 1.2,
                     "plausible": True},
                    {"freq_hz": 4000.0, "gain_db": 2.5, "q": 1.8,
                     "plausible": True},
                ],
                "notes": [],
            },
            "dynamics": {
                "present": True,
                "compression": {
                    "threshold_derived": -18.5,
                    "ratio_derived": 4.0,
                    "threshold_json": -18.4,
                    "ratio_json": 4.1,
                    "knee_json": 6.0,
                    "conflict": False,
                },
                "gr": {
                    "attack_ms": 50.0,
                    "release_ms": 250.0,
                    "attack_plausible": True,
                    "release_plausible": True,
                },
                "notes": [],
            },
            "nonlinearity": {
                "verdict": "clean",
                "thd_range_pct": [0.1, 0.3],
                "description": "low THD across sweep",
            },
            "processing_order": {
                "order": "unknown",
                "confidence": 0.6,
                "basis": ["eq_sweep", "compression"],
            },
            "usable_as_spec": True,
            "why_not_spec": [],
        }
    ],
}

# Artifact plugin (pro-c-3 style): implausible EQ Q, compression conflict,
# implausible long GR release, nonlinearity artifact, unknown order,
# not spec-usable.
ARTIFACT_DOC = {
    "generated_at": "2026-08-13T10:00:00Z",
    "source": {
        "aggregate_report": "out/aggregate_report.md",
        "dataset_dir": "datasets/pro-c-3",
        "report_generated_at": "2026-08-13T09:00:00Z",
    },
    "plugins": [
        {
            "slug": "pro-c-3",
            "plugin": "Pro-C 3",
            "plugin_type": {
                "kind": "compressor",
                "confidence": 0.7,
                "basis": ["compression", "gr_timeline"],
            },
            "eq": {
                "present": True,
                "overall": "artifact",
                "sections": [
                    {"freq_hz": 18098.0, "gain_db": 6.0, "q": 18098.0,
                     "plausible": False, "flag": "implausible",
                     "reason": "Q=18098 implausible"},
                ],
                "notes": ["Q far outside typical EQ band"],
            },
            "dynamics": {
                "present": True,
                "compression": {
                    "threshold_derived": -12.0,
                    "ratio_derived": 2.5,
                    "threshold_json": -20.0,
                    "ratio_json": 8.0,
                    "knee_json": None,
                    "conflict": True,
                    "conflict_note": "derived ratio vs doc fit diverges",
                },
                "gr": {
                    "attack_ms": 5.0,
                    "release_ms": 39676.69,
                    "attack_plausible": True,
                    "release_plausible": False,
                    "release_flag": "implausible",
                    "note": "release longer than measurement window",
                },
                "notes": [],
            },
            "nonlinearity": {
                "verdict": "artifact",
                "reason": "harmonic tones overlapped",
            },
            "processing_order": {
                "order": "unknown",
                "confidence": 0.6,
                "basis": ["eq_sweep", "compression"],
                "suggested": None,
                "suggestion_note": "cannot separate eq/dyn placement",
            },
            "usable_as_spec": False,
            "why_not_spec": [
                "eq artifact",
                "compression conflict",
                "release implausible",
                "nonlinearity artifact",
            ],
        }
    ],
}

# Degenerate plugin: no EQ activity at all.
DEGENERATE_DOC = {
    "generated_at": "2026-08-13T10:00:00Z",
    "source": {
        "aggregate_report": "out/aggregate_report.md",
        "dataset_dir": "datasets/degenerate-1",
        "report_generated_at": "2026-08-13T09:00:00Z",
    },
    "plugins": [
        {
            "slug": "degenerate-1",
            "plugin": "Degenerate One",
            "plugin_type": {
                "kind": "unknown",
                "confidence": 0.3,
                "basis": [],
            },
            "eq": {
                "present": False,
                "overall": "none",
                "sections": [],
                "notes": ["no EQ activity detected"],
            },
            "dynamics": {
                "present": False,
                "compression": None,
                "gr": None,
                "notes": [],
            },
            "nonlinearity": {
                "verdict": "not-measured",
            },
            "processing_order": {
                "order": "unknown",
                "confidence": 0.2,
                "basis": [],
            },
            "usable_as_spec": False,
            "why_not_spec": ["degenerate"],
        }
    ],
}


def _key_sets(value):
    """Dict key-sets at every nesting level, depth-first (top-down)."""
    if isinstance(value, dict):
        yield frozenset(value.keys())
        for child in value.values():
            yield from _key_sets(child)
    elif isinstance(value, list):
        for child in value:
            yield from _key_sets(child)


# ---------------------------------------------------------------------------
# render_json
# ---------------------------------------------------------------------------

def test_render_json_roundtrips_clean_doc():
    """Given a clean chain_doc literal, When rendered then dumped/loaded,
    Then every field survives with the same keys and value types."""
    rendered = render_json(CLEAN_DOC)
    loaded = json.loads(json.dumps(rendered))

    # Key sets identical at every nesting level.
    assert list(_key_sets(loaded)) == list(_key_sets(CLEAN_DOC))

    # Floats survive as numbers (not strings), unchanged by 4dp rounding.
    gr = loaded["plugins"][0]["dynamics"]["gr"]
    assert isinstance(gr["release_ms"], float)
    assert gr["release_ms"] == CLEAN_DOC["plugins"][0]["dynamics"]["gr"]["release_ms"]
    comp = loaded["plugins"][0]["dynamics"]["compression"]
    assert isinstance(comp["threshold_derived"], float)
    assert comp["threshold_derived"] == -18.5
    eq = loaded["plugins"][0]["eq"]["sections"]
    assert isinstance(eq[0]["freq_hz"], float)
    assert eq[1]["q"] == CLEAN_DOC["plugins"][0]["eq"]["sections"][1]["q"]

    # Bool / str / int / list / dict types preserved across the whole doc.
    assert loaded["plugins"][0]["usable_as_spec"] is True
    assert loaded["plugins"][0]["eq"]["overall"] == "clean"
    assert loaded["plugins"][0]["plugin_type"]["basis"] == ["compression", "gr_timeline"]
    assert loaded["source"]["aggregate_report"] == "out/aggregate_report.md"


# ---------------------------------------------------------------------------
# render_markdown
# ---------------------------------------------------------------------------

def test_render_markdown_artifact_golden():
    """Given the artifact chain_doc, When rendered to markdown, Then all
    human-verdict golden substrings appear and output is deterministic."""
    md = render_markdown(ARTIFACT_DOC)
    assert render_markdown(ARTIFACT_DOC) == md  # deterministic

    assert "## pro-c-3 (Pro-C 3)" in md
    assert "Plugin: Pro-C 3" in md
    assert "Type: compressor (confidence 0.7)" in md
    assert "implausible" in md            # EQ reason + GR release flag
    assert "conflict" in md               # compression derived-vs-doc fit
    assert "artifact" in md               # EQ verdict + nonlinearity
    assert "unknown" in md                # processing order
    assert "eq -> dyn -> eq" in md        # canonical order suggestion
    assert "Spec-usable: no" in md
    assert "Q=18098 implausible" in md    # EQ: <reason or flag> -> artifact
    assert "Compression: threshold -12.0 dB / ratio 2.5 (derived) vs -20.0/8.0 (doc fit) — conflict" in md
    assert "GR: attack 5.0 ms / release 39676.69 ms — release implausible" in md
    assert "Nonlinearity: not measured (artifact: harmonic tones overlapped)" in md
    assert "Order: unknown (suggestion: eq -> dyn -> eq — canonical, not measured)" in md


def test_render_markdown_clean():
    """Given the clean chain_doc, When rendered, Then spec-usable verdict,
    compression line and THD nonlinearity line appear."""
    md = render_markdown(CLEAN_DOC)

    assert "Spec-usable: yes" in md
    assert "Compression: threshold" in md
    assert "Compression: threshold -18.5 dB / ratio 4.0" in md
    assert "THD" in md
    assert "Nonlinearity: THD 0.1-0.3%" in md
    assert "EQ: 2 plausible section(s)" in md
    assert "GR: attack 50.0 ms / release 250.0 ms" in md
    assert "— release implausible" not in md
    assert " — conflict" not in md


def test_render_markdown_degenerate():
    """Given the degenerate chain_doc, When rendered, Then the no-EQ
    verdict appears and absent dynamics degrade to n/a."""
    md = render_markdown(DEGENERATE_DOC)

    assert "EQ: none" in md
    assert "Compression: n/a" in md
    assert "GR: n/a" in md
    assert "Nonlinearity: not measured" in md
    assert "Spec-usable: no (degenerate)" in md
