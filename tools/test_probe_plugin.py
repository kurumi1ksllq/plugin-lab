"""test_probe_plugin.py — Tests for probe_plugin.py (pure analysis functions).

Runs with pytest from the repo root:
    python -m pytest tools/test_probe_plugin.py -q

Covers the testable core (fake-surface detection, param_id scheme
classification, config snippet generation) without launching the app —
the live pipe probe is exercised by the integration/real-machine runs.
"""

import sys
from pathlib import Path

import pytest

# The tools/ scripts are not a package (no __init__.py); import by path.
sys.path.insert(0, str(Path(__file__).resolve().parent))

import probe_plugin as pp  # noqa: E402


# ---------------------------------------------------------------------------
# Inline fixture builders
# ---------------------------------------------------------------------------


def real_params(n=18):
    """A realistic vendor surface: named params with stable ids and varied values."""
    return [
        {"index": i, "param_id": str(i), "name": f"Param {i}",
         "value": round((i + 1) / 100.0, 4)}
        for i in range(n)
    ]


def hash_params():
    """Gem-style surface: hash ids + real names (Gem Comp LA/EQP mode)."""
    return [
        {"index": 0, "param_id": "1954968683", "name": "Power", "value": 1.0},
        {"index": 1, "param_id": "894065497", "name": "Gain", "value": 0.2071},
        {"index": 2, "param_id": "2135186029", "name": "Peak Reduction", "value": 0.2},
        {"index": 3, "param_id": "1292753637", "name": "Comp/Limit", "value": 0.0},
    ]


def fake_params(n=63):
    """The elysia/Pro-MB fake surface: generic names, no param_id, dup values."""
    return [
        {"index": i, "name": f"Parameter {i}", "value": 0.0}
        for i in range(n)
    ]


def fake_params_with_ids(n=63):
    """Fake surface but with sequential ids — catches the value-dup tell."""
    return [
        {"index": i, "param_id": str(i), "name": f"Parameter {i}", "value": 0.0}
        for i in range(n)
    ]


# ---------------------------------------------------------------------------
# Fake-surface detection
# ---------------------------------------------------------------------------


class TestDetectFakeSurface:
    def test_real_surface_not_fake(self):
        report = pp.detect_fake_surface(real_params())
        assert report["fake"] is False
        assert report["reasons"] == []

    def test_hash_surface_not_fake(self):
        report = pp.detect_fake_surface(hash_params())
        assert report["fake"] is False

    def test_generic_names_fake(self):
        report = pp.detect_fake_surface(fake_params())
        assert report["fake"] is True
        assert any("generic" in r for r in report["reasons"])

    def test_no_param_id_fake(self):
        report = pp.detect_fake_surface(fake_params())
        assert report["no_id"] == 63
        assert report["no_id"] / report["total"] >= pp.NO_ID_MIN_FRACTION

    def test_duplicate_values_fake_even_with_ids(self):
        # Sequential ids would pass the no-id check; the dup-value tell catches it.
        report = pp.detect_fake_surface(fake_params_with_ids())
        assert report["fake"] is True
        assert any("identical values" in r for r in report["reasons"])

    def test_empty_params_fake(self):
        report = pp.detect_fake_surface([])
        assert report["fake"] is True
        assert report["reasons"] == ["no parameters returned"]

    def test_dup_threshold_respects_real_variation(self):
        # Values differing across params should NOT trip the dup tell.
        params = [{"index": i, "param_id": str(i), "name": f"Param {i}",
                   "value": i / 100.0} for i in range(20)]
        report = pp.detect_fake_surface(params)
        assert report["fake"] is False


# ---------------------------------------------------------------------------
# param_id scheme classification
# ---------------------------------------------------------------------------


class TestParamIdKind:
    def test_sequential_index(self):
        assert pp.param_id_kind(real_params()) == "index"

    def test_hash(self):
        assert pp.param_id_kind(hash_params()) == "hash"

    def test_no_ids(self):
        assert pp.param_id_kind(fake_params()) == "none"

    def test_empty(self):
        assert pp.param_id_kind([]) == "none"


# ---------------------------------------------------------------------------
# Config snippet generation
# ---------------------------------------------------------------------------


class TestBuildConfigSnippet:
    def test_snippet_is_valid_json(self):
        import json
        snippet = pp.build_config_snippet(real_params())
        parsed = json.loads(snippet)
        assert "setup" in parsed
        assert "scan" in parsed

    def test_snippet_skips_power_bypass(self):
        params = [
            {"index": 0, "param_id": "0", "name": "Power", "value": 1.0},
            {"index": 1, "param_id": "1", "name": "Bypass", "value": 0.0},
            {"index": 2, "param_id": "2", "name": "Input Gain", "value": 0.5},
            {"index": 3, "param_id": "3", "name": "Ratio", "value": 0.0},
        ]
        import json
        snippet = json.loads(pp.build_config_snippet(params))
        setup_names = [s["name"] for s in snippet["setup"]]
        assert "Power" not in setup_names
        assert "Bypass" not in setup_names
        assert "Input Gain" in setup_names or "Ratio" in setup_names

    def test_empty_params_snippet(self):
        import json
        snippet = json.loads(pp.build_config_snippet([]))
        assert snippet["setup"] == []
        assert snippet["scan"]["param_id"] == ""


# ---------------------------------------------------------------------------
# format_report
# ---------------------------------------------------------------------------


class TestFormatReport:
    def test_real_surface_text(self):
        text = pp.format_report(real_params(3), pp.detect_fake_surface(real_params(3)), "index")
        assert "TOTAL PARAMS: 3" in text
        assert "looks real" in text
        assert "FAKE SURFACE" not in text

    def test_fake_surface_text(self):
        report = pp.detect_fake_surface(fake_params(5))
        text = pp.format_report(fake_params(5), report, "none")
        assert "FAKE SURFACE DETECTED" in text
        assert "skip collection" in text


if __name__ == "__main__":
    sys.exit(pytest.main([__file__, "-v"]))