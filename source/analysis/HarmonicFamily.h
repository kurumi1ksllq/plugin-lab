#pragma once

#include <functional>
#include <vector>

#include "HarmonicAnalysis.h"
#include "../capture/MeasurementSession.h"

/**
 * Harmonics-family measurement: THD versus input level (issue #102,
 * nonlinear_family).
 *
 * One cell per configured input level (dBFS): the session runs its harmonic
 * excitation (one tone per segment, single-tone-per-segment — never a
 * multi-tone mix, DESIGN.md §3.2) with the carrier amplitude set to that
 * level, and the recorded wet audio is analysed into per-tone THD. The
 * resulting curve separates the three non-linearity families a reverse
 * engineer cares about:
 *
 *   - hard clipping : THD rises fast and then plateaus (output pinned)
 *   - soft saturation: THD rises smoothly across the whole level range
 *   - harmonic saturation: THD rises but the dominant order stays low
 *
 * Design notes:
 *   - Mirrors CompressionFamily (levelsDB x analyser grid, process-global
 *     cancellation, progress callback), but there is only one grid axis, so
 *     entries == levelsDB.size().
 *   - The plugin is NOT part of the signature: unlike the compression family
 *     (which needs plugin->getLatencySamples() to align dry/wet), the harmonic
 *     analyser works on the wet buffer alone.
 *   - The amplitude override goes through
 *     MeasurementSession::setHarmonicAmplitudeDB, which leaves the default
 *     excitation (literal 0.4 amplitude) untouched — the existing harmonic
 *     export stays byte-identical when no family grid is requested.
 *   - The session is long-lived and shared with every other command, so the
 *     override is scoped: measure() saves the previous amplitude state and
 *     restores it on every exit path (including early return and cancellation).
 *     Without that, a later single-level harmonic measurement would inherit the
 *     last cell's level and stop being byte-compatible.
 *
 * Threading: measure() may run on any thread (the session yields to the
 * message loop when running on the message thread). The cancellation flag is
 * process-global: this application runs one family measurement at a time.
 */
class HarmonicFamily
{
public:
    /** Result of a single input level. */
    struct FamilyEntry
    {
        double inputLevelDB = 0.0;                    // configured input level (dBFS)
        std::vector<HarmonicAnalysis::ToneResult> tones;  // one per fundamental
        bool valid = false;                           // run completed + analysable
    };

    /** Result of the full level grid. */
    struct FamilyResult
    {
        std::vector<double> levelsDB;                 // requested input levels (dBFS)
        std::vector<FamilyEntry> entries;             // entries <= levelsDB.size()
                                                      // (shorter when a level failed or the run was cancelled)
        bool cancelled = false;
    };

    //==============================================================================
    /** Execute a THD-versus-level measurement.
     *  @param session  the measurement session reused for every level (harmonic
     *                  source; amplitude overwritten per cell)
     *  @param levelsDB input levels in dBFS (carrier amplitude = 10^(db/20))
     *  @param progress called with (done, total) after each level
     *  Returns the partially-filled result with cancelled == true when
     *  cancel() was requested between levels. */
    static FamilyResult measure (MeasurementSession* session,
                                 const std::vector<double>& levelsDB,
                                 std::function<void(int done, int total)> progress);

    /** Request cancellation (thread-safe). Takes effect at the next level
     *  boundary of a running measure(). */
    static void cancel();

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (HarmonicFamily)
};
