#pragma once

#include <JuceHeader.h>
#include "../host/PluginManager.h"
#include "../capture/MeasurementSession.h"
#include "../capture/ParameterTimeline.h"
#include "../analysis/FreqResponse.h"
#include "../analysis/HarmonicAnalysis.h"
#include "../analysis/CompressionCurve.h"
#include "../analysis/GainReduction.h"
#include "../analysis/TimeConstants.h"
#include "../scan/ScanEngine.h"
#include "../host/ChildMeasureContract.h"

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

    /** Set the out-of-process measurement callback (block D, D6). Invoked
     *  synchronously by the measure command when the loaded plugin is
     *  blacklisted — a blacklisted plugin is never measured in the host.
     *  Implemented by ChildMeasureOrchestrator (host-side); empty by default,
     *  in which case the measure command fails with an explicit error instead
     *  of falling back to host-direct measurement. */
    void setChildMeasureCallback (ChildMeasureContract::Callback cb)
    {
        childMeasureCallback = std::move (cb);
    }

    /** Set the child-measure target path (block D, D6 routing gap fix). The
     *  host never loads a blacklisted plugin (B+ decision), so there is no
     *  host plugin instance for the measure command to resolve — the host
     *  load path (Main.cpp loadPluginByDescription) reports the skipped
     *  plugin's fileOrIdentifier here instead. With no plugin instance and
     *  an empty path the measure command fails with "no session or plugin";
     *  with a non-empty (blacklisted) path it routes to the child. */
    void setChildMeasurePath (const juce::String& p) { childMeasurePath = p; }

    /** Set a callback invoked by the stop command (issue #3): cancels the
     *  in-flight job — the measurement session AND the out-of-process
     *  orchestrator. Runs on the pipe read thread (control command), so it
     *  must be non-blocking (atomic flag sets only). Wired by Main.cpp. */
    void setCancelRequestCallback (std::function<void()> cb)
    {
        cancelRequestCallback = std::move (cb);
    }

    //==============================================================================
    /** Process a JSON command and return a JSON response. */
    juce::String handleCommand (const juce::String& jsonCommand);

private:
    PluginManager* pluginManager = nullptr;
    MeasurementSession* session = nullptr;
    juce::AudioPluginInstance* plugin = nullptr;

    // Parameter-automation recorder (B2, recordTimeline/stopTimeline).
    // Owned here — recording is non-blocking event capture, independent of
    // the session's playback timeline.
    ParameterTimeline timeline;

    std::function<void(const juce::PluginDescription&)> loadPluginCallback;
    std::function<void(const juce::String&)> statusCallback;
    std::function<void(const MeasurementResults&)> measurementCompleteCallback;
    std::function<void(const ScanEngine::ScanResult&)> scanCompleteCallback;
    std::function<void()> cancelRequestCallback;   // issue #3: stop → in-flight job cancel
    ChildMeasureContract::Callback childMeasureCallback;

    // D6: child-measure target path of a blacklisted plugin that was never
    // loaded in the host (set by the host load path; see the measure case).
    juce::String childMeasurePath;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CommandParser)
};
