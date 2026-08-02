#pragma once

#include <JuceHeader.h>
#include "SweepRunner.h"
#include "AudioBuffer.h"
#include "../host/PluginManager.h"

/**
 * Represents a single measurement session — the "what" and "how"
 * of a measurement, including which plugin, which parameters,
 * what type of measurement, and the recorded result.
 *
 * Lifetime: create → configure → run → read result → destroy
 */
class MeasurementSession
{
public:
    /** Type of measurement to perform. */
    enum class Type
    {
        frequencyResponse,   // Sine sweep → frequency/phase response
        harmonicAnalysis,    // Multi-tone → THD + harmonics
        compressionCurve     // Tone burst at multiple levels → gain reduction
    };

    MeasurementSession() = default;
    ~MeasurementSession() = default;

    //==============================================================================
    /** Configure the session. */
    void setPluginInstance (juce::AudioPluginInstance* plugin);
    void setPluginDescription (const juce::PluginDescription& desc);
    void setMeasurementType (Type type);
    void setSampleRate (double sr)          { sampleRate = sr; }
    void setBlockSize (int bs)              { blockSize = bs; }

    /** Set a parameter snapshot — record which values are used. */
    void captureParameterSnapshot();

    //==============================================================================
    /** Run the measurement. Blocks until complete. Returns false on error/cancel. */
    bool run();

    /** Request cancellation (thread-safe). */
    void cancel() { runner.cancel(); }

    //==============================================================================
    /** Get the recorded result. */
    CaptureBuffer& getResult() { return runner.getResult(); }
    const CaptureBuffer& getResult() const { return runner.getResult(); }

    /** Get the parameter snapshot. */
    const juce::String& getParameterSnapshot() const { return paramSnapshot; }

    /** Get the measurement type. */
    Type getType() const { return type; }

    /** Get the configured sample rate. */
    double getSampleRate() const { return sampleRate; }

    /** Get the configured block size. */
    int getBlockSize() const { return blockSize; }

    /** Get the fundamental frequencies used for harmonic analysis.
     *  Empty unless the session type is harmonicAnalysis and run() completed. */
    const std::vector<double>& getFundamentalFreqs() const { return fundamentalFreqs; }

    /** Get the tone-burst input levels in dB (20*log10(amplitude)).
     *  Empty unless the session type is compressionCurve and run() completed. */
    std::vector<double> getInputLevelsDB() const;

    /** Get progress (0.0 - 1.0). */
    float getProgress() const { return lastProgress; }

    /** Set progress callback. */
    void setProgressCallback (std::function<void(float)> cb)
    {
        progressCallback = std::move (cb);
    }

private:
    juce::AudioPluginInstance* plugin = nullptr;
    juce::PluginDescription pluginDesc;
    Type type = Type::frequencyResponse;
    double sampleRate = 48000.0;
    int blockSize = 512;

    SweepRunner runner;
    float lastProgress = 0.0f;
    std::function<void(float)> progressCallback;

    juce::String paramSnapshot;

    // Analysis parameters populated by run() from the generated signal.
    std::vector<double> fundamentalFreqs;  // harmonic analysis frequencies
    std::vector<double> lastLevels;        // tone-burst amplitudes (linear)

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MeasurementSession)
};
