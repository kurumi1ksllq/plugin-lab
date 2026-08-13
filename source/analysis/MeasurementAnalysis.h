#pragma once

#include "../capture/MeasurementSession.h"
#include "FreqResponse.h"
#include "HarmonicAnalysis.h"
#include "CompressionCurve.h"

/**
 * Single mapping between the session measurement type and the analyzer
 * that produces its result (issue #42).
 *
 * Before this module, the type→analyzer dispatch (analyzer construction,
 * latency setup, session-configuration reads) was inlined at every site
 * that analyses a recorded measurement: CommandParser::runAndAnalyze
 * (measure/dataset) and ScanEngine::run (per scan round) each carried a
 * verbatim copy of the same switch. Adding a fifth measurement type meant
 * editing both. The dispatch now lives exactly here; both callers share
 * it.
 *
 * The child-measure path (ChildWavAnalyzer) and the compression-family
 * grid (CompressionFamily) keep their own entry points — different seams
 * (WAV-file analysis vs in-memory session) — but the signal-source
 * mapping is the one below.
 */
namespace MeasurementAnalysis
{
    /** One analysis slot per signal-source measurement type; exactly one
     *  Result field is populated depending on the session type. */
    struct SignalResult
    {
        FreqResponse::Result freq;
        HarmonicAnalysis::Result harmonic;
        CompressionCurve::Result compression;
    };

    /** Runs the analyzer matching session.getType() over the session's
     *  recorded dry/wet buffers and returns the per-type result.
     *
     *  @param session        The already-run measurement session (the
     *                        excitation / fundamental / level configuration
     *                        is read from it).
     *  @param latencySamples The plugin's latency in samples, applied to the
     *                        frequency-response analyser (phase ramp
     *                        compensation); ignored by the other types.
     *
     *  Type::grTimeline is not a signal-source measurement (the parsers
     *  reject it) — the returned SignalResult stays all-empty for it,
     *  keeping the enum switch exhaustive.
     */
    SignalResult analyzeByType (MeasurementSession& session, int latencySamples);
}
