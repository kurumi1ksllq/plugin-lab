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
                                      make_chain_doc_clean)


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
    assert CONTRACT_VERSION == "1"
