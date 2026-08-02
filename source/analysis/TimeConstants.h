#pragma once

#include <cstdint>
#include <vector>

#include "GainReduction.h"

/**
 * Time-constant estimator for gain-reduction timelines.
 *
 * Recovers the attack/release time constants of a (first-order) compressor
 * from a GainReduction::Result GR-over-time curve. The ideal single-pole
 * compressor produces an exact exponential edge for a level step:
 *
 *   attack : GR(t) = GR_ss * (1 - e^(-t / tau_attack))
 *   release: GR(t) = GR_ss * e^(-t / tau_release)
 *
 * Three estimators are provided:
 *   - edgeTime():  10%-90% edge transition time; for a single pole
 *                  edgeTime = tau * ln(9) ~= tau * 2.1972.
 *   - fitTau():    least-squares fit of the log-domain linearisation
 *                  (ln(1 - GR/GR_ss) vs t for attack, ln(GR/GR_ss) vs t for
 *                  release); the line slope is -1/tau.
 *   - instantaneousTau(): pointwise tau derived from d(GR)/dt, binned by
 *                  level, producing the tau(level) curve family.
 *
 * Event markers are given as sample indices relative to the start of the GR
 * timeline. Edge presence rules:
 *   - attack is present iff attackEnd > 0  (attackStart may be 0);
 *   - release is present iff releaseStart > 0; if releaseEnd <= releaseStart
 *     the release edge runs to the end of the timeline.
 * A missing edge leaves its tau at 0 and its curve family empty.
 *
 * Design notes:
 *   - GR_ss is the mean GR over the last 10% of the attack interval (steady
 *     segment before the edge ends) or the first 10% of the release interval
 *     (steady segment where the decay starts). The log-domain fit is
 *     invariant to a constant GR_ss scaling error, so a biased steady-state
 *     estimate does not bias the fitted tau.
 *   - Points whose log argument is <= 0 (GR beyond the estimated steady
 *     state) or whose |log value| exceeds the fit bound (steady-state noise
 *     floor) are excluded from the fit.
 *   - The level axis of the curve families is the average GR_dB of the points
 *     inside each level bin (0 dB = no reduction, negative dB = reduction);
 *     empty bins fall back to the bin centre so the level axis stays
 *     strictly increasing.
 *   - When GR_ss is near zero (no compression), no estimate is produced and
 *     Result::valid stays false.
 */
class TimeConstants
{
public:
    TimeConstants() = default;
    ~TimeConstants() = default;

    //==============================================================================
    /** Event-edge markers (sample positions relative to the GR timeline start). */
    struct EventMarkers
    {
        int64_t attackStart = 0;   // when the input step rises (sample index)
        int64_t attackEnd = 0;     // when the input returns to steady state / below threshold
        int64_t releaseStart = 0;  // when compression stops and the input falls
        int64_t releaseEnd = 0;    // end of the release process (0 = run to end of timeline)
    };

    /** tau(level) curve family: one level bin per entry. */
    struct TauCurve
    {
        std::vector<double> levelDB;  // bin level axis (average GR_dB of the bin's points)
        std::vector<double> tauSec;   // corresponding tau per bin (seconds)
    };

    /** Full estimation result. */
    struct Result
    {
        double tauAttackSec = 0.0;    // estimated attack tau (seconds), 0 if not estimated
        double tauReleaseSec = 0.0;   // estimated release tau (seconds), 0 if not estimated
        TauCurve attackByLevel;       // tau_attack(level) curve family
        TauCurve releaseByLevel;      // tau_release(level) curve family
        bool valid = false;           // false when nothing could be estimated
    };

    //==============================================================================
    /** Estimate attack/release time constants from a GR timeline.
     *  @param gr       GainReduction output (the GR-over-time curve)
     *  @param markers  event-edge markers in samples (see EventMarkers)
     *  @param sr       sample rate of the timeline
     */
    static Result estimate (const GainReduction::Result& gr,
                            const EventMarkers& markers,
                            double sr);

    /** 10%-90% edge transition time (seconds).
     *  @param edge  "attack" (rising edge) or "release" (falling edge)
     *  Returns 0 when the edge is missing or too flat to measure.
     */
    static double edgeTime (const GainReduction::Result& gr,
                            const EventMarkers& markers,
                            const char* edge, double sr);

    /** Single-pole tau (seconds) from a least-squares fit of the log-domain
     *  linearisation of the given edge points:
     *  attack : ln(1 - GR/GR_ss) = -t/tau,   release : ln(GR/GR_ss) = -t/tau.
     *  Returns 0 when the fit is impossible (too few points, GR_ss ~ 0, ...).
     */
    static double fitTau (const std::vector<GainReduction::Point>& points,
                          const char* edge, double sr);

    /** Instantaneous tau(t) derived from d(GR)/dt, binned by GR level into
     *  numBins bins spanning the edge's GR range.
     *  @param edge  "attack" or "release"
     */
    static TauCurve instantaneousTau (const GainReduction::Result& gr,
                                      const EventMarkers& markers,
                                      const char* edge, double sr,
                                      int numBins = 10);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TimeConstants)
};
