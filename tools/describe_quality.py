"""describe_quality.py — Measurement-quality predicates for describe_chain.

Wave-1 Task T1 of issue #26. Pure, I/O-free predicates that judge whether
the measurements captured from a plugin (SPEC.md export shapes) are
plausible enough to describe a processing chain from:

  - classify_freq_peak:            EQ-style peak {freq_hz, gain_db, q}
        sanity vs. sane Q / frequency / gain bounds
  - classify_harmonic:             THD verdict (clean / artifact /
        not-measured) with a thd_range_pct and reasons
  - tau_sanity:                    attack/release time-constant bounds and
        attack↔release extreme-mismatch flag
  - summarize_by_level:            robust median of per-level tau_sec
        arrays (ignores 0.0 and >10x-median outliers)
  - detect_duplicate_fingerprints: sha1 grouping of identical raw
        harmonic blocks across plugins (same processing chain suspected)

Stdlib only (hashlib/json/math) — no numpy/scipy. Pure functions, no I/O;
callers (describe_render.py, describe_schema.py) import this module.
Constants are defined here and re-exported/imported by the schema task.

Usage:
    import describe_quality as dq
    verdict = dq.classify_harmonic(harmonic_row)
"""

import hashlib
import json

# ---------------------------------------------------------------------------
# Sanity bounds (shared with describe_schema.py / describe_render.py)
# ---------------------------------------------------------------------------

SANE_Q_MIN = 0.01
SANE_Q_MAX = 1000.0
TAU_MIN_MS = 0.01
TAU_MAX_MS = 60000.0
THD_CLEAN_MAX_PCT = 20.0

# Extreme-mismatch floor: a release faster than 10x the attack is normal
# for compressors (release is typically 4-20x attack), so the mismatch flag
# only fires when the release is both >10x attack AND > this floor — i.e.
# seconds-scale releases (noise guard per issue #26).
_MISMATCH_FLOOR_MS = 1000.0

_FREQ_MIN_HZ = 20.0
_GAIN_MIN_DB = -30.0
_GAIN_MAX_DB = 30.0


# ---------------------------------------------------------------------------
# classify_freq_peak
# ---------------------------------------------------------------------------


def classify_freq_peak(freq_row, nyquist=None):
    """Judge the plausibility of one EQ-style peak row.

    freq_row is a dict with keys freq_hz / gain_db / q (any may be None).
    Implausible when:
      - q is not None and outside [SANE_Q_MIN, SANE_Q_MAX]
      - freq_hz is not None and nyquist is not None and outside
        [20.0, 0.95 * nyquist] (freq bound skipped when nyquist is None)
      - gain_db is not None and outside [-30.0, +30.0]

    None values mean "nothing to judge" for that bound; an all-None row is
    plausible with flag/reason None.

    Returns {"plausible": bool, "flag": str|None, "reason": str|None}.
    """
    q = freq_row.get("q")
    freq_hz = freq_row.get("freq_hz")
    gain_db = freq_row.get("gain_db")

    if q is not None and not (SANE_Q_MIN <= q <= SANE_Q_MAX):
        return {"plausible": False, "flag": "q_out_of_range",
                "reason": f"q {q:g} outside [{SANE_Q_MIN:g}, {SANE_Q_MAX:g}]"}

    if (freq_hz is not None and nyquist is not None
            and not (_FREQ_MIN_HZ <= freq_hz <= 0.95 * nyquist)):
        return {"plausible": False, "flag": "freq_out_of_range",
                "reason": f"freq_hz {freq_hz:g} outside "
                          f"[{_FREQ_MIN_HZ:g}, {0.95 * nyquist:g}]"}

    if gain_db is not None and not (_GAIN_MIN_DB <= gain_db <= _GAIN_MAX_DB):
        return {"plausible": False, "flag": "gain_out_of_range",
                "reason": f"gain_db {gain_db:g} outside "
                          f"[{_GAIN_MIN_DB:g}, {_GAIN_MAX_DB:g}]"}

    return {"plausible": True, "flag": None, "reason": None}


# ---------------------------------------------------------------------------
# classify_harmonic
# ---------------------------------------------------------------------------


def classify_harmonic(harmonic_row):
    """Classify a harmonic-measurement row as clean / artifact / not-measured.

    harmonic_row has {tones_count, summary: [{fundamental_hz, thd_percent,
    dominant_order, dominant_mag_db}], status}.

    - not-measured when tones_count == 0 or status == "no-data" (or the
      summary is empty — defensive).
    - artifact when ANY tone thd_percent >= 100.0 (impossible physics) or
      >= THD_CLEAN_MAX_PCT. Even-order dominance of an artifact tone is
      appended as a belt-and-braces reason.
    - otherwise clean.

    Returns {"verdict": str, "thd_range_pct": [min, max]|None,
    "reasons": [str]}.
    """
    tones_count = harmonic_row.get("tones_count", 0)
    status = harmonic_row.get("status")
    summary = harmonic_row.get("summary") or []

    if tones_count == 0 or status == "no-data" or not summary:
        return {"verdict": "not-measured", "thd_range_pct": None, "reasons": []}

    thds = [tone["thd_percent"] for tone in summary]
    reasons = []
    artifact = False
    for tone in summary:
        thd = tone["thd_percent"]
        if thd >= 100.0:
            artifact = True
            reasons.append("thd >= 100.0 (impossible physics)")
        elif thd >= THD_CLEAN_MAX_PCT:
            artifact = True
            reasons.append(f"thd >= THD_CLEAN_MAX_PCT ({THD_CLEAN_MAX_PCT:g})")

    if artifact:
        for tone in summary:
            order = tone.get("dominant_order")
            if (tone["thd_percent"] >= THD_CLEAN_MAX_PCT
                    and order is not None and int(order) % 2 == 0):
                reasons.append("even-order dominant")

    return {"verdict": "artifact" if artifact else "clean",
            "thd_range_pct": [min(thds), max(thds)], "reasons": reasons}


# ---------------------------------------------------------------------------
# tau_sanity
# ---------------------------------------------------------------------------


def tau_sanity(gr_row):
    """Judge attack/release time-constant plausibility of a GR row.

    gr_row has {attack_ms, release_ms, valid, status} — any key may be
    missing (defensive: never raises). A bound is plausible when its value
    is in [TAU_MIN_MS, TAU_MAX_MS]. release is additionally flagged
    "release_implausible" when release_ms > TAU_MAX_MS or when it exceeds
    10x a positive attack_ms AND clears the extreme-mismatch floor
    (_MISMATCH_FLOOR_MS — a 10x ratio alone is normal for compressors).
    valid=False invalidates the whole row.

    Returns {"attack_plausible": bool, "release_plausible": bool,
    "flag": str|None, "note": str|None}.
    """
    attack_ms = gr_row.get("attack_ms")
    release_ms = gr_row.get("release_ms")
    valid = gr_row.get("valid", True)

    if not valid:
        return {"attack_plausible": False, "release_plausible": False,
                "flag": None, "note": "measurement marked invalid"}

    attack_ok = True
    attack_note = None
    if attack_ms is None:
        attack_ok = False
        attack_note = "attack_ms missing"
    elif not (TAU_MIN_MS <= attack_ms <= TAU_MAX_MS):
        attack_ok = False
        attack_note = (f"attack_ms {attack_ms:g} outside "
                       f"[{TAU_MIN_MS:g}, {TAU_MAX_MS:g}]")

    release_ok = True
    release_note = None
    flag = None
    if release_ms is None:
        release_ok = False
        release_note = "release_ms missing"
    elif release_ms > TAU_MAX_MS:
        release_ok = False
        release_note = f"release_ms {release_ms:g} exceeds TAU_MAX_MS"
        flag = "release_implausible"
    elif (attack_ms is not None and attack_ms > 0.0
          and release_ms > 10.0 * attack_ms
          and release_ms > _MISMATCH_FLOOR_MS):
        release_ok = False
        release_note = (f"release_ms {release_ms:g} > 10x attack_ms "
                        f"(extreme mismatch)")
        flag = "release_implausible"
    elif release_ms < TAU_MIN_MS:
        release_ok = False
        release_note = f"release_ms {release_ms:g} below TAU_MIN_MS"

    note = release_note if release_note is not None else attack_note
    return {"attack_plausible": attack_ok, "release_plausible": release_ok,
            "flag": flag, "note": note}


# ---------------------------------------------------------------------------
# summarize_by_level
# ---------------------------------------------------------------------------


def _median(values):
    """Median of a non-empty list of floats (None when empty)."""
    if not values:
        return None
    ordered = sorted(values)
    mid = len(ordered) // 2
    if len(ordered) % 2 == 1:
        return ordered[mid]
    return (ordered[mid - 1] + ordered[mid]) / 2.0


def summarize_by_level(by_level, kind):
    """Robust median (ms) of a per-level tau_sec array.

    by_level is a list of dicts carrying "tau_sec" (seconds); `kind` names
    the curve ("attack" or "release") for callers — the array items always
    carry tau_sec. tau_sec values of 0.0 and values > 10x the median of the
    remaining values are ignored (iterative: the median is recomputed after
    each filtering round, so runaway outliers pull no weight).

    Returns {"median_ms": float|None, "n_outliers": int,
    "ignored_values": [float]} — ignored_values in seconds, input order.
    """
    entries = []  # (input index, ms, sec)
    for idx, row in enumerate(by_level):
        sec = row.get("tau_sec")
        if sec is None:
            continue
        ms = float(sec) * 1000.0
        entries.append((idx, ms, float(sec)))

    ignored = [(idx, sec) for (idx, ms, sec) in entries if ms == 0.0]
    keep = [(idx, ms, sec) for (idx, ms, sec) in entries if ms != 0.0]

    while True:
        median_ms = _median([ms for _, ms, _ in keep])
        if median_ms is None:
            break
        cutoff_ms = 10.0 * median_ms
        filtered = [(idx, ms, sec) for (idx, ms, sec) in keep if ms <= cutoff_ms]
        if len(filtered) == len(keep):
            break
        ignored.extend((idx, sec) for (idx, ms, sec) in keep
                       if (idx, ms, sec) not in filtered)
        keep = filtered

    ignored.sort(key=lambda item: item[0])
    return {"median_ms": _median([ms for _, ms, _ in keep]),
            "n_outliers": len(ignored),
            "ignored_values": [sec for _, sec in ignored]}


# ---------------------------------------------------------------------------
# detect_duplicate_fingerprints
# ---------------------------------------------------------------------------


def detect_duplicate_fingerprints(rows):
    """Group slugs by the sha1 fingerprint of their raw harmonic block.

    rows: list of dicts with {slug, harmonic_raw: list|None} (harmonic_raw
    is the raw tones list from dataset.json; None when absent — skipped).
    The fingerprint is sha1 of the canonical json.dumps (sort_keys=True,
    default=str) of harmonic_raw, so identical raw blocks — the signature
    of a shared processing chain — get the same key regardless of field
    order. Only fingerprints shared by >= 2 slugs are returned.

    Returns {fingerprint_hex: [slugs...]}.
    """
    groups = {}
    for row in rows:
        raw = row.get("harmonic_raw")
        slug = row.get("slug")
        if raw is None or slug is None:
            continue
        canonical = json.dumps(raw, sort_keys=True, default=str).encode()
        fingerprint = hashlib.sha1(canonical).hexdigest()
        groups.setdefault(fingerprint, []).append(slug)

    return {fp: slugs for fp, slugs in groups.items() if len(slugs) >= 2}
