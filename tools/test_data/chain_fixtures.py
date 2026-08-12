"""
chain_fixtures.py — Deterministic fixture builders for the chain-description
pipeline (describe_chain / describe_quality, issue #26).

Each builder returns a plain dict/list literal (never a class instance) with
fixed, hand-verified values — the same rows, sections and parameter snapshots
the real pipeline consumed from `out/aggregate_report.json` (see
tools/aggregate_report.py) and the measurement exports (SPEC.md):

    freq section        -> {freq_hz, gain_db, q, status}
    harmonic section    -> {tones_count, status, summary:[{fundamental_hz,
                           thd_percent, dominant_order, dominant_mag_db}]}
    gr section          -> {attack_ms, release_ms, valid, status}
    compression section -> {threshold_db, ratio, status, json_fitted}
    aggregate row       -> {slug, plugin, has_*, freq, compression, gr,
                            harmonic, status}
    parameter snapshots -> analyzer / compressor / EQ dicts
    chain_doc           -> full describe_chain output document

Values marked "verified" are the real measured pro-c-3 / scepter / pro-q-4
rows captured from out/ (Q 18098.43, THD ~195.9 %, release 39676.69 ms,
compression -13.47/3.27 vs json -9.03/3.33, etc.) — fixtures reproduce them
exactly so describe_quality tests see the same artifact shapes the real
measurements produced. Fully deterministic: no randomness, no timestamps.

Stdlib only. Namespace package (no __init__.py): with tools/ on sys.path
(pytest provides it), `from test_data.chain_fixtures import ...` works.

Usage:
    python tools/test_data/chain_fixtures.py --check   # smoke: prints pro-c-3

Exit code 0 = ok.
"""
import argparse
import sys

# ======================= Verified pro-c-3 constants ==========================

# freq section: Q ~18098 far outside the plausible band -> artifact
_PRO_C3_FREQ = {"freq_hz": 18840.8, "gain_db": 12.78, "q": 18098.43,
                "status": "ok"}
# harmonic: 7 tones, THD percent in the 96-196 range, even dominant order
_PRO_C3_TONES = [
    {"fundamental_hz": 100.3, "thd_percent": 195.8767,
     "dominant_order": 4, "dominant_mag_db": 61.0},
    {"fundamental_hz": 199.6, "thd_percent": 151.62,
     "dominant_order": 2, "dominant_mag_db": 60.8},
    {"fundamental_hz": 300.2, "thd_percent": 96.58,
     "dominant_order": 2, "dominant_mag_db": 61.2},
    {"fundamental_hz": 399.9, "thd_percent": 122.51,
     "dominant_order": 4, "dominant_mag_db": 60.6},
    {"fundamental_hz": 799.8, "thd_percent": 112.33,
     "dominant_order": 2, "dominant_mag_db": 61.4},
    {"fundamental_hz": 3200.0, "thd_percent": 108.75,
     "dominant_order": 2, "dominant_mag_db": 61.1},
    {"fundamental_hz": 6399.9, "thd_percent": 106.5,
     "dominant_order": 4, "dominant_mag_db": 60.9},
]
_PRO_C3_COMPRESSION = {"threshold_db": -13.47, "ratio": 3.27, "status": "ok",
                       "json_fitted": {"threshold_db": -9.03, "ratio": 3.33,
                                       "knee_db": 3.0}}
_PRO_C3_GR = {"attack_ms": 3.859, "release_ms": 39676.69, "valid": True,
              "status": "ok"}

# ======================= freq section builders ===============================


def make_freq_clean():
    """Return a sane EQ section: gentle bell, plausible Q."""
    return {"freq_hz": 1000.0, "gain_db": 6.0, "q": 1.5, "status": "ok"}


def make_freq_artifact():
    """Return the verified pro-c-3 EQ section: Q 18098.43 -> artifact."""
    return dict(_PRO_C3_FREQ)


def make_freq_degenerate():
    """Return a degenerate EQ section (no usable fit at all)."""
    return {"freq_hz": None, "gain_db": None, "q": None, "status": "degenerate"}


# ======================= harmonic section builders ===========================


def make_harmonic_clean():
    """Return a clean 3-tone harmonic section (plausible THD 0.8-2.1%)."""
    return {"tones_count": 3, "status": "ok", "summary": [
        {"fundamental_hz": 1000.0, "thd_percent": 1.2, "dominant_order": 3,
         "dominant_mag_db": -40.0},
        {"fundamental_hz": 2000.0, "thd_percent": 0.8, "dominant_order": 2,
         "dominant_mag_db": -45.0},
        {"fundamental_hz": 3000.0, "thd_percent": 2.1, "dominant_order": 2,
         "dominant_mag_db": -38.0},
    ]}


def make_harmonic_artifact():
    """Return the verified pro-c-3 7-tone artifact (THD 96.6-195.9%)."""
    return {"tones_count": len(_PRO_C3_TONES), "status": "ok",
            "summary": [dict(t) for t in _PRO_C3_TONES]}


def make_harmonic_none():
    """Return a no-data harmonic section."""
    return {"tones_count": 0, "status": "no-data", "summary": []}


# ======================= gr section builders =================================


def make_gr_clean():
    """Return a sane GR section (fast attack, 50 ms release)."""
    return {"attack_ms": 3.859, "release_ms": 50.0, "valid": True,
            "status": "ok"}


def make_gr_implausible():
    """Return the verified pro-c-3 GR section: release 39676.69 ms."""
    return dict(_PRO_C3_GR)


def make_gr_invalid():
    """Return an invalid GR section (fit never succeeded)."""
    return {"attack_ms": 0.0, "release_ms": 0.0, "valid": False,
            "status": "degenerate"}


# ======================= tau by level builders ===============================


def make_tau_by_level_clean():
    """Return 10 sane {level_db, tau_sec} points (tau 0.0007-0.02 s)."""
    levels = [-60.0, -50.0, -40.0, -30.0, -20.0, -12.0, -6.0, 0.0, 6.0, 12.0]
    taus = [0.0007, 0.0008, 0.0010, 0.0015, 0.0022, 0.0035, 0.0056, 0.0090,
            0.0145, 0.0200]
    return [{"level_db": lvl, "tau_sec": tau} for lvl, tau in zip(levels, taus)]


def make_tau_by_level_outliers():
    """Return the verified pro-c-3 fit-failure pattern: two 0.0 taus plus two
    absurdly large (28.9, 289.7) — release fit diverged."""
    points = [{"level_db": -60.0, "tau_sec": 0.0},
              {"level_db": -50.0, "tau_sec": 0.0},
              {"level_db": -40.0, "tau_sec": 0.0010},
              {"level_db": -30.0, "tau_sec": 0.0015},
              {"level_db": -20.0, "tau_sec": 0.0022},
              {"level_db": -12.0, "tau_sec": 28.9},
              {"level_db": -6.0, "tau_sec": 289.7},
              {"level_db": 0.0, "tau_sec": 0.0090},
              {"level_db": 6.0, "tau_sec": 0.0145},
              {"level_db": 12.0, "tau_sec": 0.0200}]
    return [dict(p) for p in points]


# ======================= compression section builders ========================


def make_compression_clean():
    """Return a consistent compression section (derived == json fit)."""
    return {"threshold_db": -30.0, "ratio": 4.0, "status": "ok",
            "json_fitted": {"threshold_db": -30.0, "ratio": 4.0,
                            "knee_db": 0.0}}


def make_compression_conflict():
    """Return the verified pro-c-3 conflict: derived -13.47/3.27 vs json
    -9.03/3.33 knee 3.0."""
    return {"threshold_db": -13.47, "ratio": 3.27, "status": "ok",
            "json_fitted": {"threshold_db": -9.03, "ratio": 3.33,
                            "knee_db": 3.0}}


def make_compression_degenerate():
    """Return a degenerate compression section (no fit available)."""
    return {"threshold_db": None, "ratio": None, "status": "degenerate",
            "json_fitted": {}}


# ======================= aggregate report rows ===============================


def _degenerate_row(slug, plugin):
    """Assemble a row for a plugin whose fits all failed; harmonic carries the
    shared rig artifact (same tones for every rig-measured plugin)."""
    return {"slug": slug, "plugin": plugin, "has_freq": True,
            "has_compression": True, "has_gr": True, "has_harmonic": True,
            "freq": make_freq_degenerate(),
            "compression": make_compression_degenerate(),
            "gr": make_gr_invalid(), "harmonic": make_harmonic_artifact(),
            "status": "degenerate"}


def make_pro_c3_row():
    """Return the verified aggregate_report row for pro-c-3 (all artifacts)."""
    return {"slug": "pro-c-3", "plugin": "Pro-C 3", "has_freq": True,
            "has_compression": True, "has_gr": True, "has_harmonic": True,
            "freq": make_freq_artifact(),
            "compression": make_compression_conflict(),
            "gr": make_gr_implausible(), "harmonic": make_harmonic_artifact(),
            "status": "ok"}


def make_scepter_row():
    """Return the verified scepter row (degenerate fits, rig harmonic)."""
    return _degenerate_row("scepter", "Scepter")


def make_pro_q4_row():
    """Return the pro-q-4 row (same degenerate shape, plugin name differs)."""
    return _degenerate_row("pro-q-4", "Pro-Q 4")


def make_auto_key2_row():
    """Return the auto-key-2 row (degenerate shape)."""
    return _degenerate_row("auto-key-2", "Auto Key 2")


def make_uadx_row():
    """Return the uadx-vibe-analog-machines-essentials row (scepter copy)."""
    return _degenerate_row("uadx-vibe-analog-machines-essentials",
                           "UADx Vibe Analog Machines Essentials")


# ======================= parameter snapshots =================================


def make_analyzer_snapshot():
    """Return the scepter analyzer classifier snapshot."""
    return {"FFT Size": 0.6, "Hold Peaks": 0.0, "Smoothing": 0.5}


def make_compressor_snapshot():
    """Return the pro-c-3 compressor classifier snapshot."""
    return {"Threshold": 0.7333, "Ratio": 0.56, "Attack": 0.1423,
            "Release": 0.2779, "Auto Release": 0.0, "Wet Gain": 0.5,
            "Dry Gain": 0.5, "Style": 0.0}


def make_eq_unused_snapshot():
    """Return the pro-q-4 EQ snapshot with unused bands (Used == 0.0)."""
    return {"Band 1 Used": 0.0, "Band 1 Enabled": 1.0,
            "Band 1 Frequency": 0.5752, "Band 1 Q": 0.707,
            "Band 1 Gain": 0.5, "Band 1 Type": 0.0,
            "Band 2 Used": 0.0, "Band 2 Enabled": 1.0,
            "Band 2 Frequency": 0.6, "Band 2 Q": 0.707, "Band 2 Gain": 0.5,
            "Band 24 Used": 0.0}


# ======================= chain_doc builders ==================================


def _eq_section(freq_hz, gain_db, q, *, plausible, flag=None, reason=None):
    return {"freq_hz": freq_hz, "gain_db": gain_db, "q": q,
            "plausible": plausible, "flag": flag, "reason": reason}


def _compression_block(*, threshold_derived, ratio_derived, threshold_json,
                       ratio_json, knee_json, conflict, conflict_note=None):
    return {"threshold_derived": threshold_derived,
            "ratio_derived": ratio_derived, "threshold_json": threshold_json,
            "ratio_json": ratio_json, "knee_json": knee_json,
            "conflict": conflict, "conflict_note": conflict_note}


def _gr_block(*, attack_ms, release_ms, attack_plausible, release_plausible,
              release_flag=None, note=None):
    return {"attack_ms": attack_ms, "release_ms": release_ms,
            "attack_plausible": attack_plausible,
            "release_plausible": release_plausible,
            "release_flag": release_flag, "note": note}


def _plugin_block(*, slug, plugin, eq, dynamics, nonlinearity,
                  usable_as_spec, why_not_spec):
    return {
        "slug": slug,
        "plugin": plugin,
        "plugin_type": {"kind": "processor", "confidence": "low",
                        "basis": []},
        "eq": eq,
        "dynamics": dynamics,
        "nonlinearity": nonlinearity,
        "processing_order": {"order": "unknown", "confidence": "low",
                             "basis": [],
                             "suggested": "eq -> dyn -> eq",
                             "suggestion_note": "canonical, not measured"},
        "usable_as_spec": usable_as_spec,
        "why_not_spec": why_not_spec,
    }


def make_chain_doc_clean():
    """Return a minimal full chain_doc for one clean plugin (synthetic)."""
    return {
        "generated_at": "2026-08-13T00:00:00+00:00",
        "source": {"aggregate_report": "aggregate_report.json",
                   "dataset_dir": None,
                   "report_generated_at": "2026-08-13T00:00:00+00:00"},
        "plugins": [_plugin_block(
            slug="synthetic", plugin="Synthetic",
            eq={"present": True, "overall": "clean",
                "sections": [_eq_section(1000.0, 6.0, 1.5, plausible=True)],
                "notes": []},
            dynamics={"present": True,
                      "compression": _compression_block(
                          threshold_derived=-30.0, ratio_derived=4.0,
                          threshold_json=-30.0, ratio_json=4.0,
                          knee_json=0.0, conflict=False),
                      "gr": _gr_block(attack_ms=3.859, release_ms=50.0,
                                      attack_plausible=True,
                                      release_plausible=True),
                      "notes": []},
            nonlinearity={"verdict": "clean", "thd_range_pct": [0.8, 2.1],
                          "description": "THD 0.8-2.1% across 3 tones",
                          "reason": None},
            usable_as_spec=True, why_not_spec=[]),
        ],
    }


def make_chain_doc_artifact():
    """Return a chain_doc for a pro-c-3-style plugin: eq/compression/gr/
    nonlinearity all flagged, not usable as spec."""
    return {
        "generated_at": "2026-08-13T00:00:00+00:00",
        "source": {"aggregate_report": "aggregate_report.json",
                   "dataset_dir": None,
                   "report_generated_at": "2026-08-13T00:00:00+00:00"},
        "plugins": [_plugin_block(
            slug="pro-c-3", plugin="Pro-C 3",
            eq={"present": True, "overall": "artifact",
                "sections": [_eq_section(
                    18840.8, 12.78, 18098.43, plausible=False,
                    flag="implausible_q",
                    reason="Q=18098.4 far outside the plausible 0.1..200 band")],
                "notes": []},
            dynamics={"present": True,
                      "compression": _compression_block(
                          threshold_derived=-13.47, ratio_derived=3.27,
                          threshold_json=-9.03, ratio_json=3.33,
                          knee_json=3.0, conflict=True,
                          conflict_note="derived vs json threshold differ "
                                        "by 4.4 dB"),
                      "gr": _gr_block(attack_ms=3.859,
                                      release_ms=39676.69,
                                      attack_plausible=True,
                                      release_plausible=False,
                                      release_flag="implausible_release",
                                      note="release 39676.7 ms far outside "
                                           "the plausible range"),
                      "notes": []},
            nonlinearity={"verdict": "artifact",
                          "thd_range_pct": [96.58, 195.8767],
                          "description": "THD 96.6-195.9% across 7 tones",
                          "reason": None},
            usable_as_spec=False,
            why_not_spec=["eq artifact", "compression fit conflict",
                          "release implausible", "harmonic artifact"]),
        ],
    }


# ======================= CLI =================================================


def main():
    parser = argparse.ArgumentParser(
        description="chain_fixtures smoke check (issue #26)")
    parser.add_argument("--check", action="store_true",
                        help="print the slug of make_pro_c3_row() and exit 0")
    args = parser.parse_args()
    if args.check:
        print(make_pro_c3_row()["slug"])
    sys.exit(0)


if __name__ == "__main__":
    main()
