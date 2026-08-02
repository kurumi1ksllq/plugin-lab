#pragma once

#include <functional>
#include <vector>

#include <juce_audio_processors/juce_audio_processors.h>

#include "../capture/MeasurementSession.h"
#include "CompressionCurve.h"
#include "GainReduction.h"
#include "TimeConstants.h"

/**
 * Compression-family measurement: a grid over (input level × envelope speed)
 * of a compressor's response family.
 *
 * Each cell runs the session's dynamic source once — a 2 s sine sweep shaped
 * by an ADSR envelope whose speed (and thus edge rate) is set per cell — and
 * post-processes the recorded dry/wet pair into:
 *   - the static compression curve (CompressionCurve::analyze),
 *   - the gain-reduction timeline (GainReduction::analyze),
 *   - the attack/release time constants (TimeConstants::estimate).
 *
 * Design notes (deviations from the simplest marker scheme, documented):
 *   - The GR timeline stored in each entry uses the TimeConstants convention
 *     (positive dB = reduction). GainReduction::analyze reports the wet/dry
 *     ratio (negative dB for a compressor), so measure() negates the timeline.
 *   - detectMarkers() sets attackEnd at the peak (the end of the attack edge,
 *     i.e. the start of the sustain plateau) instead of at the release start;
 *     including the sustain/release regions in the attack interval would let
 *     their (log-domain) points pollute the attack fit. The release marker is
 *     placed where the reduction falls to a ~1 dB residual — the input has
 *     dropped below the threshold there and the decay is the compressor's own
 *     release pole, so the fit sees a pure exponential.
 *   - The envelope attack/release edges are linear ramps, not steps. On a
 *     compressor whose detector is instantaneous and whose smoothing is
 *     direction-dependent (up with tau_attack, down with tau_release), an AC
 *     carrier makes the attack edge charge with the effective time constant
 *     2*tau_a*tau_r/(tau_a+tau_r) (the falling half-cycles release with
 *     tau_release and slow the charge) — the tau test accounts for this. The
 *     release edge is driven by a DC zero target below the threshold and
 *     measures the configured release directly.
 *
 * Threading: measure() may run on any thread. It yields to the message loop
 * through the SweepRunner inside MeasurementSession when running on the
 * message thread (the GUI stays responsive); the per-cell cancellation flag
 * is process-global (this application runs one family measurement at a time).
 */
class CompressionFamily
{
public:
    /** Result of a single (level × speed) cell. */
    struct FamilyEntry
    {
        double inputLevelDB = 0.0;            // configured input level (dBFS)
        double speed = 1.0;                   // EnvelopeSignal::setSpeed value
        CompressionCurve::Result curve;       // static compression curve
        GainReduction::Result gr;             // GR timeline (positive dB = reduction)
        TimeConstants::Result tau;            // attack/release time constants
        bool valid = false;                   // measurement completed + analyzable
    };

    /** Result of the full grid. */
    struct FamilyResult
    {
        std::vector<double> levelsDB;         // scanned input levels (dB)
        std::vector<double> speeds;           // scanned dynamic speeds
        std::vector<FamilyEntry> entries;     // size == levelsDB.size() * speeds.size()
        bool cancelled = false;
    };

    //==============================================================================
    /** Execute a compression-response-family measurement.
     *  @param plugin   the plugin under test (already prepared externally);
     *                  also the latency source
     *  @param session  the measurement session reused for every cell (dynamic
     *                  source; config overwritten per cell)
     *  @param levelsDB input levels in dBFS (carrier amplitude = 10^(db/20))
     *  @param speeds   dynamic envelope speeds (EnvelopeSignal::setSpeed)
     *  @param progress called with (done, total) after each cell
     *  Returns the partially-filled result with cancelled == true when
     *  cancel() was requested between cells. */
    static FamilyResult measure (juce::AudioPluginInstance* plugin,
                                 MeasurementSession* session,
                                 const std::vector<double>& levelsDB,
                                 const std::vector<double>& speeds,
                                 std::function<void(int done, int total)> progress);

    /** Locate the attack/release event edges of a GR timeline (sample
     *  indices), for TimeConstants::estimate.
     *  Expected convention: positive dB = reduction.
     *  Strategy (simplified, deterministic):
     *    - no edge when the peak reduction is below 0.5 dB;
     *    - attackStart: first rising crossing of half the peak reduction;
     *    - attackEnd:   first index reaching the peak reduction (start of the
     *      sustain plateau — the attack edge has converged there);
     *    - releaseStart: first point after the peak where the reduction has
     *      fallen to the ~1 dB residual (pure release-pole region);
     *    - releaseEnd: 0 (the release edge runs to the end of the timeline). */
    static TimeConstants::EventMarkers detectMarkers (const GainReduction::Result& gr);

    /** Request cancellation (thread-safe). Takes effect at the next cell
     *  boundary of a running measure(). */
    static void cancel();

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CompressionFamily)
};
