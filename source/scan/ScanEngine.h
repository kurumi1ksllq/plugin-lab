#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <atomic>
#include <functional>
#include <vector>

#include "../analysis/FreqResponse.h"
#include "../analysis/HarmonicAnalysis.h"
#include "../analysis/CompressionCurve.h"
#include "../capture/MeasurementSession.h"

/**
 * Parameter scan engine.
 *
 * Runs a measurement session once per parameter value (a "round"),
 * collecting the per-round analysis results plus the plugin latency
 * observed after each round, so a caller can sweep a parameter and
 * study how the plugin's response changes.
 *
 * The engine only orchestrates session->run() + analysis; it owns no
 * signal generation or export logic (those live in MeasurementSession /
 * Export). All plugin parameters are snapshotted on entry and restored
 * on exit (including cancellation), via an RAII guard.
 *
 * Threading: run() may be called from any thread. setValueNotifyingHost
 * is followed by a short message-loop yield ONLY when run() executes on
 * the message thread (mirroring CommandParser's dispatch strategy); the
 * SweepRunner inside MeasurementSession yields the loop itself while
 * processing.
 */
class ScanEngine
{
public:
    /** Result of a single scan round (one parameter value). */
    struct ScanResultEntry
    {
        double paramValue = 0.0;          // normalized value
        juce::String paramValueText;      // param->getText(value, readable length)
        int latencySamples = 0;           // plugin->getLatencySamples() after the round
        FreqResponse::Result freq;        // populated for Type::frequencyResponse
        HarmonicAnalysis::Result harmonic; // populated for Type::harmonicAnalysis
        CompressionCurve::Result compression; // populated for Type::compressionCurve
        bool cancelled = false;
    };

    /** Result of a full scan across all values. */
    struct ScanResult
    {
        juce::String paramId, paramName;
        std::vector<double> values;
        std::vector<ScanResultEntry> family;
        bool cancelled = false;
    };

    ScanEngine() = default;
    ~ScanEngine() = default;

    /** Set the plugin whose parameter is scanned (also the latency source). */
    void setPluginInstance (juce::AudioPluginInstance* plugin);

    /** Set the measurement session reused for every round. */
    void setSession (MeasurementSession* session);

    /** Scan one parameter across the given normalized values (0..1).
     *  Blocks until complete. On cancellation, the partially filled result
     *  is returned with cancelled == true; parameters are always restored.
     *  progress(round, totalRounds) is invoked after each round. */
    ScanResult run (const juce::String& paramId,
                    const std::vector<float>& values,   // normalized 0..1
                    MeasurementSession::Type type,
                    std::function<void(int round, int totalRounds)> progress);

    /** Request cancellation (thread-safe). Takes effect at the next round
     *  boundary of a running scan. */
    void cancel();

private:
    juce::AudioPluginInstance* plugin_ = nullptr;
    MeasurementSession* session_ = nullptr;
    std::atomic<bool> cancelled_ { false };
};
