#pragma once

#include <JuceHeader.h>
#include <mutex>
#include "../host/PluginManager.h"
#include "../capture/MeasurementSession.h"
#include "../capture/ParameterTimeline.h"
#include "../analysis/Export.h"
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
    void setSession (MeasurementSession* s)
    {
        // Lock-guarded like the plugin pointer (issue #33 D2): the session is
        // set once before the pipe server starts, but the lock keeps the
        // swap/read pairing uniform.
        std::lock_guard<std::mutex> lock (pluginLock);
        sessionPtr = s;
    }

    /** Set the active plugin instance.
     *
     *  Issue #33 D2: the pointer is swapped on the message thread while
     *  commands dereference it on the IPC worker — the swap is lock-guarded
     *  against every worker read. The caller must NOT destroy the old
     *  instance until AFTER the swap returns (null first, destroy second),
     *  so a worker that grabbed the lock earlier can never dereference a
     *  destroyed instance. */
    void setPluginInstance (juce::AudioPluginInstance* p)
    {
        std::lock_guard<std::mutex> lock (pluginLock);
        pluginPtr = p;
    }

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
    //==============================================================================
    // Command-family handlers (issue #42): handleCommand is a thin router —
    // it parses the JSON, matches the command family and delegates here.
    // Each handler is a verbatim move of the pre-refactor inline branch:
    // same guard strings (cmd is passed by the router so the conditions
    // read exactly as before), same validation order, same JSON responses.
    // obj is the parsed request object; cmd the request's "cmd" string.

    /** Control commands served directly on the worker thread (non-blocking,
     *  atomic/snapshot-only): getScanStatus / loadPlugin / setParam /
     *  getParams / stop. */
    juce::String handleControlCommands (const juce::DynamicObject& obj, const juce::String& cmd);

    /** measure — a blocking command; the run body is dispatched to the
     *  message thread via dispatchToMessageThread so processBlock runs
     *  there (required by Pro-Q 4 and similar VST3 plugins). */
    juce::String handleMeasurementCommands (const juce::DynamicObject& obj, const juce::String& cmd);

    /** scan — parameter sweep across values; same blocking dispatch as
     *  measure. */
    juce::String handleScanCommands (const juce::DynamicObject& obj, const juce::String& cmd);

    /** dataset (measurement battery) / exportWav (offline dry/wet WAV
     *  export of the last measurement). */
    juce::String handleDatasetExportCommands (const juce::DynamicObject& obj, const juce::String& cmd);

    /** recordTimeline / stopTimeline / playTimeline — parameter-automation
     *  recording, export and playback. */
    juce::String handleTimelineCommands (const juce::DynamicObject& obj, const juce::String& cmd);

    /** Shared blocking dispatch for long commands (measure/scan/dataset/
     *  playTimeline): executes synchronously when already on the message
     *  thread (unit tests / message callbacks), otherwise callAsync +
     *  WaitableEvent so the body runs on the message thread. One helper
     *  replaces the four textually-identical dispatch blocks. */
    static juce::String dispatchToMessageThread (const std::function<juce::String()>& run);

    PluginManager* pluginManager = nullptr;
    MeasurementSession* sessionPtr = nullptr;
    juce::AudioPluginInstance* pluginPtr = nullptr;

    // Issue #33 D2: serializes the plugin/session pointer swap (message
    // thread, setPluginInstance/setSession) against worker-thread reads in
    // handleCommand. Never held across a message-thread dispatch (the
    // dispatched body runs on the message thread, where the D1 in-flight-job
    // guard already serializes unloads — holding the lock across done.wait()
    // would deadlock with the message thread).
    mutable std::mutex pluginLock;

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

    // Issue #39: the last successful in-process measurement (measure command
    // or dataset battery), captured together with the export context frozen
    // at measurement time — the exportData command re-exports this pair as a
    // dataset JSON without re-running. Written inside the dispatched command
    // bodies on the message thread; read by exportData on the IPC worker.
    // The single-worker FIFO queue plus the WaitableEvent handoff between
    // commands order every write before the next command's read; the lock
    // keeps the access uniform with the plugin/session pointer rule (issue
    // #33 D2: copy under lock, use outside).
    MeasurementResults lastResults;
    Export::Context lastExportContext;
    bool hasLastResults = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CommandParser)
};
