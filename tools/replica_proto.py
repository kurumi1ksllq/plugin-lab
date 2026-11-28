"""replica_proto.py — render the five measured exports from one chain_doc.

Wave-3 task T8 of issue #106. A chain_doc (describe_chain.py / stage_fusion.py,
contract v3) decomposes ONE measured plugin into stages[]:

    {kind: "eq",           params: {overall, sections:[{freq_hz,gain_db,q}]}}
    {kind: "dynamics",     params: {threshold_db, ratio, knee_db,
                                    attack_ms, release_ms, conflict}}
    {kind: "nonlinearity", params: {verdict, thd_range_pct:[min,max]|None}}

This module reads those fitted numbers as a DSP spec, runs a small numeric
prototype of the chain, and renders the five SPEC.md export documents the
measurement side produces — so a later gap report can diff "what the spec
says" against "what the plugin did":

    frequency_response / compression / gr_timeline /
    nonlinear_family / stereo_width

DSP model (stdlib only, closed form — no fitting, no iteration):

  * EQ — one RBJ peaking biquad per section (fs 48 kHz). |H| in dB and the
    phase in degrees are evaluated analytically on a 100..10000 Hz logarithmic
    grid (200 points) and summed over sections. Smoothing is the identity here
    (smoothed_1_12 == smoothed_1_3 == raw): the prototype has no noise to
    smooth. No eq stage -> a flat 0 dB line with phase 0.

  * Compressor — static curve, input -60..0 dBFS in 2 dB steps (31 points):
    identity at or below the threshold, out = thr + (in - thr)/ratio above it,
    gr_db = out - in. fitted = {ratio, threshold_db, knee_db} straight from
    the stage (knee absent -> 0.0). No dynamics stage -> identity
    (ratio 1.0, threshold 0.0, knee 0.0). The knee is reported, never applied:
    the prototype curve is hard-knee (SPEC 3 treats the knee as a fit output).

  * GR timeline — single-pole gain computer, 0..0.5 s in 5 ms steps (101
    points). A fixed -6 dBFS excitation crosses the threshold at t = 0 and
    targets the steady-state reduction (level - thr)(1/ratio - 1); the attack
    branch approaches it with tau = attack_ms/1000, and from t = 0.1 s the
    release branch decays back to 0 with tau = release_ms/1000. tau reports
    those two constants. level_db in attack_by_level / release_by_level is
    that same fixed -6.0 dBFS excitation: the prototype simulates one
    stimulus, so a per-level family would be invented data. No dynamics stage
    -> flat 0 GR, valid:false and all-zero taus, mirroring the SPEC 6
    "no edge -> false + all zeros" convention.

  * Nonlinearity — cubic waveshaper y = x + c*x^3 driven by a 400 Hz tone per
    level, x normalised to [-1, 1]. Chebyshev closed form: with x = cos(w),
    x^3 = (T3 + 3*T1)/4, so a unit-amplitude tone yields A1 = 1 + 3*u/4 and
    A3 = u/4, where u = c_eff = c*a^2 and a = 10^(level_db/20) * 0.4 is the
    excitation amplitude (SPEC 13 uses the same 10^(db/20) convention; 0.4 is
    the rig's literal harmonic amplitude). Hence
    thd_percent = 100*|A3|/|A1| and fundamental_db = level_db + 20*log10(A1).
    c is solved so the -6 dB reference level reproduces the measured upper
    bound t = thd_range_pct[1]/100:

        t = u/(4 + 3u)  =>  u = 4t/(1 - 3t)  =>  c = u / a_ref^2

    (the task prompt's 4t/(1-0.75t) does not invert this relation; the
    inversion above is what the closed form requires — see the task report).
    H2 is emitted at the -120 dB floor / 0 % rather than dropped: an odd
    cubic has no even-order term. thd_range_pct null, a verdict outside
    {clean, artifact}, or an upper bound at or above 33.3 % (u -> inf) all
    render c = 0 — an identity shaper with thd 0, never an invented curve.
    levels_db is the SPEC 13 default grid [-60,-48,-36,-24,-12,-6,0].

  * Stereo width — chain_doc v3 has no stereo stage kind (STAGE_KINDS is
    eq/dynamics/nonlinearity), so the model copies the mono chain to L = R:
    mid_response equals frequency_response raw, side_response sits on the
    -120 dB floor with phase 0, correlation is 1.0, and
    width_db(f) = mid_mag(f) + 120 dB (the ADR-0003 width definition against
    the floor). A deterministic convention, not a measurement.

Context blocks follow the SPEC general rules: frequency_response and
compression carry the context fields flat at top level; gr_timeline,
nonlinear_family and stereo_width nest them under "context". Values are the
fixed prototype identity (class_id "replica-prototype", latency 0, 48 kHz,
block 512, empty parameter snapshot, source type "signal"). Every number is
rounded per the SPEC precision table (frequency 1, magnitude/phase/curve 2,
tau 6, time 4, THD 4), so rendering is byte-stable: the same chain_doc always
renders byte-identical JSON. Nothing reads the clock, the filesystem or an
RNG.

Contract: render_replica(chain_doc) validates its input with
describe_schema.validate_chain_doc and raises ValueError when the document is
not a valid chain_doc, or when it does not describe exactly one plugin
(stage_fusion's single-plugin convention).

Usage (CLI):
    python tools/replica_proto.py chain_doc.json --out-dir DIR

Exit codes: 0 = the five documents were written; 2 = input or output error
(missing file, unparseable JSON, contract violation, unwritable output) — one
"error: ..." line on stderr, nothing written.
"""
import argparse
import json
import math
import sys
from pathlib import Path

from describe_schema import validate_chain_doc

# ---------------------------------------------------------------------------
# Model constants
# ---------------------------------------------------------------------------

SAMPLE_RATE_HZ = 48000.0
BLOCK_SIZE = 512
CLASS_ID = "replica-prototype"

# Magnitude floor shared by the side channel and the harmonics the model does
# not produce (a zero magnitude is -inf dB, which JSON cannot carry).
MAG_FLOOR_DB = -120.0

# Frequency grid: 100 Hz .. 10 kHz, logarithmic, 200 points (SPEC 1).
FREQ_MIN_HZ = 100.0
FREQ_MAX_HZ = 10000.0
FREQ_POINTS = 200

# Static compression sweep (SPEC 3): -60 .. 0 dBFS in 2 dB steps.
COMPRESSION_MIN_DB = -60.0
COMPRESSION_STEP_DB = 2.0
COMPRESSION_POINTS = 31

# GR timeline (SPEC 6): 0 .. 0.5 s in 5 ms steps; release starts at 0.1 s.
GR_POINTS = 101
GR_STEP_SEC = 0.005
GR_RELEASE_START_SEC = 0.1

# Fixed stimulus shared by the GR simulation and the tau by-level rows: one
# -6 dBFS excitation, the reference level of the harmonic measurement.
GR_EXCITATION_DB = -6.0

# Nonlinear family (SPEC 13): one 400 Hz tone per level, default level grid.
TONE_FREQ_HZ = 400.0
LEVELS_DB = (-60.0, -48.0, -36.0, -24.0, -12.0, -6.0, 0.0)
REFERENCE_LEVEL_DB = -6.0
EXCITATION_SCALE = 0.4

# The five documents this renderer produces, in output order.
OUTPUT_NAMES = ("frequency_response", "compression", "gr_timeline",
                "nonlinear_family", "stereo_width")


class InputError(Exception):
    """Usage or input problem: reported on stderr, exit code 2."""


# ---------------------------------------------------------------------------
# Stage lookup and context
# ---------------------------------------------------------------------------


def _stage_params(plugin, kind):
    """params of the first stage of this kind, or None when there is none."""
    for stage in plugin.get("stages") or []:
        if stage.get("kind") == kind:
            params = stage.get("params")
            return params if isinstance(params, dict) else {}
    return None


def _context(plugin_name, source_type="signal"):
    """The SPEC general-convention context block for a prototype render."""
    return {"plugin": plugin_name,
            "class_id": CLASS_ID,
            "latency_samples": 0,
            "sample_rate": SAMPLE_RATE_HZ,
            "measurement": {"sample_rate": SAMPLE_RATE_HZ,
                            "block_size": BLOCK_SIZE},
            "parameter_snapshot": {},
            "source": {"type": source_type}}


# ---------------------------------------------------------------------------
# EQ -> frequency_response
# ---------------------------------------------------------------------------


def _freq_grid():
    """Logarithmic 100 Hz .. 10 kHz grid, 200 points, rounded to 1 decimal."""
    span = math.log(FREQ_MAX_HZ / FREQ_MIN_HZ)
    return [round(FREQ_MIN_HZ * math.exp(span * i / (FREQ_POINTS - 1)), 1)
            for i in range(FREQ_POINTS)]


def _peaking_response(freq_hz, centre_hz, gain_db, q):
    """(magnitude dB, phase deg) of one RBJ peaking biquad at freq_hz.

    A section with a null gain, a non-positive centre or a non-positive Q is
    transparent (0 dB / 0 deg) instead of dividing by zero.
    """
    if not centre_hz or not q or not gain_db or centre_hz <= 0.0 or q <= 0.0:
        return 0.0, 0.0
    w0 = 2.0 * math.pi * centre_hz / SAMPLE_RATE_HZ
    alpha = math.sin(w0) / (2.0 * q)
    amp = 10.0 ** (gain_db / 40.0)
    b0, b1, b2 = 1.0 + alpha * amp, -2.0 * math.cos(w0), 1.0 - alpha * amp
    a0, a1, a2 = 1.0 + alpha / amp, -2.0 * math.cos(w0), 1.0 - alpha / amp
    w = 2.0 * math.pi * freq_hz / SAMPLE_RATE_HZ
    # H(e^jw) with z^-1 = e^-jw; the transfer function is
    # (b0 + b1 z^-1 + b2 z^-2) / (a0 + a1 z^-1 + a2 z^-2).
    num_re = b0 + b1 * math.cos(w) + b2 * math.cos(2.0 * w)
    num_im = -(b1 * math.sin(w) + b2 * math.sin(2.0 * w))
    den_re = a0 + a1 * math.cos(w) + a2 * math.cos(2.0 * w)
    den_im = -(a1 * math.sin(w) + a2 * math.sin(2.0 * w))
    den = math.hypot(den_re, den_im)
    if den == 0.0:
        return 0.0, 0.0
    mag_db = 20.0 * math.log10(math.hypot(num_re, num_im) / den)
    phase_deg = math.degrees(math.atan2(num_im, num_re)
                             - math.atan2(den_im, den_re))
    return mag_db, phase_deg


def _frequency_points(plugin):
    """Raw frequency_response points: summed EQ bands on the fixed grid."""
    params = _stage_params(plugin, "eq")
    sections = (params or {}).get("sections") or []
    points = []
    for freq in _freq_grid():
        mag = 0.0
        phase = 0.0
        for section in sections:
            section_mag, section_phase = _peaking_response(
                freq, section.get("freq_hz"), section.get("gain_db"),
                section.get("q"))
            mag += section_mag
            phase += section_phase
        points.append({"f": freq, "mag": round(mag, 2),
                       "phase": round(phase, 2)})
    return points


def _frequency_doc(plugin_name, points):
    """frequency_response: raw plus the identity-smoothed copies (SPEC 1)."""
    return {"type": "frequency_response",
            **_context(plugin_name),
            "raw": points,
            "smoothed_1_12": [dict(point) for point in points],
            "smoothed_1_3": [dict(point) for point in points]}


# ---------------------------------------------------------------------------
# Dynamics -> compression_curve
# ---------------------------------------------------------------------------


def _static_curve(threshold_db, ratio):
    """31 static points: unity at/below the threshold, slope 1/ratio above."""
    curve = []
    for index in range(COMPRESSION_POINTS):
        level = COMPRESSION_MIN_DB + index * COMPRESSION_STEP_DB
        out_db = (level if level <= threshold_db
                  else threshold_db + (level - threshold_db) / ratio)
        curve.append({"input_db": round(level, 2),
                      "output_db": round(out_db, 2),
                      "gr_db": round(out_db - level, 2)})
    return curve


def _compression_doc(plugin_name, params):
    """compression_curve: the stage's threshold/ratio/knee as a static curve."""
    threshold_db = (params or {}).get("threshold_db")
    ratio = (params or {}).get("ratio")
    knee_db = (params or {}).get("knee_db")
    # A missing or unusable fit renders the identity, never a guessed curve.
    if threshold_db is None or ratio is None or ratio <= 0.0:
        threshold_db, ratio, knee_db = 0.0, 1.0, 0.0
    return {"type": "compression_curve",
            **_context(plugin_name),
            "curve": _static_curve(threshold_db, ratio),
            "fitted": {"ratio": round(ratio, 2),
                       "threshold_db": round(threshold_db, 2),
                       "knee_db": round(0.0 if knee_db is None
                                        else knee_db, 2)}}


# ---------------------------------------------------------------------------
# Dynamics -> gr_timeline
# ---------------------------------------------------------------------------


def _target_gr_db(params):
    """Steady-state reduction for the fixed excitation (0 below threshold)."""
    threshold_db = params.get("threshold_db")
    ratio = params.get("ratio")
    if threshold_db is None or ratio is None or ratio <= 0.0:
        return 0.0
    if GR_EXCITATION_DB <= threshold_db:
        return 0.0
    return (GR_EXCITATION_DB - threshold_db) * (1.0 / ratio - 1.0)


def _attack_value(target_db, attack_sec, time_sec):
    """Single-pole approach to the target (instant when tau is 0)."""
    if attack_sec <= 0.0:
        return target_db
    return target_db * (1.0 - math.exp(-time_sec / attack_sec))


def _gr_curve(target_db, attack_sec, release_sec):
    """Attack toward the target up to 0.1 s, then release back to 0."""
    timeline = []
    for index in range(GR_POINTS):
        time_sec = index * GR_STEP_SEC
        if time_sec <= GR_RELEASE_START_SEC:
            gr_db = _attack_value(target_db, attack_sec, time_sec)
        elif release_sec <= 0.0:
            gr_db = 0.0
        else:
            held = _attack_value(target_db, attack_sec, GR_RELEASE_START_SEC)
            gr_db = held * math.exp(-(time_sec - GR_RELEASE_START_SEC)
                                    / release_sec)
        timeline.append({"t": round(time_sec, 4), "gr_db": round(gr_db, 2)})
    return timeline


def _gr_doc(plugin_name, params):
    """gr_timeline: simulated GR curve plus the fitted time constants."""
    attack_ms = (params or {}).get("attack_ms")
    release_ms = (params or {}).get("release_ms")
    valid = attack_ms is not None and release_ms is not None
    attack_sec = attack_ms / 1000.0 if valid else 0.0
    release_sec = release_ms / 1000.0 if valid else 0.0
    if valid:
        timeline = _gr_curve(_target_gr_db(params), attack_sec, release_sec)
    else:
        # No dynamics stage: no edge was detected -> flat 0 GR and false +
        # all-zero taus, the SPEC 6 convention for an edge-free source.
        timeline = [{"t": round(index * GR_STEP_SEC, 4), "gr_db": 0.0}
                    for index in range(GR_POINTS)]
    # SPEC.md §6: gr_timeline is driven by a level step -> source "dynamic".
    return {"type": "gr_timeline",
            "context": _context(plugin_name, "dynamic"),
            "gr": {"sample_rate": SAMPLE_RATE_HZ,
                   "num_points": len(timeline),
                   "timeline": timeline},
            "tau": {"attack_sec": round(attack_sec, 6),
                    "release_sec": round(release_sec, 6),
                    "valid": valid,
                    "attack_by_level": [{"level_db": GR_EXCITATION_DB,
                                         "tau_sec": round(attack_sec, 6)}],
                    "release_by_level": [{"level_db": GR_EXCITATION_DB,
                                          "tau_sec": round(release_sec, 6)}]}}


# ---------------------------------------------------------------------------
# Nonlinearity -> nonlinear_family
# ---------------------------------------------------------------------------


def _cubic_coefficient(params):
    """c in y = x + c*x^3 for the stage's measured THD, or 0.0 (identity).

    With x = cos(w) the Chebyshev expansion gives x^3 = (T3 + 3*T1)/4, so for
    u = c*a^2 the fundamental is A1 = 1 + 3u/4 and the third harmonic is
    A3 = u/4. THD as a fraction is therefore t = u/(4 + 3u), whose inverse is
    u = 4t/(1 - 3t); c = u/a_ref^2 places it at the -6 dB reference level.
    """
    if not params or params.get("verdict") not in ("clean", "artifact"):
        return 0.0
    thd_range = params.get("thd_range_pct")
    if not isinstance(thd_range, (list, tuple)) or len(thd_range) != 2:
        return 0.0
    upper_pct = thd_range[1]
    if upper_pct is None or upper_pct <= 0.0:
        return 0.0
    t = upper_pct / 100.0
    # t >= 1/3 lies beyond this cubic family (u -> inf): render identity.
    if 3.0 * t >= 1.0:
        return 0.0
    u = 4.0 * t / (1.0 - 3.0 * t)
    amplitude = 10.0 ** (REFERENCE_LEVEL_DB / 20.0) * EXCITATION_SCALE
    return u / (amplitude * amplitude)


def _tone(level_db, coefficient):
    """One 400 Hz tone at one level: fundamental plus the H2/H3 harmonics."""
    amplitude = 10.0 ** (level_db / 20.0) * EXCITATION_SCALE
    effective = coefficient * amplitude * amplitude
    a1 = 1.0 + 3.0 * effective / 4.0
    a3 = effective / 4.0
    fundamental_db = level_db + (20.0 * math.log10(a1) if a1 > 0.0 else 0.0)
    if a3 != 0.0 and a1 != 0.0:
        ratio = abs(a3) / abs(a1)
        h3_db = max(fundamental_db + 20.0 * math.log10(ratio), MAG_FLOOR_DB)
        thd_percent = 100.0 * ratio
    else:
        h3_db = MAG_FLOOR_DB
        thd_percent = 0.0
    return {"fundamental_hz": round(TONE_FREQ_HZ, 1),
            "fundamental_db": round(fundamental_db, 2),
            "thd_percent": round(thd_percent, 4),
            "harmonics": [
                # The cubic is odd: no even-order term exists. The field is
                # reported at the floor rather than dropped (SPEC 2 shape).
                {"order": 2, "freq": round(TONE_FREQ_HZ * 2.0, 1),
                 "mag_db": MAG_FLOOR_DB, "percent": 0.0},
                {"order": 3, "freq": round(TONE_FREQ_HZ * 3.0, 1),
                 "mag_db": round(h3_db, 2),
                 "percent": round(thd_percent, 4)},
            ]}


def _nonlinear_doc(plugin_name, params):
    """nonlinear_family: THD across the SPEC 13 default level grid."""
    coefficient = _cubic_coefficient(params)
    return {"type": "nonlinear_family",
            "context": _context(plugin_name),
            "family": [{"input_level_db": level,
                        "valid": True,
                        "tones": [_tone(level, coefficient)]}
                       for level in LEVELS_DB]}


# ---------------------------------------------------------------------------
# Stereo width
# ---------------------------------------------------------------------------


def _stereo_doc(plugin_name, points):
    """stereo_width of the mono replica: L = R, so there is no side energy.

    chain_doc v3 carries no stereo stage kind, so width is a deterministic
    convention rather than a measurement: the mid response is the
    frequency_response raw curve, the side channel sits on the -120 dB floor,
    correlation is 1.0 and width_db(f) = mid_mag(f) + 120 dB (ADR-0003).
    shortcut: no stereo stage kind in chain_doc v3, render the mono replica
    until one exists (ADR-0003 / issue #104).
    """
    return {"type": "stereo_width",
            "context": _context(plugin_name),
            "mid_response": points,
            "side_response": [{"f": point["f"], "mag": MAG_FLOOR_DB,
                               "phase": 0.0} for point in points],
            "correlation": [{"f": point["f"], "rho": 1.0} for point in points],
            "width_db": [{"f": point["f"],
                          "width_db": round(point["mag"] - MAG_FLOOR_DB, 2)}
                         for point in points]}


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------


def render_replica(chain_doc):
    """Render the five export documents from one v3 chain_doc.

    Returns {name: doc} for frequency_response, compression, gr_timeline,
    nonlinear_family and stereo_width. Pure and deterministic: the input is
    never mutated and nothing reads the clock or an RNG, so the same document
    always renders byte-identical JSON.

    Raises ValueError when chain_doc is not a valid chain_doc (per
    describe_schema.validate_chain_doc) or does not describe exactly one
    plugin (stage_fusion's single-plugin convention).
    """
    result = validate_chain_doc(chain_doc)
    if not result["ok"]:
        raise ValueError("invalid chain_doc: "
                         + "; ".join(result["errors"][:3]))
    plugins = chain_doc.get("plugins") or []
    if len(plugins) != 1:
        raise ValueError(f"chain_doc describes {len(plugins)} plugins; "
                         f"replica_proto requires exactly 1 plugin")

    plugin = plugins[0]
    plugin_name = plugin.get("plugin")
    dynamics = _stage_params(plugin, "dynamics")
    points = _frequency_points(plugin)
    return {"frequency_response": _frequency_doc(plugin_name, points),
            "compression": _compression_doc(plugin_name, dynamics),
            "gr_timeline": _gr_doc(plugin_name, dynamics),
            "nonlinear_family": _nonlinear_doc(
                plugin_name, _stage_params(plugin, "nonlinearity")),
            "stereo_width": _stereo_doc(plugin_name, points)}


def write_documents(docs, out_dir):
    """Write the five documents into out_dir as stable, UTF-8 JSON files."""
    out_dir = Path(out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    paths = []
    for name in OUTPUT_NAMES:
        path = out_dir / f"{name}.json"
        path.write_text(json.dumps(docs[name], indent=2, ensure_ascii=False)
                        + "\n", encoding="utf-8")
        paths.append(path)
    return paths


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------


def main(argv=None):
    """CLI entry. Exit 0 = five documents written; 2 = input/output error."""
    parser = argparse.ArgumentParser(
        description="Render the five SPEC.md export documents from one "
                    "chain_doc (issue #106)")
    parser.add_argument("chain_doc", metavar="chain_doc.json",
                        help="one chain_doc describing a single plugin")
    parser.add_argument("--out-dir", metavar="DIR", default="replica_out",
                        help="directory for the five JSON documents "
                             "(default: replica_out)")
    args = parser.parse_args(argv)

    path = Path(args.chain_doc)
    try:
        if not path.is_file():
            raise InputError(f"input not found: {path}")
        try:
            doc = json.loads(path.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError) as exc:
            raise InputError(f"input is not readable JSON ({path}): {exc}")
        try:
            docs = render_replica(doc)
        except ValueError as exc:
            raise InputError(f"input is not a usable chain_doc ({path}): {exc}")
    except InputError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2

    try:
        paths = write_documents(docs, args.out_dir)
    except OSError as exc:
        print(f"error: cannot write output ({exc})", file=sys.stderr)
        return 2

    print("rendered: " + ", ".join(str(item) for item in paths))
    return 0


if __name__ == "__main__":
    sys.exit(main())
