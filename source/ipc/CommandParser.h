#pragma once

#include <JuceHeader.h>
#include "../host/PluginManager.h"
#include "../capture/MeasurementSession.h"
#include "../analysis/FreqResponse.h"
#include "../analysis/HarmonicAnalysis.h"
#include "../analysis/CompressionCurve.h"
#include "../analysis/GainReduction.h"
#include "../analysis/TimeConstants.h"
#include "../scan/ScanEngine.h"

/**
 * Aggregated measurement outcome passed to the UI after analysis + export.
 * Exactly one Result field is populated depending on type; the others stay
 * default-constructed (empty).
 *
 * For non-signal input sources (file/noise/dynamic) no analysis runs by
 * default — the result only carries the raw-capture metadata (source /
 * rawSamples / rawSampleRate). The exception is Type::grTimeline, which
 * analyses the dry/wet pair into a GR timeline + tau (gr / tau).
 */
struct MeasurementResults
{
    MeasurementSession::Type type = MeasurementSession::Type::frequencyResponse;
    FreqResponse::Result freq;
    HarmonicAnalysis::Result harmonic;
    CompressionCurve::Result compression;
    GainReduction::Result gr;        // populated for Type::grTimeline
    TimeConstants::Result tau;       // populated for Type::grTimeline

    /** Input source that produced this result (Protocol::Source value). */
    juce::String source;
    int64_t rawSamples = 0;
    double rawSampleRate = 0.0;
};

/**
 * Parses incoming JSON commands and dispatches them to the
 * appropriate subsystems.
 *
 * Commands are processed on the IPC thread. UI-affecting operations
 * are dispatched to the message thread via MessageManager::callAsync.
 */
class CommandParser
{
public:
    CommandParser() = default;
    ~CommandParser() = default;

    //==============================================================================
    /** Set the plugin manager. */
    void setPluginManager (PluginManager* pm)  { pluginManager = pm; }

    /** Set the active measurement session. */
    void setSession (MeasurementSession* s)    { session = s; }

    /** Set the active plugin instance. */
    void setPluginInstance (juce::AudioPluginInstance* p) { plugin = p; }

    /** Set a callback to load a plugin (runs on message thread). */
    void setLoadPluginCallback (std::function<void(const juce::PluginDescription&)> cb)
    {
        loadPluginCallback = std::move (cb);
    }

    /** Set a callback to update status text. */
    void setStatusCallback (std::function<void(const juce::String&)> cb)
    {
        statusCallback = std::move (cb);
    }

    /** Set a callback invoked on the message thread after measurement
     *  completes (analysis + export finished). */
    void setMeasurementCompleteCallback (std::function<void(const MeasurementResults&)> cb)
    {
        measurementCompleteCallback = std::move (cb);
    }

    /** Set a callback invoked after a parameter scan completes (analysis +
     *  export finished). Fires synchronously on the measurement thread,
     *  mirroring setMeasurementCompleteCallback timing. */
    void setScanCompleteCallback (std::function<void(const ScanEngine::ScanResult&)> cb)
    {
        scanCompleteCallback = std::move (cb);
    }

    //==============================================================================
    /** Process a JSON command and return a JSON response. */
    juce::String handleCommand (const juce::String& jsonCommand);

private:
    PluginManager* pluginManager = nullptr;
    MeasurementSession* session = nullptr;
    juce::AudioPluginInstance* plugin = nullptr;

    std::function<void(const juce::PluginDescription&)> loadPluginCallback;
    std::function<void(const juce::String&)> statusCallback;
    std::function<void(const MeasurementResults&)> measurementCompleteCallback;
    std::function<void(const ScanEngine::ScanResult&)> scanCompleteCallback;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CommandParser)
};
