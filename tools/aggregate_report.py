"""aggregate_report.py — Pure-function layer for batch reverse-derive reports.

Wave-1 T1-B (ticket #24): harmonic extraction/summary and
measurement-validity predicates consumed by the later-wave aggregation
engine. No CLI, no I/O, no aggregation logic here — pure functions only.

Stdlib only, no third-party dependencies.

Usage (import only; consumed by the aggregation engine):
    from aggregate_report import load_harmonic, harmonic_summary, ...
"""
import math

# ---------------------------------------------------------------------------
# Harmonic extraction
# ---------------------------------------------------------------------------


def load_harmonic(data):
    """Extract harmonic tone measurements from an export doc.

    Accepts both layouts used by PluginLab exports (SPEC.md):

      - dataset doc: the harmonic block is nested under ``data["harmonic"]``
      - standalone / body doc: tones live directly at the top level

    Missing or empty blocks yield []. Defensive fallback mirrors
    reverse_derive.py's ``_parse_*`` helpers (``data.get("harmonic") or
    data``).

    Returns a list of {"fundamental_hz", "fundamental_db", "thd_percent",
    "harmonics": [{"order", "freq", "mag_db", "percent"}, ...]}.
    """
    body = data.get("harmonic") or data
    return body.get("tones") or []


def harmonic_summary(tones):
    """Summarize each harmonic tone measurement into one compact dict.

    For every tone: {"fundamental_hz", "thd_percent", "dominant_order",
    "dominant_mag_db"}, where the dominant harmonic is the one with the
    maximum mag_db; both dominant fields are None when the tone has no
    harmonics. No tones in, no summaries out.
    """
    summary = []
    for tone in tones:
        harmonics = tone.get("harmonics") or []
        if harmonics:
            dominant = max(harmonics, key=lambda h: h.get("mag_db", -math.inf))
            dominant_order = dominant.get("order")
            dominant_mag_db = dominant.get("mag_db")
        else:
            dominant_order = None
            dominant_mag_db = None
        summary.append({"fundamental_hz": tone.get("fundamental_hz"),
                        "thd_percent": tone.get("thd_percent"),
                        "dominant_order": dominant_order,
                        "dominant_mag_db": dominant_mag_db})
    return summary


# ---------------------------------------------------------------------------
# Measurement-validity predicates
# ---------------------------------------------------------------------------


def is_eq_flat(mags):
    """True when the frequency-response magnitudes are degenerate-flat.

    All magnitudes are ≈ 0 dB: max(mags) - min(mags) < 0.1. Real historical
    captures are flat (all zeros). An empty capture counts as flat (nothing
    was measured).
    """
    if not mags:
        return True
    return max(mags) - min(mags) < 0.1


def is_compression_unity(curve):
    """True when the compression curve is degenerate-unity (no compression).

    All |gr_db| < 0.05, or the curve is empty.
    """
    if not curve:
        return True
    return all(abs(point.get("gr_db", 0.0)) < 0.05 for point in curve)


def is_harmonic_empty(tones):
    """True when no harmonic tone measurements were captured."""
    return not tones
