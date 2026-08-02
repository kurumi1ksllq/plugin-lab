#pragma once

#include <JuceHeader.h>
#include "SweepRunner.h"
#include "AudioBuffer.h"
#include "../host/PluginManager.h"
#include "../signal/NoiseGenerator.h"

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

    /** Input signal source. The source decides which signal is generated;
     *  Type remains an analysis field. Analysis is only performed for
     *  Source::signal — file/noise/dynamic are captured raw (phase 4). */
    enum class Source
    {
        signal,    // Built-in analytical signals (sweep / multi-tone / tone-burst)
        file,      // Audio file playback
        noise,     // Deterministic white/pink noise
        dynamic    // Enveloped carrier with a dynamic level
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

    /** Select the input signal source (default: signal). */
    void setSource (Source s)               { source = s; }
    Source getSource() const                { return source; }

    // --- file source ---
    void setFilePath (const juce::File& f)  { filePath = f; }
    const juce::File& getFilePath() const   { return filePath; }

    // --- noise source ---
    void setNoiseConfig (NoiseGenerator::Type type, double durationSec, uint32_t seed);
    NoiseGenerator::Type getNoiseType() const { return noiseType; }
    double getNoiseDuration() const         { return noiseDuration; }
    uint32_t getNoiseSeed() const           { return noiseSeed; }

    // --- dynamic source ---
    void setDynamicCarrierFreq (double hz)  { dynamicCarrierFreq = hz; }
    double getDynamicCarrierFreq() const    { return dynamicCarrierFreq; }

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

    //==============================================================================
    /** Source metadata captured by run() from the generator (used for export).
     *  Only populated for the file source. */
    const juce::String& getSourceFilePath() const { return sourceFilePath; }
    double getSourceSampleRate() const { return sourceSampleRate; }
    double getResampleRatio() const { return resampleRatio; }
    double getSourceDurationSec() const { return sourceDurationSec; }

private:
    juce::AudioPluginInstance* plugin = nullptr;
    juce::PluginDescription pluginDesc;
    Type type = Type::frequencyResponse;
    Source source = Source::signal;
    double sampleRate = 48000.0;
    int blockSize = 512;

    // Source configuration (used by run() when source != signal).
    juce::File filePath;
    NoiseGenerator::Type noiseType = NoiseGenerator::Type::white;
    double noiseDuration = 2.0;
    uint32_t noiseSeed = 0x2E42A5;
    double dynamicCarrierFreq = 1000.0;

    // Source metadata populated by run() (file playback only).
    juce::String sourceFilePath;
    double sourceSampleRate = 0.0;
    double resampleRatio = 0.0;
    double sourceDurationSec = 0.0;

    SweepRunner runner;
    float lastProgress = 0.0f;
    std::function<void(float)> progressCallback;

    juce::String paramSnapshot;

    // Analysis parameters populated by run() from the generated signal.
    std::vector<double> fundamentalFreqs;  // harmonic analysis frequencies
    std::vector<double> lastLevels;        // tone-burst amplitudes (linear)

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MeasurementSession)
};
