"""test_describe_chain.py — Tests for describe_chain.py (chain_doc builders).

Wave-2 Task T4 of issue #26: the processing-chain description generator.
Given the aggregate_report rows (tools/aggregate_report.py) plus optional
per-plugin dataset.json enrichment, describe_chain builds the chain_doc
per-plugin contract consumed by describe_render.py (and the T5 validator):

    {slug, plugin, plugin_type:{kind,confidence,basis[]},
     eq:{present,overall,sections[],notes[]},
     dynamics:{present,compression:{...},gr:{...},notes[]},
     nonlinearity:{verdict,thd_range_pct?,description?,reason?},
     processing_order:{order,confidence,basis[],suggested?,suggestion_note?},
     usable_as_spec:bool, why_not_spec[]}

Tests follow the red→green discipline: fixtures come from
test_data/chain_fixtures.py (deterministic builders), tmp_path is the
pytest builtin for dataset.json enrichment scenarios, and the CLI is
exercised via subprocess (sys.executable).

Runs with pytest from the repo root:
    python -m pytest tools/test_describe_chain.py -q
"""

import json
import subprocess
import sys
from pathlib import Path

# The tools/ scripts are not a package (no __init__.py); import by path.
sys.path.insert(0, str(Path(__file__).resolve().parent))

import describe_chain as dc  # noqa: E402
import describe_quality as dq  # noqa: E402
from describe_schema import validate_chain_doc  # noqa: E402
from test_data.chain_fixtures import (  # noqa: E402
    make_analyzer_snapshot,
    make_gem_eqp_row,
    make_gem_eqp_snapshot,
    make_ozone_eq_row,
    make_ozone_eq_snapshot,
    make_compression_clean,
    make_compression_conflict,
    make_compression_degenerate,
    make_compression_unity_threshold_mismatch,
    make_compressor_snapshot,
    make_dyn_active_row,
    make_eq_dynamic_snapshot,
    make_eq_unused_snapshot,
    make_freq_artifact,
    make_freq_clean,
    make_freq_degenerate,
    make_gr_clean,
    make_gr_implausible,
    make_gr_invalid,
    make_harmonic_artifact,
    make_harmonic_clean,
    make_harmonic_none,
    make_multiband_comp_snapshot,
    make_pro_c3_row,
    make_pro_q4_measured_row,
    make_pro_q4_row,
    make_saturation_snapshot,
    make_scepter_row,
)

_META = {"generated_at": "2026-08-13T00:00:00+00:00",
         "aggregate_report": "aggregate_report.json",
         "report_generated_at": "2026-08-13T00:00:00+00:00"}

_SCRIPT = Path(__file__).resolve().parent / "describe_chain.py"


# ---------------------------------------------------------------------------
# enrich_from_dataset
# ---------------------------------------------------------------------------


def _write_dataset(tmp_path, payload):
    path = tmp_path / "dataset.json"
    path.write_text(json.dumps(payload), encoding="utf-8")
    return path


def test_enrich_minimal_dataset(tmp_path):
    """A well-formed dataset.json yields the enriched context fields."""
    snapshot = make_analyzer_snapshot()
    path = _write_dataset(tmp_path, {
        "context": {"parameter_snapshot": snapshot, "sample_rate": 48000},
        "harmonic": {"tones": [{"freq": 100.0}]},
        "compression_family": True,
    })
    result = dc.enrich_from_dataset(str(path))
    assert result["parameter_snapshot"] == snapshot
    assert result["harmonic_block"] == [{"freq": 100.0}]
    assert result["sample_rate"] == 48000.0
    assert isinstance(result["sample_rate"], float)
    assert result["has_compression_family"] is True


def test_enrich_sample_rate_fallback(tmp_path):
    """context.sample_rate missing → context.measurement.sample_rate."""
    path = _write_dataset(tmp_path, {
        "context": {"measurement": {"sample_rate": 96000}},
        "harmonic": {"tones": []},
    })
    result = dc.enrich_from_dataset(str(path))
    assert result["sample_rate"] == 96000.0
    assert result["has_compression_family"] is False
    assert result["parameter_snapshot"] is None


def test_enrich_missing_file(tmp_path):
    """Missing dataset.json → all-None/false defensive shape, never raises."""
    result = dc.enrich_from_dataset(str(tmp_path / "nope" / "dataset.json"))
    assert result == {"parameter_snapshot": None, "harmonic_block": None,
                      "sample_rate": None, "has_compression_family": False}


def test_enrich_corrupt_json(tmp_path):
    """Unparseable JSON → all-None/false defensive shape, never raises."""
    path = tmp_path / "dataset.json"
    path.write_text("{not json", encoding="utf-8")
    result = dc.enrich_from_dataset(str(path))
    assert result == {"parameter_snapshot": None, "harmonic_block": None,
                      "sample_rate": None, "has_compression_family": False}


# ---------------------------------------------------------------------------
# classify_plugin_type
# ---------------------------------------------------------------------------


def test_classify_analyzer_snapshot():
    """FFT Size / Hold Peaks / Smoothing keys → analyzer, high confidence."""
    result = dc.classify_plugin_type(make_analyzer_snapshot())
    assert result["kind"] == "analyzer"
    assert result["confidence"] == "high"
    assert result["basis"]


def test_classify_compressor_snapshot():
    """Threshold + Ratio + Attack/Release keys → compressor."""
    result = dc.classify_plugin_type(make_compressor_snapshot())
    assert result["kind"] == "compressor"
    assert result["confidence"] == "high"


def test_classify_eq_unused_snapshot():
    """Band keys with no dynamics keys → eq-only; basis notes unused bands."""
    result = dc.classify_plugin_type(make_eq_unused_snapshot())
    assert result["kind"] == "eq-only"
    assert any("unused" in item.lower() for item in result["basis"])


def test_classify_eq_dynamic_snapshot():
    """Band keys + per-band dynamics (Attack/Release/Threshold), no ratio
    → eq-dynamics hybrid kind, high confidence, basis notes all bands
    unused."""
    result = dc.classify_plugin_type(make_eq_dynamic_snapshot())
    assert result["kind"] == "eq-dynamics"
    assert result["confidence"] == "high"
    assert any("unused" in item.lower() for item in result["basis"])


def test_classify_multiband_comp_snapshot():
    """Band keys + per-band Ratio → compressor (ratio is the compressor
    tell), never eq-dynamics; basis notes both families detected."""
    result = dc.classify_plugin_type(make_multiband_comp_snapshot())
    assert result["kind"] == "compressor"
    assert result["kind"] != "eq-dynamics"
    assert result["confidence"] == "high"
    assert any("EQ band keys" in item for item in result["basis"])


def test_classify_no_snapshot():
    """None / {} snapshot → unknown, low confidence, explicit basis."""
    for snapshot in (None, {}):
        result = dc.classify_plugin_type(snapshot)
        assert result["kind"] == "unknown"
        assert result["confidence"] == "low"
        assert "no parameter snapshot" in result["basis"]


def test_classify_processor_heuristic():
    """No snapshot + row with >= 2 real-content blocks → processor heuristic."""
    result = dc.classify_plugin_type(None, row=make_pro_c3_row())
    assert result["kind"] == "processor"
    assert result["confidence"] == "low"
    assert any("processor" in item.lower() for item in result["basis"])


def test_classify_unknown_row_with_one_block():
    """Degenerate row (single real block) + no snapshot → still unknown."""
    result = dc.classify_plugin_type(None, row=make_scepter_row())
    assert result["kind"] == "unknown"


def test_classify_saturation_snapshot():
    """Generic non-linear keys (Power/Machine/Param 1) → saturation, low
    confidence (weak parameter-face evidence alone), basis names the keys."""
    result = dc.classify_plugin_type(make_saturation_snapshot())
    assert result["kind"] == "saturation"
    assert result["confidence"] == "low"
    assert any("Power" in item for item in result["basis"])


def test_classify_eq_dynamic_measured_not_exercised():
    """eq-dynamics snapshot + measurement showing NO dynamics exercised
    (both compression fits unity + invalid GR) → confidence low, basis
    notes the dynamics were not exercised (issue #73)."""
    result = dc.classify_plugin_type(make_eq_dynamic_snapshot(),
                                     row=make_pro_q4_measured_row())
    assert result["kind"] == "eq-dynamics"
    assert result["confidence"] == "low"
    assert any("not exercised" in item for item in result["basis"])


def test_classify_eq_dynamic_measured_active():
    """eq-dynamics snapshot + measurement with ACTIVE dynamics (ratio 2.5,
    valid GR) → confidence stays high (issue #73)."""
    result = dc.classify_plugin_type(make_eq_dynamic_snapshot(),
                                     row=make_dyn_active_row())
    assert result["kind"] == "eq-dynamics"
    assert result["confidence"] == "high"
    assert all("not exercised" not in item for item in result["basis"])


# ---------------------------------------------------------------------------
# Issue #94: generic parameter names must not decide the plugin type
# ---------------------------------------------------------------------------


def test_classify_generic_gain_keys_never_dynamics():
    """Issue #94: the Ozone 12 Equalizer surface names its 8 EQ bands
    "EQ: <channel> Gain <n>" — the generic word Gain is NOT dynamics
    evidence (no Threshold/Ratio/Attack/Release anywhere). Names alone
    abstain: unknown, low confidence, and the basis says why."""
    result = dc.classify_plugin_type(make_ozone_eq_snapshot())
    assert result["kind"] != "dynamics-only"
    assert result["kind"] == "unknown"
    assert result["confidence"] == "low"
    assert any("Gain" in item and "generic" in item.lower()
               for item in result["basis"])


def test_classify_generic_power_keys_never_saturation():
    """Issue #94: the Gem EQP surface only exposes Power switches (Power /
    EQP Power / HPF Power / LPF Power) — a generic power switch is NOT
    saturation evidence. Names alone abstain: unknown, low confidence."""
    result = dc.classify_plugin_type(make_gem_eqp_snapshot())
    assert result["kind"] != "saturation"
    assert result["kind"] == "unknown"
    assert result["confidence"] == "low"
    assert any("Power" in item and "generic" in item.lower()
               for item in result["basis"])


def test_classify_gem_eqp_measured_eq_from_frequency_response():
    """Issue #94 acceptance 2: Gem EQP's measured freq curve is a real,
    plausible EQ bell (-30.8 .. +8.75 dB) — when the names abstain, the
    measurement decides: eq-only, basis names the measured curve."""
    result = dc.classify_plugin_type(make_gem_eqp_snapshot(),
                                     row=make_gem_eqp_row())
    assert result["kind"] == "eq-only"
    assert result["confidence"] == "low"
    assert any("frequency response" in item.lower() for item in result["basis"])


def test_classify_measurement_fallback_requires_a_plausible_fit():
    """Issue #94: the measurement fallback only fires on a plausible EQ fit —
    an implausible single-peak fit (pro-c-3 Q 18098) stays unknown."""
    row = make_gem_eqp_row()
    row["freq"] = make_freq_artifact()
    result = dc.classify_plugin_type(make_gem_eqp_snapshot(), row=row)
    assert result["kind"] == "unknown"
    assert result["confidence"] == "low"


def test_measured_dynamics_beats_the_eq_fallback():
    """Issue #94 review: abstaining on generic names must not turn a working
    compressor into an EQ plugin. A usable compression fit (ratio away from
    unity) plus a valid GR edge is direct dynamic evidence, even when a freq
    fit is also present."""
    row = {
        "compression": {"status": "ok", "ratio": 4.0, "threshold_db": -20.0},
        "gr": {"valid": True, "attack_ms": 10.0, "release_ms": 100.0},
        "freq": {"status": "ok", "freq_hz": 1000.0, "gain_db": 6.0, "q": 1.0},
    }
    result = dc.classify_plugin_type({"Output Gain": 0.5}, row=row)
    assert result["kind"] == "dynamics-only"
    assert any("dynamics evidence" in item.lower() for item in result["basis"])
    assert not any("EQ peak" in item for item in result["basis"])


def test_peak_above_nyquist_is_not_eq_evidence():
    """Issue #94 review: the fallback must judge peaks exactly like build_eq.
    A 60 kHz peak at 48 kHz is out of range there, so it cannot be EQ evidence
    here — otherwise plugin_type and the eq section contradict each other."""
    row = {"freq": {"status": "ok", "freq_hz": 60000.0, "gain_db": 6.0, "q": 1.0}}
    above = dc.classify_plugin_type({"Knob A": 0.5}, row=row, sample_rate=48000.0)
    assert above["kind"] == "unknown"
    # The same peak inside the band is still EQ evidence.
    row["freq"]["freq_hz"] = 1000.0
    inside = dc.classify_plugin_type({"Knob A": 0.5}, row=row, sample_rate=48000.0)
    assert inside["kind"] == "eq-only"


def test_passthrough_never_typed_from_a_stale_freq_section():
    """Issue #94 review: the passthrough guard must win over the measurement
    fallback. A not-exercised row can still carry a stale (plausible-looking)
    freq section; typing it as eq-only would contradict the measurement that
    says the plugin did nothing."""
    row = make_gem_eqp_row()
    row["status"] = "not-exercised"
    result = dc.classify_plugin_type(make_gem_eqp_snapshot(), row=row)
    assert result["kind"] == "unknown"
    assert not any("frequency response" in item.lower()
                   for item in result["basis"])
    assert any("not exercised" in item.lower() for item in result["basis"])


def test_classify_ozone_eq_passthrough_not_exercised():
    """Issue #94 acceptance 1 + requirement 4: Ozone 12 Equalizer measured a
    headless passthrough (freq flat, compression unity, invalid GR) — the
    classifier must say the processing was not exercised (issue #73 pattern)
    instead of labelling it dynamics-only."""
    result = dc.classify_plugin_type(make_ozone_eq_snapshot(),
                                     row=make_ozone_eq_row())
    assert result["kind"] != "dynamics-only"
    assert result["kind"] == "unknown"
    assert result["confidence"] == "low"
    assert any("not exercised" in item for item in result["basis"])


def _write_snapshot_dataset(tmp_path, slug, snapshot):
    """Write one plugin's dataset.json carrying just the parameter snapshot."""
    plugin_dir = tmp_path / slug
    plugin_dir.mkdir()
    (plugin_dir / "dataset.json").write_text(json.dumps({
        "context": {"parameter_snapshot": snapshot, "sample_rate": 48000},
        "harmonic": {"tones": []},
    }), encoding="utf-8")


def test_classify_passthrough_row_never_typed_from_measurement():
    """Issue #94: a not-exercised row (issue #100) carries no discriminative
    data, so even a plausible freq fit must not type it — the passthrough
    verdict wins over the measurement fallback."""
    row = make_gem_eqp_row()
    row["status"] = "not-exercised"
    result = dc.classify_plugin_type(make_gem_eqp_snapshot(), row=row)
    assert result["kind"] == "unknown"
    assert any("not exercised" in item for item in result["basis"])


def test_chain_doc_gem_eqp_is_an_eq_plugin(tmp_path):
    """Issue #94 acceptance 2 end-to-end: Gem EQP's chain_doc entry is typed
    eq-only from its measured curve, with a clean EQ section."""
    _write_snapshot_dataset(tmp_path, "gem-eqp", make_gem_eqp_snapshot())
    plugin = dc.build_chain_doc([make_gem_eqp_row()], _META,
                                dataset_dir=str(tmp_path))["plugins"][0]
    assert plugin["plugin_type"]["kind"] == "eq-only"
    assert plugin["plugin_type"]["confidence"] == "low"
    assert plugin["eq"]["overall"] == "clean"
    assert plugin["usable_as_spec"] is True, plugin["why_not_spec"]


def test_chain_doc_ozone_eq_passthrough_not_usable(tmp_path):
    """Issue #94 acceptance 1 end-to-end: Ozone 12 Equalizer is typed unknown
    (the parameter names abstain) and never ships as a usable spec — the
    measurement is a headless passthrough."""
    _write_snapshot_dataset(tmp_path, "ozone-12-equalizer",
                            make_ozone_eq_snapshot())
    plugin = dc.build_chain_doc([make_ozone_eq_row()], _META,
                                dataset_dir=str(tmp_path))["plugins"][0]
    assert plugin["plugin_type"]["kind"] == "unknown"
    assert plugin["plugin_type"]["confidence"] == "low"
    assert plugin["usable_as_spec"] is False
    assert any("not-exercised" in reason for reason in plugin["why_not_spec"])


# ---------------------------------------------------------------------------
# build_eq
# ---------------------------------------------------------------------------


def test_build_eq_artifact():
    """pro-c-3 Q=18098.4 → overall artifact, implausible section with flag."""
    eq = dc.build_eq(make_freq_artifact(), {})
    assert eq["present"] is True
    assert eq["overall"] == "artifact"
    assert len(eq["sections"]) == 1
    section = eq["sections"][0]
    assert section["plausible"] is False
    assert section["flag"] == "q_out_of_range"
    assert section["reason"] is not None
    assert section["freq_hz"] == 18840.8


def test_build_eq_artifact_non_eq_downgraded():
    """Issue #78: an implausible freq fit on a NON-EQ plugin (compressor
    snapshot) is a single-peak-model mismatch, not an EQ artifact — downgrade
    to present False / overall none so usable_as_spec is not blocked."""
    eq = dc.build_eq(make_freq_artifact(),
                     {"parameter_snapshot": make_compressor_snapshot()})
    assert eq["present"] is False
    assert eq["overall"] == "none"
    assert eq["sections"] == []
    assert any("not applicable" in note for note in eq["notes"])


def test_build_eq_artifact_eq_kept():
    """Issue #78: an implausible fit on an EQ-plugin (eq-dynamics snapshot)
    IS an EQ artifact — kept, so the unreliable EQ spec is not emitted."""
    eq = dc.build_eq(make_freq_artifact(),
                     {"parameter_snapshot": make_eq_dynamic_snapshot()})
    assert eq["present"] is True
    assert eq["overall"] == "artifact"
    assert len(eq["sections"]) == 1
    assert eq["sections"][0]["plausible"] is False


def test_build_eq_saturation_downgraded():
    """Issue #78: saturation plugin (uadx-vibe gain 31.61 dB out of range)
    → none, not artifact."""
    eq = dc.build_eq({"freq_hz": 18858.4, "gain_db": 31.61, "q": None,
                      "status": "ok"},
                     {"parameter_snapshot": make_saturation_snapshot()})
    assert eq["present"] is False
    assert eq["overall"] == "none"


def test_build_eq_clean():
    """Sane bell → overall clean, plausible section, nyquist from ctx."""
    eq = dc.build_eq(make_freq_clean(), {"sample_rate": 48000.0})
    assert eq["present"] is True
    assert eq["overall"] == "clean"
    assert eq["sections"][0]["plausible"] is True
    assert eq["sections"][0]["flag"] is None


def test_build_eq_degenerate():
    """status degenerate → present False, overall none, no sections."""
    eq = dc.build_eq(make_freq_degenerate(), {})
    assert eq["present"] is False
    assert eq["overall"] == "none"
    assert eq["sections"] == []


def test_build_eq_missing_row():
    """freq_row None → present False, overall none."""
    eq = dc.build_eq(None, {})
    assert eq["present"] is False
    assert eq["overall"] == "none"
    assert eq["notes"]


# ---------------------------------------------------------------------------
# build_dynamics
# ---------------------------------------------------------------------------


def test_build_dynamics_conflict():
    """pro-c-3: derived vs json threshold conflict; release implausible."""
    dynamics = dc.build_dynamics(make_compression_conflict(),
                                 make_gr_implausible(), {})
    assert dynamics["present"] is True
    compression = dynamics["compression"]
    assert compression["threshold_derived"] == -13.47
    assert compression["ratio_derived"] == 3.27
    assert compression["threshold_json"] == -9.03
    assert compression["ratio_json"] == 3.33
    assert compression["knee_json"] == 3.0
    assert compression["conflict"] is True
    # conflict_note must describe both fits so a reader can judge.
    assert "-13.47" in compression["conflict_note"]
    assert "-9.03" in compression["conflict_note"]
    gr = dynamics["gr"]
    assert gr["attack_ms"] == 3.859
    assert gr["release_ms"] == 39676.69
    assert gr["attack_plausible"] is True
    assert gr["release_plausible"] is False
    assert gr["release_flag"] == "release_implausible"
    assert gr["note"] is not None


def test_build_dynamics_clean():
    """Consistent fits + sane GR → no conflict, everything plausible."""
    dynamics = dc.build_dynamics(make_compression_clean(), make_gr_clean(), {})
    assert dynamics["present"] is True
    assert dynamics["compression"]["conflict"] is False
    assert dynamics["compression"]["conflict_note"] is None
    assert dynamics["compression"]["threshold_json"] == -30.0
    assert dynamics["gr"]["attack_plausible"] is True
    assert dynamics["gr"]["release_plausible"] is True
    assert dynamics["gr"]["release_flag"] is None


def test_build_dynamics_absent():
    """Degenerate compression + invalid GR — present False, no fits."""
    dynamics = dc.build_dynamics(make_compression_degenerate(),
                                 make_gr_invalid(), {})
    assert dynamics["present"] is False
    compression = dynamics["compression"]
    assert compression["threshold_derived"] is None
    assert compression["conflict"] is False
    assert compression["threshold_json"] is None
    assert dynamics["gr"]["release_plausible"] is False
    assert dynamics["notes"]


def test_build_dynamics_unity_ratio_threshold_mismatch():
    """Issue #28 T5: an EQ-only plugin (pro-q-4 bell) fits ratio 1.0 on BOTH
    sides — no compression evidence — so the unidentifiable threshold fit
    difference must NOT be a conflict. Downgraded to a note; the unusable GR
    section carries section_usable=False so spec gating can skip it."""
    dynamics = dc.build_dynamics(make_compression_unity_threshold_mismatch(),
                                 make_gr_invalid(), {})
    assert dynamics["present"] is True
    compression = dynamics["compression"]
    assert compression["ratio_derived"] == 1.0
    assert compression["ratio_json"] == 1.0
    assert compression["conflict"] is False
    assert compression["conflict_note"] is None
    assert any("no compression" in note for note in dynamics["notes"])
    gr = dynamics["gr"]
    assert gr["section_usable"] is False


def test_why_not_spec_eq_only_usable():
    """Issue #28 T5 end-to-end: a clean bell EQ with unity-ratio compression
    and a degenerate GR section yields NO why_not_spec reasons -> the
    chain_doc is usable as a VST development spec."""
    row = {"slug": "pro-q-4", "plugin": "Pro-Q 4", "has_freq": True,
           "has_compression": True, "has_gr": True, "has_harmonic": True,
           "freq": make_freq_clean(),
           "compression": make_compression_unity_threshold_mismatch(),
           "gr": make_gr_invalid(),
"harmonic": make_harmonic_clean(),
            "status": "ok"}
    doc = dc.build_chain_doc([row], _META)
    plugin = doc["plugins"][0]
    assert plugin["eq"]["overall"] == "clean"
    assert plugin["dynamics"]["compression"]["conflict"] is False
    assert plugin["usable_as_spec"] is True, plugin["why_not_spec"]
    assert plugin["why_not_spec"] == []


def _conflict_dynamics():
    """A dynamics block with a compression fit conflict + implausible
    release on a usable GR section (the pro-c-3 / uadx-vibe shape)."""
    return {"present": True,
            "compression": {"threshold_derived": -13.47, "ratio_derived": 3.27,
                            "threshold_json": -9.03, "ratio_json": 3.33,
                            "knee_json": 3.0, "conflict": True,
                            "conflict_note": "test conflict"},
            "gr": {"attack_ms": 1.0, "release_ms": 39676.69,
                   "attack_plausible": True, "release_plausible": False,
                   "section_usable": True, "note": None},
            "notes": []}


def test_why_not_spec_saturating_downgraded():
    """Issue #79: high-THD saturation (THD max 2.1%) with a compression fit
    conflict + implausible release → NOT a spec blocker; a saturator's
    compression curve is a mis-fit, not evidence against the spec."""
    dynamics = _conflict_dynamics()
    nonlinearity = {"verdict": "clean", "thd_range_pct": [0.8, 2.1],
                    "description": "THD 0.8-2.1%", "reason": None}
    eq = {"present": False, "overall": "none", "sections": [], "notes": []}
    reasons = dc._why_not_spec(eq, dynamics, nonlinearity)
    assert "compression fit conflict" not in reasons
    assert "release implausible" not in reasons
    assert reasons == []


def test_why_not_spec_compressor_conflict_kept():
    """Issue #79: low-THD (max 0.5%) — the compressor profile — a fit
    conflict and implausible release DO block the spec."""
    dynamics = _conflict_dynamics()
    nonlinearity = {"verdict": "clean", "thd_range_pct": [0.01, 0.5],
                    "description": "THD 0.01-0.5%", "reason": None}
    eq = {"present": False, "overall": "none", "sections": [], "notes": []}
    reasons = dc._why_not_spec(eq, dynamics, nonlinearity)
    assert "compression fit conflict" in reasons
    assert "release implausible" in reasons


# ---------------------------------------------------------------------------
# build_nonlinearity
# ---------------------------------------------------------------------------


def test_build_nonlinearity_artifact():
    """pro-c-3 96.6-195.9% THD → verdict artifact with a reason."""
    result = dc.build_nonlinearity(make_harmonic_artifact(), set())
    assert result["verdict"] == "artifact"
    assert result["thd_range_pct"] == [96.58, 195.8767]
    assert result["reason"]


def test_build_nonlinearity_clean():
    """Clean 0.8-2.1% THD → verdict clean, description carries the range."""
    result = dc.build_nonlinearity(make_harmonic_clean(), set())
    assert result["verdict"] == "clean"
    assert result["thd_range_pct"] == [0.8, 2.1]
    assert "THD" in result["description"]
    assert result["reason"] is None


def test_build_nonlinearity_not_measured():
    """No tones → verdict not-measured, no range/description."""
    result = dc.build_nonlinearity(make_harmonic_none(), set())
    assert result["verdict"] == "not-measured"
    assert result["thd_range_pct"] is None
    assert result["description"] is None


def test_build_nonlinearity_shared_fingerprint():
    """Clean THD but harmonic_raw in the shared-fingerprint set → artifact."""
    raw = [{"fundamental_hz": 1000.0, "thd_percent": 1.0,
            "dominant_order": 2, "dominant_mag_db": -40.0}]
    groups = dq.detect_duplicate_fingerprints(
        [{"slug": "a", "harmonic_raw": raw},
         {"slug": "b", "harmonic_raw": raw}])
    assert groups  # the fingerprint is genuinely shared
    row = dict(make_harmonic_clean())
    row["harmonic_raw"] = raw
    result = dc.build_nonlinearity(row, set(groups))
    assert result["verdict"] == "artifact"
    assert "fingerprint" in result["reason"]


def test_build_nonlinearity_artifact_shared_fingerprint():
    """Artifact THD + harmonic_raw in the shared-fingerprint set keeps the
    artifact verdict but ALSO names the shared fingerprint in the reason."""
    raw = [{"fundamental_hz": 100.3, "thd_percent": 195.8,
            "dominant_order": 2, "dominant_mag_db": 61.4}]
    groups = dq.detect_duplicate_fingerprints(
        [{"slug": "a", "harmonic_raw": raw},
         {"slug": "b", "harmonic_raw": raw}])
    assert groups  # the fingerprint is genuinely shared
    row = dict(make_harmonic_artifact())
    row["harmonic_raw"] = raw
    result = dc.build_nonlinearity(row, set(groups))
    assert result["verdict"] == "artifact"
    assert "fingerprint" in result["reason"]


def test_build_nonlinearity_zero_thd_shared_fingerprint():
    """Issue #86: clean-passthrough plugins (scepter / auto-key-2) share an
    identical zero-THD fingerprint (all thd == 0.0) — that is legitimate
    clean behavior, NOT the rig-artifact shared-chain signal. Verdict must
    stay clean, with an explanatory reason."""
    raw = [{"fundamental_hz": 100.0, "thd_percent": 0.0,
            "dominant_order": 2, "dominant_mag_db": -60.0},
           {"fundamental_hz": 200.0, "thd_percent": 0.0,
            "dominant_order": 3, "dominant_mag_db": -70.0}]
    groups = dq.detect_duplicate_fingerprints(
        [{"slug": "a", "harmonic_raw": raw},
         {"slug": "b", "harmonic_raw": raw}])
    assert groups  # the fingerprint is genuinely shared
    row = {"tones_count": 2, "status": "ok",
           "summary": [dict(t) for t in raw], "harmonic_raw": raw}
    result = dc.build_nonlinearity(row, set(groups))
    assert result["verdict"] == "clean"
    assert result["reason"] is not None
    assert "zero-THD" in result["reason"]


# ---------------------------------------------------------------------------
# infer_order
# ---------------------------------------------------------------------------


def test_infer_order_unknown_with_heuristic():
    """Order stays unknown; decisive compressor snapshot adds a basis line."""
    plugin_type = {"kind": "compressor", "confidence": "high", "basis": []}
    result = dc.infer_order(plugin_type, {"present": True},
                            {"present": True}, make_compressor_snapshot())
    assert result["order"] == "unknown"
    assert result["confidence"] == "low"
    assert result["suggested"] == "eq -> dyn -> eq"
    assert "not measured" in result["suggestion_note"]
    assert any("dyn-only" in item for item in result["basis"])


def test_infer_order_basis_evidence_only():
    """No snapshot → basis is pure evidence, no heuristic line."""
    plugin_type = {"kind": "unknown", "confidence": "low", "basis": []}
    result = dc.infer_order(plugin_type, {"present": False},
                            {"present": False}, None)
    assert result["order"] == "unknown"
    assert any("plugin type" in item for item in result["basis"])
    assert any("eq: absent" in item for item in result["basis"])
    assert not any("dyn-only" in item for item in result["basis"])


# ---------------------------------------------------------------------------
# build_chain_doc
# ---------------------------------------------------------------------------


def test_build_chain_doc_proc3_without_dataset():
    """pro-c-3 row alone, no enrichment → artifact verdicts, not spec-usable."""
    doc = dc.build_chain_doc([make_pro_c3_row()], _META)
    assert doc["generated_at"] == _META["generated_at"]
    assert doc["source"] == {"aggregate_report": "aggregate_report.json",
                             "dataset_dir": None,
                             "report_generated_at": _META["report_generated_at"]}
    plugin = doc["plugins"][0]
    assert plugin["slug"] == "pro-c-3"
    assert plugin["eq"]["overall"] == "artifact"
    assert plugin["dynamics"]["compression"]["conflict"] is True
    assert plugin["dynamics"]["gr"]["release_plausible"] is False
    assert plugin["nonlinearity"]["verdict"] == "artifact"
    assert plugin["processing_order"]["order"] == "unknown"
    assert plugin["usable_as_spec"] is False
    assert plugin["why_not_spec"] == ["eq artifact",
                                      "compression fit conflict",
                                      "release implausible",
                                      "harmonic artifact"]


def test_build_chain_doc_two_plugins_with_dataset(tmp_path):
    """scepter enriched from dataset.json → analyzer; order deterministic."""
    scepter_dir = tmp_path / "scepter"
    scepter_dir.mkdir()
    (scepter_dir / "dataset.json").write_text(json.dumps({
        "context": {"parameter_snapshot": make_analyzer_snapshot(),
                    "sample_rate": 48000},
        "harmonic": {"tones": []},
    }), encoding="utf-8")
    doc = dc.build_chain_doc([make_pro_c3_row(), make_scepter_row()],
                             _META, dataset_dir=str(tmp_path))
    assert [p["slug"] for p in doc["plugins"]] == ["pro-c-3", "scepter"]
    assert doc["source"]["dataset_dir"] == str(tmp_path)
    # pro-c-3 has no dataset.json → missing-file defensive enrichment.
    assert doc["plugins"][0]["plugin_type"]["kind"] == "processor"
    assert doc["plugins"][0]["usable_as_spec"] is False
    assert doc["plugins"][1]["plugin_type"]["kind"] == "analyzer"
    assert doc["plugins"][1]["plugin_type"]["confidence"] == "high"
    # scepter shares the rig harmonic artifact → not spec-usable either.
    assert doc["plugins"][1]["usable_as_spec"] is False
    assert "harmonic artifact" in doc["plugins"][1]["why_not_spec"]


# ---------------------------------------------------------------------------
# main() CLI
# ---------------------------------------------------------------------------


def _write_report(tmp_path, rows):
    report = {"generated_at": _META["generated_at"], "out_dir": "out",
              "tolerances": {}, "counts": {}, "plugins": rows}
    path = tmp_path / "aggregate_report.json"
    path.write_text(json.dumps(report), encoding="utf-8")
    return path


def test_cli_success(tmp_path):
    """CLI against a real report → exit 0, both output files written."""
    report_path = _write_report(tmp_path,
                                [make_pro_c3_row(), make_scepter_row()])
    json_out = tmp_path / "chain_description.json"
    md_out = tmp_path / "chain_description.md"
    proc = subprocess.run(
        [sys.executable, str(_SCRIPT), str(report_path),
         "--json", str(json_out), "--markdown", str(md_out)],
        capture_output=True, text=True)
    assert proc.returncode == 0, proc.stderr
    assert json_out.is_file()
    assert md_out.is_file()
    doc = json.loads(json_out.read_text(encoding="utf-8"))
    assert doc["generated_at"] == _META["generated_at"]
    assert len(doc["plugins"]) == 2
    assert doc["plugins"][0]["slug"] == "pro-c-3"
    markdown = md_out.read_text(encoding="utf-8")
    assert "pro-c-3" in markdown
    assert "Spec-usable: no" in markdown


def test_cli_success_with_dataset_dir(tmp_path):
    """--dataset-dir enriches plugin types end-to-end through the CLI."""
    report_path = _write_report(tmp_path, [make_scepter_row()])
    scepter_dir = tmp_path / "scepter"
    scepter_dir.mkdir()
    (scepter_dir / "dataset.json").write_text(json.dumps({
        "context": {"parameter_snapshot": make_analyzer_snapshot(),
                    "sample_rate": 48000},
        "harmonic": {"tones": []},
    }), encoding="utf-8")
    json_out = tmp_path / "chain_description.json"
    md_out = tmp_path / "chain_description.md"
    proc = subprocess.run(
        [sys.executable, str(_SCRIPT), str(report_path),
         "--dataset-dir", str(tmp_path),
         "--json", str(json_out), "--markdown", str(md_out)],
        capture_output=True, text=True)
    assert proc.returncode == 0, proc.stderr
    doc = json.loads(json_out.read_text(encoding="utf-8"))
    assert doc["plugins"][0]["plugin_type"]["kind"] == "analyzer"


# ---------------------------------------------------------------------------
# v3 stages (issue #105): per-plugin segment decomposition + provenance
# ---------------------------------------------------------------------------


def test_chain_doc_declares_contract_version_3():
    """The assembled document carries the v3 contract version."""
    doc = dc.build_chain_doc([make_dyn_active_row()], _META)
    assert doc["contract_version"] == "3"


def test_stages_cover_every_present_segment_in_fixed_order():
    """All three measured segments appear, always in eq/dynamics/nonlinearity
    order (deterministic, never dependent on dict iteration)."""
    plugin = dc.build_chain_doc([make_dyn_active_row()], _META)["plugins"][0]
    assert [stage["kind"] for stage in plugin["stages"]] == [
        "eq", "dynamics", "nonlinearity"]


def test_eq_stage_params_carry_the_measured_band():
    """The eq stage repeats the fitted band so a consumer need not read eq."""
    stages = dc.build_chain_doc([make_dyn_active_row()], _META)["plugins"][0][
        "stages"]
    eq_stage = stages[0]
    assert eq_stage["kind"] == "eq"
    assert eq_stage["params"]["overall"] == "clean"
    assert eq_stage["params"]["sections"] == [
        {"freq_hz": 1000.0, "gain_db": 6.0, "q": 1.5}]


def test_dynamics_stage_params_carry_fits_and_taus():
    """The dynamics stage carries threshold/ratio/knee + attack/release."""
    stages = dc.build_chain_doc([make_dyn_active_row()], _META)["plugins"][0][
        "stages"]
    params = stages[1]["params"]
    assert stages[1]["kind"] == "dynamics"
    assert params["threshold_db"] == -30.0
    assert params["ratio"] == 4.0
    assert params["knee_db"] == 0.0
    assert params["attack_ms"] == 3.859
    assert params["release_ms"] == 50.0
    assert params["conflict"] is False


def test_nonlinearity_stage_params_carry_the_thd_range():
    """The nonlinearity stage carries the verdict + THD range."""
    stages = dc.build_chain_doc([make_dyn_active_row()], _META)["plugins"][0][
        "stages"]
    assert stages[2]["kind"] == "nonlinearity"
    assert stages[2]["params"] == {"verdict": "clean",
                                   "thd_range_pct": [0.8, 2.1]}


def test_stage_provenance_names_its_measurement_and_evidence():
    """Every stage records which measurement produced it plus non-empty,
    string-only evidence lines."""
    stages = dc.build_chain_doc([make_dyn_active_row()], _META)["plugins"][0][
        "stages"]
    assert [stage["provenance"]["source"] for stage in stages] == [
        "frequency_response", "compression+gr", "harmonic"]
    for stage in stages:
        basis = stage["provenance"]["basis"]
        assert basis
        assert all(isinstance(line, str) for line in basis)


def test_artifact_segment_keeps_its_verdict_in_the_stage():
    """An artifact segment is still described (with its verdict) — dropping
    it would silently lose the reason the plugin is not spec-usable."""
    plugin = dc.build_chain_doc([make_pro_c3_row()], _META)["plugins"][0]
    stages = {stage["kind"]: stage for stage in plugin["stages"]}
    assert stages["eq"]["params"]["overall"] == "artifact"
    assert stages["dynamics"]["params"]["conflict"] is True
    assert stages["nonlinearity"]["params"]["verdict"] == "artifact"


def test_degenerate_plugin_only_emits_the_measured_segment():
    """A plugin whose EQ/dynamics fits all failed emits no eq/dynamics stage."""
    plugin = dc.build_chain_doc([make_scepter_row()], _META)["plugins"][0]
    assert [stage["kind"] for stage in plugin["stages"]] == ["nonlinearity"]


def test_chain_doc_with_stages_passes_the_validator():
    """The generated v3 document validates against its own contract."""
    doc = dc.build_chain_doc(
        [make_dyn_active_row(), make_pro_c3_row(), make_scepter_row()], _META)
    assert validate_chain_doc(doc) == {"ok": True, "errors": []}


def test_stages_are_stable_across_two_builds():
    """Two builds from the same rows serialize byte-identically.

    Deliberately NOT sort_keys=True: key-insertion-order drift is exactly the
    property under test (issue #105 review, finding 3).
    """
    rows = [make_dyn_active_row(), make_pro_c3_row()]
    first = json.dumps(dc.build_chain_doc(rows, _META))
    second = json.dumps(dc.build_chain_doc(rows, _META))
    assert first == second


def test_stage_blockers_come_from_the_plugin_verdict():
    """Artifact segments carry the exact why_not_spec reasons that apply to
    them — the fused document inherits these, it never re-derives them."""
    plugin = dc.build_chain_doc([make_pro_c3_row()], _META)["plugins"][0]
    stages = {stage["kind"]: stage for stage in plugin["stages"]}

    assert stages["eq"]["blockers"] == ["eq artifact"]
    assert stages["dynamics"]["blockers"] == ["compression fit conflict",
                                              "release implausible"]
    assert stages["nonlinearity"]["blockers"] == ["harmonic artifact"]
    assert stages["eq"]["usable"] is False


def test_stage_blockers_are_empty_on_a_clean_plugin():
    """A spec-usable plugin's segments carry no blockers and are usable."""
    plugin = dc.build_chain_doc([make_dyn_active_row()], _META)["plugins"][0]
    assert plugin["usable_as_spec"] is True
    for stage in plugin["stages"]:
        assert stage["blockers"] == []
        assert stage["usable"] is True


def test_not_exercised_blocks_every_segment():
    """#100: a plugin that did nothing under measurement must not become a
    spec through fusion — the plugin-level reason lands on every segment."""
    row = make_dyn_active_row()
    row["status"] = "not-exercised"
    plugin = dc.build_chain_doc([row], _META)["plugins"][0]

    assert plugin["usable_as_spec"] is False
    assert plugin["stages"]
    for stage in plugin["stages"]:
        assert any("not-exercised" in blocker for blocker in stage["blockers"])
        assert stage["usable"] is False


def test_saturation_downgrade_keeps_the_dynamics_stage_usable():
    """#79: on a saturation-dominant plugin the compression-fit conflict does
    not block the spec, so the dynamics stage must not carry it either."""
    row = make_pro_c3_row()
    row["compression"] = make_compression_conflict()
    row["harmonic"] = make_harmonic_clean()   # THD up to 2.1% > saturation bound
    plugin = dc.build_chain_doc([row], _META)["plugins"][0]

    stages = {stage["kind"]: stage for stage in plugin["stages"]}
    assert "compression fit conflict" not in stages["dynamics"]["blockers"]
    assert stages["dynamics"]["blockers"] == []


def test_dynamics_stage_reports_no_knee_on_a_fit_conflict():
    """A knee only exists in the json fit: reporting it beside derived
    threshold/ratio would invent a parameter set that was never measured."""
    plugin = dc.build_chain_doc([make_pro_c3_row()], _META)["plugins"][0]
    stages = {stage["kind"]: stage for stage in plugin["stages"]}
    assert stages["dynamics"]["params"]["knee_db"] is None
    assert any("conflict" in line
               for line in stages["dynamics"]["provenance"]["basis"])


def test_cli_missing_input(tmp_path):
    """Missing report file → error on stderr, exit code 2."""
    proc = subprocess.run(
        [sys.executable, str(_SCRIPT), str(tmp_path / "missing.json")],
        capture_output=True, text=True)
    assert proc.returncode == 2
    assert "error" in proc.stderr.lower()
