"""test_stage_fusion.py — Tests for stage_fusion.py (issue #105 T7).

stage_fusion.py blends picked v3 stages from two or more chain_doc inputs
into one fused chain_doc:

    {contract_version:"3", generated_at, source,
     plugins:[{... stages:[picked stages, provenance preserved] ...}],
     fusion:{inputs:[{alias,path,slug,plugin}], picks:[{alias,stage}]}}

Tests exercise the CLI (public behavior) only: inputs are chain_doc files
written to tmp_path from the shared deterministic fixtures
(test_data/chain_fixtures.py), outputs are parsed back and validated
against the v3 contract.

Runs with pytest from the repo root:
    python -m pytest tools/test_stage_fusion.py -q
"""

import json
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import describe_chain as dc  # noqa: E402
from describe_schema import validate_chain_doc  # noqa: E402
from test_data.chain_fixtures import (  # noqa: E402
    make_chain_doc_artifact,
    make_chain_doc_v3_artifact,
    make_chain_doc_v3_clean,
    make_compression_conflict,
    make_dyn_active_row,
    make_harmonic_clean,
    make_pro_c3_row,
    upgrade_to_v3,
)

_META = {"generated_at": "2026-08-13T00:00:00+00:00",
         "aggregate_report": "aggregate_report.json",
         "report_generated_at": "2026-08-13T00:00:00+00:00"}

_SCRIPT = Path(__file__).resolve().parent / "stage_fusion.py"


def _write_doc(tmp_path, name, doc):
    path = tmp_path / name
    path.write_text(json.dumps(doc, indent=2), encoding="utf-8")
    return path


def _run(args):
    return subprocess.run([sys.executable, str(_SCRIPT)] + [str(a) for a in args],
                          capture_output=True, text=True)


# ---------------------------------------------------------------------------
# Happy path
# ---------------------------------------------------------------------------


def test_fusion_picks_stages_from_each_input(tmp_path):
    """Pick A's eq + B's dynamics -> fused doc carries exactly those stages."""
    doc_a = _write_doc(tmp_path, "a.json", make_chain_doc_v3_clean())
    doc_b = _write_doc(tmp_path, "b.json", make_chain_doc_v3_artifact())
    out = tmp_path / "fused.json"

    proc = _run([doc_a, doc_b, "--pick", "a:eq", "--pick", "b:dynamics",
                 "--out", out])
    assert proc.returncode == 0, proc.stderr

    fused = json.loads(out.read_text(encoding="utf-8"))
    assert fused["contract_version"] == "3"
    assert fused["fusion"]["picks"] == [{"alias": "a", "stage": "eq"},
                                        {"alias": "b", "stage": "dynamics"}]
    assert [stage["kind"] for stage in fused["plugins"][0]["stages"]] == [
        "eq", "dynamics"]
    assert validate_chain_doc(fused) == {"ok": True, "errors": []}


def test_fusion_records_inputs_and_provenance(tmp_path):
    """The fusion block names both inputs; each stage keeps its provenance
    and gains the input alias it came from."""
    doc_a = _write_doc(tmp_path, "a.json", make_chain_doc_v3_clean())
    doc_b = _write_doc(tmp_path, "b.json", make_chain_doc_v3_artifact())
    out = tmp_path / "fused.json"

    proc = _run([doc_a, doc_b, "--pick", "a:eq", "--pick", "b:dynamics",
                 "--out", out])
    assert proc.returncode == 0, proc.stderr

    fused = json.loads(out.read_text(encoding="utf-8"))
    aliases = [item["alias"] for item in fused["fusion"]["inputs"]]
    assert aliases == ["a", "b"]
    assert fused["fusion"]["inputs"][0]["slug"] == "synthetic"

    stages = fused["plugins"][0]["stages"]
    assert stages[0]["provenance"]["source"] == "frequency_response"
    assert stages[0]["provenance"]["input"]["alias"] == "a"
    assert stages[1]["provenance"]["source"] == "compression+gr"
    assert stages[1]["provenance"]["input"]["alias"] == "b"
    assert stages[1]["provenance"]["basis"]


def test_fusion_output_is_byte_stable(tmp_path):
    """Two runs with the same inputs produce byte-identical documents."""
    doc_a = _write_doc(tmp_path, "a.json", make_chain_doc_v3_clean())
    doc_b = _write_doc(tmp_path, "b.json", make_chain_doc_v3_artifact())
    first = tmp_path / "first.json"
    second = tmp_path / "second.json"

    assert _run([doc_a, doc_b, "--pick", "a:eq", "--pick", "b:dynamics",
                 "--out", first]).returncode == 0
    assert _run([doc_a, doc_b, "--pick", "a:eq", "--pick", "b:dynamics",
                 "--out", second]).returncode == 0
    assert first.read_bytes() == second.read_bytes()


def test_fusion_inherits_blockers_from_its_sources(tmp_path):
    """Picking an artifact dynamics segment marks the fused doc unusable and
    says which input the blocker came from."""
    doc_a = _write_doc(tmp_path, "a.json", make_chain_doc_v3_clean())
    doc_b = _write_doc(tmp_path, "b.json", make_chain_doc_v3_artifact())
    out = tmp_path / "fused.json"

    proc = _run([doc_a, doc_b, "--pick", "a:eq", "--pick", "b:dynamics",
                 "--out", out])
    assert proc.returncode == 0, proc.stderr

    plugin = json.loads(out.read_text(encoding="utf-8"))["plugins"][0]
    assert plugin["usable_as_spec"] is False
    assert any("inherited from b" in reason
               for reason in plugin["why_not_spec"])
    # The non-picked segments are present but explicitly empty.
    assert plugin["nonlinearity"]["verdict"] == "not-measured"


def test_fusion_clean_picks_stay_spec_usable(tmp_path):
    """Picking only clean segments keeps the fused document spec-usable."""
    doc_a = _write_doc(tmp_path, "a.json", make_chain_doc_v3_clean())
    out = tmp_path / "fused.json"

    proc = _run([doc_a, "--pick", "a:eq", "--pick", "a:nonlinearity",
                 "--out", out])
    assert proc.returncode == 0, proc.stderr

    plugin = json.loads(out.read_text(encoding="utf-8"))["plugins"][0]
    assert plugin["usable_as_spec"] is True
    assert plugin["why_not_spec"] == []


def test_fusion_renders_markdown_when_asked(tmp_path):
    """--markdown also writes the human report, including the Fusion section."""
    doc_a = _write_doc(tmp_path, "a.json", make_chain_doc_v3_clean())
    doc_b = _write_doc(tmp_path, "b.json", make_chain_doc_v3_artifact())
    out = tmp_path / "fused.json"
    md = tmp_path / "fused.md"

    proc = _run([doc_a, doc_b, "--pick", "a:eq", "--pick", "b:dynamics",
                 "--out", out, "--markdown", md])
    assert proc.returncode == 0, proc.stderr

    text = md.read_text(encoding="utf-8")
    assert "## Fusion" in text
    assert "Picks: a:eq, b:dynamics" in text


# ---------------------------------------------------------------------------
# Rejections (deterministic errors, exit code 2)
# ---------------------------------------------------------------------------


# ---------------------------------------------------------------------------
# Review findings (issue #105, dual-axis review)
# ---------------------------------------------------------------------------


def test_fusion_refuses_two_stages_of_the_same_kind(tmp_path):
    """Two picks of the same kind would leave one source block beside two
    stage summaries -> refused instead of silently dropping one."""
    doc_a = _write_doc(tmp_path, "a.json", make_chain_doc_v3_clean())
    doc_b = _write_doc(tmp_path, "b.json", make_chain_doc_v3_artifact())
    out = tmp_path / "fused.json"

    proc = _run([doc_a, doc_b, "--pick", "a:eq", "--pick", "b:eq",
                 "--out", out])
    assert proc.returncode == 2
    assert "duplicate stage" in proc.stderr
    assert not out.exists()


def test_fusion_carries_the_not_exercised_verdict(tmp_path):
    """#100: "plugin did nothing under measurement" must survive fusion."""
    row = make_dyn_active_row()
    row["status"] = "not-exercised"
    doc = dc.build_chain_doc([row], _META)
    path = _write_doc(tmp_path, "not-exercised.json", doc)
    out = tmp_path / "fused.json"

    proc = _run([path, "--pick", "a:eq", "--pick", "a:nonlinearity",
                 "--out", out])
    assert proc.returncode == 0, proc.stderr

    plugin = json.loads(out.read_text(encoding="utf-8"))["plugins"][0]
    assert plugin["usable_as_spec"] is False
    assert any("not-exercised" in reason for reason in plugin["why_not_spec"])


def test_fusion_keeps_a_saturation_downgrade_usable(tmp_path):
    """#79: the source's own verdict decides — a conflict it did not treat as a
    blocker must not reappear as one in the fused document."""
    row = make_pro_c3_row()
    row["compression"] = make_compression_conflict()
    row["harmonic"] = make_harmonic_clean()
    doc = dc.build_chain_doc([row], _META)
    path = _write_doc(tmp_path, "saturating.json", doc)
    out = tmp_path / "fused.json"

    proc = _run([path, "--pick", "a:dynamics", "--out", out])
    assert proc.returncode == 0, proc.stderr

    plugin = json.loads(out.read_text(encoding="utf-8"))["plugins"][0]
    assert plugin["usable_as_spec"] is True
    assert plugin["why_not_spec"] == []


def test_fusion_markdown_without_dynamics_claims_nothing_about_release(
        tmp_path):
    """A fusion that never picked dynamics must not render a release verdict."""
    doc_a = _write_doc(tmp_path, "a.json", make_chain_doc_v3_clean())
    out = tmp_path / "fused.json"
    md = tmp_path / "fused.md"

    proc = _run([doc_a, "--pick", "a:eq", "--out", out, "--markdown", md])
    assert proc.returncode == 0, proc.stderr

    text = md.read_text(encoding="utf-8")
    assert "not picked in fusion" in json.dumps(
        json.loads(out.read_text(encoding="utf-8")))
    assert "release implausible" not in text


def test_fusion_reports_a_write_error_without_half_output(tmp_path):
    """An unwritable --markdown path is a usage error (exit 2), and the JSON is
    not left behind as a half product."""
    doc_a = _write_doc(tmp_path, "a.json", make_chain_doc_v3_clean())
    out = tmp_path / "fused.json"

    proc = _run([doc_a, "--pick", "a:eq", "--out", out,
                 "--markdown", tmp_path / "missing-dir" / "x.md"])
    assert proc.returncode == 2
    assert "cannot write output" in proc.stderr
    assert not out.exists()


def test_fusion_keeps_paths_relative_to_the_invocation(tmp_path):
    """Fused documents record the path as written by the caller — an absolute
    local layout must not leak into a committed spec."""
    _write_doc(tmp_path, "a.json", make_chain_doc_v3_clean())

    # Run from the input's directory with a plain relative name.
    proc = subprocess.run([sys.executable, str(_SCRIPT), "a.json", "--pick",
                           "a:eq", "--out", "fused.json"],
                          capture_output=True, text=True, cwd=str(tmp_path))
    assert proc.returncode == 0, proc.stderr

    fused = json.loads((tmp_path / "fused.json").read_text(encoding="utf-8"))
    assert fused["fusion"]["inputs"][0]["path"] == "a.json"


def test_fusion_rejects_unknown_stage(tmp_path):
    """A stage outside eq/dynamics/nonlinearity is refused by name."""
    doc_a = _write_doc(tmp_path, "a.json", make_chain_doc_v3_clean())
    out = tmp_path / "fused.json"

    proc = _run([doc_a, "--pick", "a:reverb", "--out", out])
    assert proc.returncode == 2
    assert "reverb" in proc.stderr
    assert not out.exists()


def test_fusion_rejects_stage_absent_from_its_input(tmp_path):
    """Picking a stage the input never measured is refused, not invented."""
    # An artifact doc legitimately carries no usable segment: stages [] is
    # valid there (it never claims to be spec-usable).
    doc_a = _write_doc(tmp_path, "a.json", upgrade_to_v3(
        make_chain_doc_artifact(), stages=[]))
    out = tmp_path / "fused.json"

    proc = _run([doc_a, "--pick", "a:eq", "--out", out])
    assert proc.returncode == 2
    assert "no 'eq' stage" in proc.stderr
    assert not out.exists()


def test_fusion_rejects_unknown_alias(tmp_path):
    """An alias that names no input is refused."""
    doc_a = _write_doc(tmp_path, "a.json", make_chain_doc_v3_clean())
    out = tmp_path / "fused.json"

    proc = _run([doc_a, "--pick", "z:eq", "--out", out])
    assert proc.returncode == 2
    assert "names no input" in proc.stderr


def test_fusion_rejects_missing_input(tmp_path):
    """A missing input path exits 2 with an error, writing nothing."""
    out = tmp_path / "fused.json"

    proc = _run([tmp_path / "nope.json", "--pick", "a:eq", "--out", out])
    assert proc.returncode == 2
    assert not out.exists()


def test_fusion_rejects_invalid_chain_doc(tmp_path):
    """An input that is not a valid chain_doc is refused with the reason."""
    bad = tmp_path / "bad.json"
    bad.write_text(json.dumps({"plugins": [{"slug": "x"}]}), encoding="utf-8")
    out = tmp_path / "fused.json"

    proc = _run([bad, "--pick", "a:eq", "--out", out])
    assert proc.returncode == 2
    assert "invalid" in proc.stderr.lower()
    assert not out.exists()


def test_fusion_rejects_multi_plugin_input(tmp_path):
    """An input document describing several plugins is ambiguous -> refused."""
    doc = make_chain_doc_v3_clean()
    second = json.loads(json.dumps(doc["plugins"][0]))
    second["slug"] = "other"
    doc["plugins"].append(second)
    path = _write_doc(tmp_path, "multi.json", doc)
    out = tmp_path / "fused.json"

    proc = _run([path, "--pick", "a:eq", "--out", out])
    assert proc.returncode == 2
    assert "one plugin" in proc.stderr
    assert not out.exists()


def test_fusion_rejects_no_picks(tmp_path):
    """A fusion with no --pick has nothing to fuse -> refused."""
    doc_a = _write_doc(tmp_path, "a.json", make_chain_doc_v3_clean())
    out = tmp_path / "fused.json"

    proc = _run([doc_a, "--out", out])
    assert proc.returncode == 2
    assert "pick" in proc.stderr.lower()
