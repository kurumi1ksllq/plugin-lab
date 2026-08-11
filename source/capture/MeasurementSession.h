#pragma once

#include <JuceHeader.h>
#include "SweepRunner.h"
#include "AudioBuffer.h"
#include "ParameterTimeline.h"
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
        compressionCurve,    // Tone burst at multiple levels → gain reduction
        grTimeline           // GR-over-time + attack/release tau (non-signal sources)
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

    /** Select the frequency-response excitation: true = MLS (fast,
     *  full-band deconvolution), false = sine sweep (default, backward
     *  compatible). */
    void setFreqExcitation (bool useMLS) { freqExcitationMLS = useMLS; }
    bool getFreqExcitation() const       { return freqExcitationMLS; }

    /** MLS sequence length used by the MLS frequency-response excitation. */
    int getFreqMLSLength() const         { return freqMLSLength; }

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

    // --- dynamic source configuration (T4.3: CompressionFamily) ---
    // All defaults reproduce the original dynamic-source signal exactly, so
    // existing dynamic commands are unaffected until a setter is called.
    /** Carrier amplitude (linear 0..1). Default 0.5. */
    void setDynamicAmplitude (double amp)   { dynamicAmplitude = amp; }
    double getDynamicAmplitude() const      { return dynamicAmplitude; }

    /** Envelope speed multiplier (EnvelopeSignal::setSpeed). Default 1.0. */
    void setDynamicSpeed (double speed)     { dynamicSpeed = speed; }
    double getDynamicSpeed() const          { return dynamicSpeed; }

    /** ADSR envelope in seconds (attack/decay/sustain/release).
     *  Default (0.02, 0.1, 0.8, 0.2). */
    void setDynamicADSR (double attackSec, double decaySec,
                         double sustain, double releaseSec);
    /** Sweep start frequency (Hz) of the dynamic carrier. Default 20.0
     *  (the original 20 Hz..20 kHz sweep). */
    void setDynamicCarrierStartHz (double hz) { dynamicCarrierStartHz = hz; }
    double getDynamicCarrierStartHz() const   { return dynamicCarrierStartHz; }

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

    /** Register a per-block callback (T4.4 live GR header): invoked after
     *  every processed block with the total progress and the dry/wet block
     *  contents just appended to the result. Forwarded to SweepRunner by
     *  run() (which may wrap it with timeline-playback handling). */
    void setBlockCallback (std::function<void(float progress,
                                               const juce::AudioBuffer<float>& dryBlock,
                                               const juce::AudioBuffer<float>& wetBlock)> cb)
    {
        blockCallback = std::move (cb);
    }

    /** Set a playback-progress callback (issue #2): fired on the measurement
     *  (message) thread whenever the timeline cursor advances during a
     *  playTimeline run — (eventIndex, eventTotal, elapsedMs). The GUI shows
     *  the event index; the IPC path forwards it as a progress line. */
    void setPlaybackProgressCallback (std::function<void (int eventIndex, int eventTotal, int64_t elapsedMs)> cb)
    {
        playbackProgressCallback = std::move (cb);
    }

    /** Set a parameter-automation timeline to play during the next run()
     *  (B2 playTimeline): its events are applied to the plugin between
     *  blocks (elapsed wall-clock ms since the run started), and the
     *  affected parameters are restored to the values they hold at the
     *  time of this call after the run finishes (R2). One-shot — run()
     *  consumes the timeline and resets it. */
    void setTimelinePlayback (std::vector<TimelineEvent> events, double playbackRate);

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

    // T4.3: dynamic-source configuration (defaults = original signal).
    double dynamicAmplitude = 0.5;
    double dynamicSpeed = 1.0;
    double dynamicCarrierStartHz = 20.0;
    double dynamicADSR[4] = { 0.02, 0.1, 0.8, 0.2 };

    // Source metadata populated by run() (file playback only).
    juce::String sourceFilePath;
    double sourceSampleRate = 0.0;
    double resampleRatio = 0.0;
    double sourceDurationSec = 0.0;

    SweepRunner runner;
    float lastProgress = 0.0f;
    std::function<void(float)> progressCallback;
    std::function<void(float, const juce::AudioBuffer<float>&, const juce::AudioBuffer<float>&)> blockCallback;
    std::function<void(int, int, int64_t)> playbackProgressCallback;

    // Timeline playback state (B2). timelineRestore holds the pre-play
    // values of the parameters the playback timeline touches (R2).
    ParameterTimeline timelinePlayback;
    bool timelinePlaybackActive = false;
    std::vector<std::pair<juce::String, float>> timelineRestore;

    juce::String paramSnapshot;

    // Analysis parameters populated by run() from the generated signal.
    std::vector<double> fundamentalFreqs;  // harmonic analysis frequencies
    std::vector<double> lastLevels;        // tone-burst amplitudes (linear)

    // Frequency-response excitation selection (default: sine sweep).
    bool freqExcitationMLS = false;
    int freqMLSLength = 16383;   // 2^14 - 1: full LFSR period at 48 kHz ≈ 0.34 s

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MeasurementSession)
};
