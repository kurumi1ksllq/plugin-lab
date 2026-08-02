#pragma once

#include <JuceHeader.h>
#include "../host/PluginManager.h"
#include "../capture/MeasurementSession.h"
#include "../analysis/FreqResponse.h"
#include "../analysis/HarmonicAnalysis.h"
#include "../analysis/CompressionCurve.h"

/**
 * Aggregated measurement outcome passed to the UI after analysis + export.
 * Exactly one Result field is populated depending on type; the others stay
 * default-constructed (empty).
 */
struct MeasurementResults
{
    MeasurementSession::Type type = MeasurementSession::Type::frequencyResponse;
    FreqResponse::Result freq;
    HarmonicAnalysis::Result harmonic;
    CompressionCurve::Result compression;
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

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CommandParser)
};
