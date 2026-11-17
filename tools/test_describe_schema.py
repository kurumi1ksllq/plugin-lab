"""test_describe_schema.py — Tests for describe_schema.validate_chain_doc.

Wave-2 Task T5 of issue #26: the chain_doc contract constants and the
structural validator the T4 consumer anchors on (CONTRACT_VERSION).

Fixtures come from test_data.chain_fixtures (make_chain_doc_clean /
make_chain_doc_artifact) so the tests exercise the exact contract shape
the real pipeline builds — never inline copies that can drift.

Validation is structural: artifacts are valid documents. Only missing
keys, wrong types and out-of-vocab values are errors.

Runs with pytest from the repo root:
    python -m pytest tools/test_describe_schema.py -q
"""

import copy

from describe_schema import (CONTRACT_VERSION, SANE_Q_MAX, THD_CLEAN_MAX_PCT,
                             validate_chain_doc)
from test_data.chain_fixtures import (make_chain_doc_artifact,
                                      make_chain_doc_clean,
                                      make_chain_doc_v3_artifact,
                                      make_chain_doc_v3_clean,
                                      make_fusion_block, upgrade_to_v3)


# ---------------------------------------------------------------------------
# Valid documents (clean + artifact) must pass
# ---------------------------------------------------------------------------


def test_clean_doc_valid():
    """make_chain_doc_clean() → ok True, no errors."""
    result = validate_chain_doc(make_chain_doc_clean())
    assert result == {"ok": True, "errors": []}


def test_artifact_doc_valid():
    """Artifacts are structurally valid docs — validation is structural,
    not a quality judgement."""
    result = validate_chain_doc(make_chain_doc_artifact())
    assert result["ok"] is True
    assert result["errors"] == []


def test_eq_dynamics_kind_accepted():
    """kind 'eq-dynamics' is in the plugin-kind vocab — accepted."""
    doc = copy.deepcopy(make_chain_doc_clean())
    doc["plugins"][0]["plugin_type"]["kind"] = "eq-dynamics"
    result = validate_chain_doc(doc)
    assert result == {"ok": True, "errors": []}


# ---------------------------------------------------------------------------
# Malformed documents must be rejected with named errors
# ---------------------------------------------------------------------------


def test_missing_processing_order_rejected():
    """processing_order removed → ok False, an error names it."""
    doc = copy.deepcopy(make_chain_doc_clean())
    del doc["plugins"][0]["processing_order"]
    result = validate_chain_doc(doc)
    assert result["ok"] is False
    assert any("processing_order" in err for err in result["errors"])


def test_bad_eq_overall_vocab_rejected():
    """eq.overall 'banana' → ok False, an error names the vocab."""
    doc = copy.deepcopy(make_chain_doc_clean())
    doc["plugins"][0]["eq"]["overall"] = "banana"
    result = validate_chain_doc(doc)
    assert result["ok"] is False
    assert any("eq.overall" in err and "banana" in err and "not in" in err
               for err in result["errors"])


def test_wrong_type_usable_as_spec_rejected():
    """usable_as_spec 'yes' (str, not bool) → ok False, error names it."""
    doc = copy.deepcopy(make_chain_doc_clean())
    doc["plugins"][0]["usable_as_spec"] = "yes"
    result = validate_chain_doc(doc)
    assert result["ok"] is False
    assert any("usable_as_spec" in err for err in result["errors"])


def test_empty_plugins_rejected():
    """plugins [] → ok False, an error names 'plugins'."""
    doc = copy.deepcopy(make_chain_doc_clean())
    doc["plugins"] = []
    result = validate_chain_doc(doc)
    assert result["ok"] is False
    assert any("plugins" in err for err in result["errors"])


# ---------------------------------------------------------------------------
# Never raises on malformed input
# ---------------------------------------------------------------------------


def test_malformed_input_never_raises():
    """None / non-dict / non-dict plugin input must return errors, not raise."""
    for bad in (None, [], "junk", {"plugins": [None]}):
        result = validate_chain_doc(bad)
        assert isinstance(result, dict)
        assert result["ok"] is False
        assert isinstance(result["errors"], list)
        assert result["errors"]


# ---------------------------------------------------------------------------
# Contract constants (single source of truth)
# ---------------------------------------------------------------------------


def test_shared_threshold_constants():
    """Thresholds re-exported from describe_quality keep the pinned values."""
    assert SANE_Q_MAX == 1000.0
    assert THD_CLEAN_MAX_PCT == 20.0
    assert CONTRACT_VERSION == "3"

# ---------------------------------------------------------------------------
# v3 contract: top-level contract_version + per-plugin stages[]
# ---------------------------------------------------------------------------


def test_v3_doc_with_stages_valid():
    """The v3 shape (contract_version "3" + stages[]) validates."""
    assert validate_chain_doc(make_chain_doc_v3_clean()) == {"ok": True,
                                                            "errors": []}


def test_v3_artifact_doc_valid():
    """Artifact docs are structurally valid in v3 too."""
    assert validate_chain_doc(make_chain_doc_v3_artifact()) == {"ok": True,
                                                               "errors": []}


def test_v3_missing_stages_rejected():
    """v3 plugin without stages[] -> error naming stages."""
    doc = upgrade_to_v3(make_chain_doc_clean())
    del doc["plugins"][0]["stages"]
    result = validate_chain_doc(doc)
    assert result["ok"] is False
    assert any("stages" in err for err in result["errors"])


def test_v3_empty_stages_rejected():
    """v3 plugin with stages [] -> error naming stages (v3 must decompose)."""
    doc = upgrade_to_v3(make_chain_doc_clean(), stages=[])
    result = validate_chain_doc(doc)
    assert result["ok"] is False
    assert any("stages" in err for err in result["errors"])


def test_v3_empty_stages_allowed_when_not_usable():
    """A plugin with no usable segment carries stages [] and stays valid —
    only a usable_as_spec=true claim forbids the empty list."""
    doc = upgrade_to_v3(make_chain_doc_artifact(), stages=[])
    assert validate_chain_doc(doc) == {"ok": True, "errors": []}


def test_v3_stage_kind_vocab_rejected():
    """Stage kind outside {eq,dynamics,nonlinearity} -> named vocab error."""
    doc = upgrade_to_v3(make_chain_doc_clean())
    doc["plugins"][0]["stages"][0]["kind"] = "reverb"
    result = validate_chain_doc(doc)
    assert result["ok"] is False
    assert any("stages[0].kind" in err and "reverb" in err and "not in" in err
               for err in result["errors"])


def test_v3_stage_missing_params_rejected():
    """Stage without params -> error naming stages[0].params."""
    doc = upgrade_to_v3(make_chain_doc_clean())
    del doc["plugins"][0]["stages"][0]["params"]
    result = validate_chain_doc(doc)
    assert result["ok"] is False
    assert any("stages[0].params" in err for err in result["errors"])


def test_v3_stage_missing_provenance_rejected():
    """Stage without provenance -> error naming stages[0].provenance."""
    doc = upgrade_to_v3(make_chain_doc_clean())
    del doc["plugins"][0]["stages"][0]["provenance"]
    result = validate_chain_doc(doc)
    assert result["ok"] is False
    assert any("stages[0].provenance" in err for err in result["errors"])


def test_v3_stage_provenance_basis_must_be_list():
    """provenance.basis as a str -> error naming that slot."""
    doc = upgrade_to_v3(make_chain_doc_clean())
    doc["plugins"][0]["stages"][0]["provenance"]["basis"] = "because"
    result = validate_chain_doc(doc)
    assert result["ok"] is False
    assert any("stages[0].provenance.basis" in err
               for err in result["errors"])


# ---------------------------------------------------------------------------
# Version handling: legacy v2 stays valid, unknown versions are rejected
# ---------------------------------------------------------------------------


def test_legacy_doc_without_contract_version_still_valid():
    """A pre-v3 document (no contract_version, no stages) still validates."""
    assert validate_chain_doc(make_chain_doc_clean()) == {"ok": True,
                                                          "errors": []}


def test_v2_versioned_doc_still_valid():
    """contract_version "2" keeps the v2 rules: stages are not required."""
    doc = make_chain_doc_clean()
    doc["contract_version"] = "2"
    assert validate_chain_doc(doc) == {"ok": True, "errors": []}


def test_unknown_contract_version_rejected():
    """An unsupported contract_version -> one deterministic error string."""
    doc = make_chain_doc_v3_clean()
    doc["contract_version"] = "4"
    result = validate_chain_doc(doc)
    assert result == {
        "ok": False,
        "errors": ["doc: contract_version '4' unsupported (known: 2, 3)"],
    }


# ---------------------------------------------------------------------------
# Optional top-level fusion block (stage_fusion.py output)
# ---------------------------------------------------------------------------


def test_fusion_block_valid():
    """A valid fusion block validates alongside the v3 doc."""
    doc = make_chain_doc_v3_clean()
    doc["fusion"] = make_fusion_block()
    assert validate_chain_doc(doc) == {"ok": True, "errors": []}


def test_fusion_wrong_type_rejected():
    """fusion as a str -> error naming fusion."""
    doc = make_chain_doc_v3_clean()
    doc["fusion"] = "junk"
    result = validate_chain_doc(doc)
    assert result["ok"] is False
    assert any("fusion" in err for err in result["errors"])


def test_fusion_pick_stage_vocab_rejected():
    """fusion.picks[].stage outside the stage vocab -> named vocab error."""
    doc = make_chain_doc_v3_clean()
    doc["fusion"] = make_fusion_block()
    doc["fusion"]["picks"][0]["stage"] = "reverb"
    result = validate_chain_doc(doc)
    assert result["ok"] is False
    assert any("fusion.picks[0].stage" in err and "reverb" in err
               for err in result["errors"])

