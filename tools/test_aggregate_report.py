"""Tests for aggregate_report.py (pure helpers + aggregation engine).

Wave-1 T1-B of ticket #24: pure-function layer. Wave-2 T1-C adds the
aggregation engine (discover_plugins / analyze_plugin) and ground-truth
tolerance calibration (verify_against). Wave-3 T1-D adds the report
writers (write_markdown / write_json) — deterministic, idempotent
(only generated_at differs between runs). Wave-1 fixtures are inline
literals matching the PluginLab export shapes (SPEC.md); Wave-2 fixtures
come from tools/test_data/synthetic_dataset.py (deterministic
known-parameter docs).

Usage:
    python -m pytest tools/test_aggregate_report.py -q
"""
import json
from pathlib import Path

import pytest

from aggregate_report import (
    LOCKED_TOLERANCES,
    analyze_plugin,
    discover_plugins,
    harmonic_summary,
    is_compression_unity,
    is_eq_flat,
    is_harmonic_empty,
    load_harmonic,
    verify_against,
    write_json,
    write_markdown,
)

# test_data/ is a package-less dir. pytest puts tools/ on sys.path, so
# `from test_data.synthetic_dataset import ...` resolves it as a namespace
# package; the fallback (direct sys.path insertion) covers invocations where
# tools/ is not on sys.path.
try:
    from test_data.synthetic_dataset import (
        make_compression_curve,
        make_dataset,
        make_freq_curve,
    )
except ImportError:
    import sys
    from pathlib import Path

    sys.path.insert(0, str(Path(__file__).resolve().parent / "test_data"))
    from synthetic_dataset import (  # noqa: F401
        make_compression_curve,
        make_dataset,
        make_freq_curve,
    )

# ---------------------------------------------------------------------------
# load_harmonic
# ---------------------------------------------------------------------------


def test_load_harmonic_dataset_nested():
    """A dataset doc nests the harmonic block under data['harmonic']['tones']."""
    data = {"type": "dataset", "harmonic": {"tones": [
        {"fundamental_hz": 1000.0, "fundamental_db": -3.0, "thd_percent": 1.2,
         "harmonics": [{"order": 2, "freq": 2000.0, "mag_db": -40.0,
                        "percent": 0.8}]},
    ]}}
    tones = load_harmonic(data)
    assert len(tones) == 1
    assert tones[0]["fundamental_hz"] == 1000.0
    assert tones[0]["harmonics"][0]["order"] == 2


def test_load_harmonic_standalone():
    """A standalone layout carries tones directly at the top level."""
    data = {"tones": [
        {"fundamental_hz": 440.0, "fundamental_db": 0.0, "thd_percent": 0.5,
         "harmonics": [{"order": 2, "freq": 880.0, "mag_db": -50.0,
                        "percent": 0.3}]},
    ]}
    tones = load_harmonic(data)
    assert len(tones) == 1
    assert tones[0]["fundamental_hz"] == 440.0
    assert tones[0]["thd_percent"] == 0.5


def test_load_harmonic_body_is_data():
    """A doc whose body itself IS the harmonic block is accepted."""
    data = {"fundamental_hz": 220.0, "fundamental_db": -6.0,
            "thd_percent": 0.3, "tones": [
                {"fundamental_hz": 220.0, "fundamental_db": -6.0,
                 "thd_percent": 0.3, "harmonics": []},
            ]}
    tones = load_harmonic(data)
    assert len(tones) == 1
    assert tones[0]["fundamental_hz"] == 220.0


def test_load_harmonic_empty_doc():
    """An empty doc has no tones."""
    assert load_harmonic({}) == []


def test_load_harmonic_empty_block():
    """A present-but-empty harmonic block yields no tones."""
    assert load_harmonic({"harmonic": {}}) == []


def test_load_harmonic_missing_block():
    """A dataset doc without the harmonic block yields no tones."""
    assert load_harmonic({"type": "dataset"}) == []


# ---------------------------------------------------------------------------
# harmonic_summary
# ---------------------------------------------------------------------------


def test_harmonic_summary_dominant_by_max_mag():
    """The dominant harmonic is the one with max mag_db, not lowest order."""
    tones = [{"fundamental_hz": 1000.0, "fundamental_db": -3.0,
              "thd_percent": 1.2, "harmonics": [
                  {"order": 2, "freq": 2000.0, "mag_db": -40.0, "percent": 0.8},
                  {"order": 3, "freq": 3000.0, "mag_db": -25.0, "percent": 2.0},
                  {"order": 4, "freq": 4000.0, "mag_db": -33.0, "percent": 1.1},
              ]}]
    summary = harmonic_summary(tones)
    assert len(summary) == 1
    assert summary[0]["fundamental_hz"] == 1000.0
    assert summary[0]["thd_percent"] == 1.2
    assert summary[0]["dominant_order"] == 3
    assert summary[0]["dominant_mag_db"] == -25.0


def test_harmonic_summary_empty_harmonics_dominant_none():
    """A tone with no harmonics has no dominant harmonic."""
    tones = [{"fundamental_hz": 440.0, "fundamental_db": 0.0,
              "thd_percent": 0.0, "harmonics": []}]
    summary = harmonic_summary(tones)
    assert summary[0]["fundamental_hz"] == 440.0
    assert summary[0]["dominant_order"] is None
    assert summary[0]["dominant_mag_db"] is None


def test_harmonic_summary_empty_tones():
    """No tones in, no summaries out."""
    assert harmonic_summary([]) == []


# ---------------------------------------------------------------------------
# is_eq_flat
# ---------------------------------------------------------------------------


def test_is_eq_flat_zeros():
    """All-zero magnitudes are degenerate-flat."""
    assert is_eq_flat([0.0, 0.0, 0.0, 0.0]) is True


def test_is_eq_flat_bell_curve():
    """A real EQ bell (a few dB of variation) is not flat."""
    assert is_eq_flat([0.0, 3.0, 6.0, 3.0, 0.0]) is False


def test_is_eq_flat_near_flat():
    """Variation under the 0.1 dB threshold still counts as flat."""
    assert is_eq_flat([0.0, 0.04, -0.04]) is True


def test_is_eq_flat_empty():
    """An empty capture has nothing to measure, so it is flat."""
    assert is_eq_flat([]) is True


# ---------------------------------------------------------------------------
# is_compression_unity
# ---------------------------------------------------------------------------


def test_is_compression_unity_zero_gr():
    """All-zero gain reduction is degenerate-unity."""
    curve = [{"input_db": -60.0, "output_db": -60.0, "gr_db": 0.0},
             {"input_db": -20.0, "output_db": -20.0, "gr_db": 0.0},
             {"input_db": 0.0, "output_db": 0.0, "gr_db": 0.0}]
    assert is_compression_unity(curve) is True


def test_is_compression_unity_real_gain_reduction():
    """Real gain reduction (-3, -6 dB) means the curve is not unity."""
    curve = [{"input_db": -60.0, "output_db": -60.0, "gr_db": 0.0},
             {"input_db": -20.0, "output_db": -23.0, "gr_db": -3.0},
             {"input_db": 0.0, "output_db": -6.0, "gr_db": -6.0}]
    assert is_compression_unity(curve) is False


def test_is_compression_unity_empty():
    """An empty curve has no compression to report, so it is unity."""
    assert is_compression_unity([]) is True


# ---------------------------------------------------------------------------
# is_harmonic_empty
# ---------------------------------------------------------------------------


def test_is_harmonic_empty_true():
    """An empty tones list is empty."""
    assert is_harmonic_empty([]) is True


def test_is_harmonic_empty_false():
    """A tones list with one measurement is not empty."""
    tones = [{"fundamental_hz": 440.0, "fundamental_db": 0.0,
              "thd_percent": 0.0, "harmonics": []}]
    assert is_harmonic_empty(tones) is False


# ---------------------------------------------------------------------------
# LOCKED_TOLERANCES
# ---------------------------------------------------------------------------


def test_locked_tolerances_seeded_with_headroom():
    """Locked tolerances sit far above the measured calibration seed
    (0.68% freq / 0.005 dB gain / 2.77% Q / bit-exact compression+GR)."""
    assert LOCKED_TOLERANCES["freq_pct"] == 5.0
    assert LOCKED_TOLERANCES["gain_db"] == 0.5
    assert LOCKED_TOLERANCES["q_pct"] == 20.0
    assert LOCKED_TOLERANCES["threshold_db"] == 1.0
    assert LOCKED_TOLERANCES["ratio_pct"] == 20.0
    assert LOCKED_TOLERANCES["tau_pct"] == 20.0


# ---------------------------------------------------------------------------
# discover_plugins
# ---------------------------------------------------------------------------


def test_discover_plugins_sorted_flags_and_ignores_summary(tmp_path):
    """Per-plugin dirs are listed sorted by slug; dataset.json presence is
    flagged; summary.json files (root-level or inside a dir) are ignored."""
    (tmp_path / "zeta").mkdir()
    (tmp_path / "zeta" / "dataset.json").write_text(
        '{"type": "dataset", "context": {"plugin": "Z"}}', encoding="utf-8")
    (tmp_path / "alpha").mkdir()
    (tmp_path / "alpha" / "dataset.json").write_text(
        '{"type": "dataset", "context": {"plugin": "A"}}', encoding="utf-8")
    (tmp_path / "alpha" / "summary.json").write_text("{}", encoding="utf-8")
    (tmp_path / "gamma").mkdir()            # no dataset.json at all
    (tmp_path / "summary.json").write_text("{}", encoding="utf-8")

    plugins = discover_plugins(tmp_path)

    assert [p["slug"] for p in plugins] == ["alpha", "gamma", "zeta"]
    assert [p["has_dataset"] for p in plugins] == [True, False, True]
    assert plugins[0]["path"] == str(tmp_path / "alpha")
    assert plugins[2]["path"] == str(tmp_path / "zeta")


def test_discover_plugins_missing_out_dir_is_empty():
    """A missing out_dir yields no plugins, never an error."""
    assert discover_plugins("definitely/not/here") == []


# ---------------------------------------------------------------------------
# analyze_plugin
# ---------------------------------------------------------------------------


def _write_doc(tmp_path, slug, doc):
    """Write a doc dict to <tmp_path>/<slug>/dataset.json; return its path."""
    out = tmp_path / slug
    out.mkdir()
    path = out / "dataset.json"
    path.write_text(json.dumps(doc), encoding="utf-8")
    return path


def test_analyze_plugin_synthetic_derives_all_sections(tmp_path):
    """A synthetic dataset with known params derives every section as ok."""
    data = make_dataset(
        "synth", "Synthetic",
        peak_hz=1000.0, gain_db=6.0, q=1.0,
        threshold_db=-30.0, ratio=4.0,
        attack_sec=0.001, release_sec=0.05,
        tones=[{"fundamental_hz": 440.0, "fundamental_db": -12.0,
                "thd_percent": 1.0}])
    path = _write_doc(tmp_path, "synth", data)

    row = analyze_plugin(path)

    assert row["slug"] == "synth"
    assert row["plugin"] == "Synthetic"
    assert row["has_freq"] is True and row["has_compression"] is True
    assert row["has_gr"] is True and row["has_harmonic"] is True
    assert row["freq"]["status"] == "ok"
    assert row["freq"]["freq_hz"] == pytest.approx(1000.0, rel=0.01)
    assert row["freq"]["gain_db"] == pytest.approx(6.0, abs=0.05)
    assert row["freq"]["q"] == pytest.approx(1.0, rel=0.05)
    assert row["compression"]["status"] == "ok"
    assert row["compression"]["threshold_db"] == pytest.approx(-30.0, abs=1e-6)
    assert row["compression"]["ratio"] == pytest.approx(4.0, rel=1e-6)
    assert row["compression"]["json_fitted"] == {
        "ratio": 4.0, "threshold_db": -30.0, "knee_db": 0.0}
    assert row["gr"]["status"] == "ok"
    assert row["gr"]["attack_ms"] == pytest.approx(1.0, abs=1e-9)
    assert row["gr"]["release_ms"] == pytest.approx(50.0, abs=1e-9)
    assert row["gr"]["valid"] is True
    assert row["harmonic"]["status"] == "ok"
    assert row["harmonic"]["tones_count"] == 1
    assert row["harmonic"]["summary"][0]["fundamental_hz"] == 440.0
    assert row["status"] == "ok"


def test_analyze_plugin_degenerate_dataset(tmp_path):
    """Flat EQ / unity compression / invalid tau -> degenerate sections,
    degenerate overall verdict, no crash."""
    doc = {
        "type": "dataset",
        "context": {"plugin": "FlatBoi"},
        "frequency_response": {"raw": [
            {"f": 100.0, "mag": 0.0, "phase": 0.0},
            {"f": 1000.0, "mag": 0.0, "phase": 0.0},
            {"f": 10000.0, "mag": 0.0, "phase": 0.0},
        ], "smoothed_1_12": []},
        "compression": {"curve": [
            {"input_db": -20.0, "output_db": -20.0, "gr_db": 0.0},
            {"input_db": 0.0, "output_db": 0.0, "gr_db": 0.0},
        ], "fitted": {}},
        "gr_timeline": {"gr": {},
                        "tau": {"attack_sec": 0.0, "release_sec": 0.0,
                                "valid": False}},
        "harmonic": {"tones": []},
    }
    path = _write_doc(tmp_path, "flat", doc)

    row = analyze_plugin(path)

    assert row["freq"]["status"] == "degenerate"
    assert row["compression"]["status"] == "degenerate"
    assert row["gr"]["status"] == "degenerate"
    assert row["harmonic"]["status"] == "no-data"
    assert row["status"] == "degenerate"
    assert row["has_freq"] is True and row["has_gr"] is True
    assert row["has_harmonic"] is False


def test_analyze_plugin_missing_and_malformed_files(tmp_path):
    """Unreadable or unparseable dataset files mark every section
    derivation-failed — never a crash."""
    missing = analyze_plugin(tmp_path / "ghost" / "dataset.json")
    assert missing["slug"] == "ghost"
    assert missing["plugin"] == "?"
    assert missing["freq"]["status"] == "derivation-failed"
    assert missing["compression"]["status"] == "derivation-failed"
    assert missing["gr"]["status"] == "derivation-failed"
    assert missing["harmonic"]["status"] == "derivation-failed"
    assert missing["status"] == "no-data"

    bad = tmp_path / "bad"
    bad.mkdir()
    (bad / "dataset.json").write_text("{not json!!!", encoding="utf-8")
    malformed = analyze_plugin(bad / "dataset.json")
    assert malformed["freq"]["status"] == "derivation-failed"
    assert malformed["status"] == "no-data"


# ---------------------------------------------------------------------------
# verify_against (ground-truth calibration)
# ---------------------------------------------------------------------------


def test_verify_against_calibration_all_checks_pass(tmp_path):
    """Known ground truth vs derived row: every check passes with small
    error (freq < 1%, gain < 0.05 dB) and sensible units."""
    data = make_dataset(
        "synth", "Synthetic",
        peak_hz=1000.0, gain_db=6.0, q=1.0,
        threshold_db=-30.0, ratio=4.0,
        attack_sec=0.001, release_sec=0.05)
    row = analyze_plugin(_write_doc(tmp_path, "synth", data))

    result = verify_against(row, {
        "freq_hz": 1000.0, "gain_db": 6.0, "q": 1.0,
        "threshold_db": -30.0, "ratio": 4.0,
        "attack_ms": 1.0, "release_ms": 50.0,
    })

    assert result["all_ok"] is True
    assert set(result["checks"]) == {
        "freq_hz", "gain_db", "q", "threshold_db", "ratio",
        "attack_ms", "release_ms"}
    assert result["checks"]["freq_hz"]["error"] < 1.0        # percent
    assert result["checks"]["gain_db"]["error"] < 0.05       # dB
    assert result["checks"]["q"]["error"] < 5.0              # percent
    assert result["checks"]["freq_hz"]["unit"] == "%"
    assert result["checks"]["gain_db"]["unit"] == "dB"
    assert result["checks"]["threshold_db"]["unit"] == "dB"
    assert result["checks"]["attack_ms"]["unit"] == "%"
    assert result["checks"]["threshold_db"]["error"] == 0.0  # bit-exact
    assert result["checks"]["ratio"]["error"] == 0.0         # bit-exact


def test_verify_against_checks_only_known_derived_pairs(tmp_path):
    """Only parameters with both a known expected and a derived value
    are checked; unknown params in known_params are ignored."""
    data = make_dataset(
        "synth", "Synthetic",
        peak_hz=1000.0, gain_db=6.0, q=1.0,
        threshold_db=-30.0, ratio=4.0,
        attack_sec=0.001, release_sec=0.05)
    row = analyze_plugin(_write_doc(tmp_path, "synth", data))

    result = verify_against(row, {"freq_hz": 1000.0, "gain_db": 6.0,
                                  "bogus_param": 123.0})

    assert set(result["checks"]) == {"freq_hz", "gain_db"}
    assert result["all_ok"] is True


def test_verify_against_no_known_params_yields_no_checks(tmp_path):
    """No known params -> no checks; all_ok is False (nothing verified
    is not a pass)."""
    data = make_dataset(
        "synth", "Synthetic",
        peak_hz=1000.0, gain_db=6.0, q=1.0,
        threshold_db=-30.0, ratio=4.0,
        attack_sec=0.001, release_sec=0.05)
    row = analyze_plugin(_write_doc(tmp_path, "synth", data))

    result = verify_against(row, {})

    assert result["checks"] == {}
    assert result["all_ok"] is False


def test_verify_against_skips_invalid_tau(tmp_path):
    """attack/release are not checked when the tau block is invalid
    (mirrors reverse_derive main's 'tau not valid' skip)."""
    doc = {
        "type": "dataset",
        "context": {"plugin": "Mixed"},
        "frequency_response": {"raw": make_freq_curve(1000.0, 6.0, 1.0),
                               "smoothed_1_12": []},
        "compression": {"curve": make_compression_curve(-30.0, 4.0),
                        "fitted": {}},
        "gr_timeline": {"gr": {},
                        "tau": {"attack_sec": 0.0, "release_sec": 0.0,
                                "valid": False}},
        "harmonic": {"tones": []},
    }
    row = analyze_plugin(_write_doc(tmp_path, "mixed", doc))
    assert row["gr"]["status"] == "degenerate"

    result = verify_against(row, {
        "freq_hz": 1000.0, "gain_db": 6.0, "q": 1.0,
        "threshold_db": -30.0, "ratio": 4.0,
        "attack_ms": 1.0, "release_ms": 50.0,
    })

    assert set(result["checks"]) == {
        "freq_hz", "gain_db", "q", "threshold_db", "ratio"}
    assert result["all_ok"] is True


# ---------------------------------------------------------------------------
# Report writers (T1-D): write_markdown / write_json
# ---------------------------------------------------------------------------

_META = {"generated_at": "2026-08-12T10:00:00+00:00", "out_dir": "out"}


def _hand_built_rows():
    """Two rows built by hand: one degenerate (data present but flat/unity),
    one no-data (every section derivation-failed, e.g. unreadable file)."""
    return [
        {"slug": "flat", "plugin": "FlatBoi",
         "has_freq": True, "has_compression": True,
         "has_gr": True, "has_harmonic": False,
         "freq": {"freq_hz": None, "gain_db": None, "q": None,
                  "status": "degenerate"},
         "compression": {"threshold_db": None, "ratio": None,
                         "status": "degenerate", "json_fitted": {}},
         "gr": {"attack_ms": None, "release_ms": None, "valid": False,
                "status": "degenerate"},
         "harmonic": {"tones_count": 0, "summary": [],
                      "status": "no-data"},
         "status": "degenerate"},
        {"slug": "ghost", "plugin": "?",
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
         "status": "no-data"},
    ]


def _analyzed_rows(tmp_path):
    """Real analyze_plugin rows: one synthetic ok dataset + one empty dir
    (missing dataset.json -> derivation-failed row, overall no-data)."""
    data_dir = tmp_path / "datasets"
    data_dir.mkdir()
    data = make_dataset(
        "synth", "Synthetic",
        peak_hz=1000.0, gain_db=6.0, q=1.0,
        threshold_db=-30.0, ratio=4.0,
        attack_sec=0.001, release_sec=0.05,
        tones=[{"fundamental_hz": 440.0, "fundamental_db": -12.0,
                "thd_percent": 1.0}])
    _write_doc(data_dir, "synth", data)
    (data_dir / "empty").mkdir()
    return [analyze_plugin(Path(p["path"]) / "dataset.json")
            for p in discover_plugins(data_dir)]


def test_write_json_real_rows(tmp_path):
    """Real analyzed rows: JSON mirrors generated_at/out_dir/tolerances/
    counts/plugins, plugins sorted by slug, trailing newline."""
    rows = _analyzed_rows(tmp_path)
    path = write_json(rows, _META, tmp_path / "report.json")

    assert path == tmp_path / "report.json"
    assert path.is_file()
    text = path.read_text(encoding="utf-8")
    assert text.endswith("\n")
    doc = json.loads(text)
    assert set(doc) == {"generated_at", "out_dir", "tolerances", "counts",
                        "plugins"}
    assert doc["generated_at"] == _META["generated_at"]
    assert doc["out_dir"] == _META["out_dir"]
    assert doc["tolerances"] == LOCKED_TOLERANCES
    assert doc["counts"] == {"total": 2, "with_data": 1, "no_data": 1,
                             "degenerate": 0, "derivation_failed": 1}
    assert [p["slug"] for p in doc["plugins"]] == ["empty", "synth"]
    assert doc["plugins"][0]["status"] == "no-data"
    assert doc["plugins"][1]["plugin"] == "Synthetic"
    assert doc["plugins"][1]["freq"]["freq_hz"] == pytest.approx(
        1000.0, rel=0.01)


def test_write_markdown_real_rows(tmp_path):
    """Real analyzed rows: markdown has per-plugin sections with slug, plugin
    name, derived values and a summary table."""
    rows = _analyzed_rows(tmp_path)
    path = write_markdown(rows, _META, tmp_path / "report.md")

    assert path == tmp_path / "report.md"
    assert path.is_file()
    text = path.read_text(encoding="utf-8")
    assert "## synth" in text
    assert "Synthetic" in text
    assert "## empty" in text
    assert "threshold_db=-30" in text
    assert "ratio=4" in text
    assert "attack_ms=1" in text
    assert "release_ms=50" in text
    assert "tones=1" in text
    assert "| Total plugins | 2 |" in text
    assert "| With data | 1 |" in text
    assert "| No data | 1 |" in text
    assert "| Derivation failed | 1 |" in text
    assert f"| Generated at | {_META['generated_at']} |" in text


def test_write_json_hand_built_rows(tmp_path):
    """Hand-built degenerate + no-data rows: counts break down correctly and
    per-section statuses survive the round-trip."""
    rows = _hand_built_rows()
    path = write_json(rows, _META, tmp_path / "report.json")

    doc = json.loads(path.read_text(encoding="utf-8"))
    assert doc["counts"] == {"total": 2, "with_data": 1, "no_data": 1,
                             "degenerate": 1, "derivation_failed": 1}
    assert [p["slug"] for p in doc["plugins"]] == ["flat", "ghost"]
    assert doc["plugins"][0]["status"] == "degenerate"
    assert doc["plugins"][1]["freq"]["status"] == "derivation-failed"
    assert doc["plugins"][1]["gr"]["valid"] is False


def test_write_markdown_hand_built_rows(tmp_path):
    """Hand-built rows: degenerate/no-data statuses and tolerance list are
    rendered in the summary table."""
    rows = _hand_built_rows()
    path = write_markdown(rows, _META, tmp_path / "report.md")

    text = path.read_text(encoding="utf-8")
    assert "## flat" in text
    assert "FlatBoi" in text
    assert "Status: degenerate" in text
    assert "## ghost" in text
    assert "derivation-failed" in text
    assert "| Degenerate | 1 |" in text
    assert "| Derivation failed | 1 |" in text
    assert "| Locked tolerances |" in text
    assert "freq_pct=5.0%" in text
    assert "gain_db=0.5 dB" in text


def test_write_json_escaping_special_chars(tmp_path):
    """A plugin name with quotes/backslashes renders as valid JSON and
    round-trips exactly."""
    name = 'Sneaky "quotes" \\ backslash'
    row = dict(_hand_built_rows()[1], plugin=name)
    path = write_json([row], _META, tmp_path / "report.json")

    doc = json.loads(path.read_text(encoding="utf-8"))
    assert doc["plugins"][0]["plugin"] == name


def test_writers_idempotent_except_generated_at(tmp_path):
    """Re-running both writers with a different generated_at produces output
    differing ONLY in the generated_at value — the rest is byte-identical."""
    rows = _hand_built_rows()
    meta_a = {"generated_at": "2026-08-12T10:00:00+00:00", "out_dir": "out"}
    meta_b = {"generated_at": "2026-08-12T11:00:00+00:00", "out_dir": "out"}

    md_a = write_markdown(rows, meta_a, tmp_path / "a.md")
    md_b = write_markdown(rows, meta_b, tmp_path / "b.md")
    text_a = md_a.read_text(encoding="utf-8")
    text_b = md_b.read_text(encoding="utf-8")
    assert text_a != text_b
    diff = [i for i, (x, y) in enumerate(zip(text_a.splitlines(),
                                             text_b.splitlines())) if x != y]
    assert len(diff) == 1
    assert "Generated at" in text_a.splitlines()[diff[0]]

    json_a = write_json(rows, meta_a, tmp_path / "a.json")
    json_b = write_json(rows, meta_b, tmp_path / "b.json")
    doc_a = json.loads(json_a.read_text(encoding="utf-8"))
    doc_b = json.loads(json_b.read_text(encoding="utf-8"))
    assert doc_a["generated_at"] != doc_b["generated_at"]
    doc_a.pop("generated_at")
    doc_b.pop("generated_at")
    assert doc_a == doc_b


def test_write_json_deterministic_order(tmp_path):
    """Rows in any input order produce identical output: plugins are sorted
    by slug inside the writer."""
    rows = _hand_built_rows()
    shuffled = [rows[1], rows[0]]  # ghost before flat
    path_a = write_json(shuffled, _META, tmp_path / "a.json")
    path_b = write_json(rows, _META, tmp_path / "b.json")

    assert path_a.read_text(encoding="utf-8") == \
        path_b.read_text(encoding="utf-8")
    doc = json.loads(path_a.read_text(encoding="utf-8"))
    assert [p["slug"] for p in doc["plugins"]] == ["flat", "ghost"]


def test_writers_propagate_oserror(tmp_path):
    """An unwritable path raises OSError — writers never swallow IO errors."""
    bad = tmp_path / "no" / "such" / "dir" / "report.json"
    rows = _hand_built_rows()
    with pytest.raises(OSError):
        write_json(rows, _META, bad)
    with pytest.raises(OSError):
        write_markdown(rows, _META, bad)
