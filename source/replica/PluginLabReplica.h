#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <array>
#include <atomic>
#include <cmath>
#include <utility>

#include "ReplicaChain.h"
#include "../utils/CrashLog.h"

//==============================================================================
/**
    A spec-driven replica VST3 (issue #27, T4): the plugin blueprint of the
    PluginLab closed loop — AI measures a chain_doc (describe_chain.py), and
    this plugin mirrors that processing so the T5 scan loop can drive it like
    any real plugin.

    SPEC RESOLUTION (decision A):
    The constructor reads the chain_doc from, in order:
      1. the PLUGINLAB_REPLICA_SPEC environment variable (absolute path), or
      2. %APPDATA%/PluginLab/replica_spec.json (juce userApplicationData
         Directory), or
      3. neither exists / unparseable / no usable_as_spec entry -> IDENTITY
         chain (decision U2: dry == wet, harmless default) + CRASH_LOG_WARN
         with the reason.
    Parsing goes through ReplicaSpec::fromChainDoc (first plugins[] entry with
    usable_as_spec == true; plausible eq.sections; compression from the
    *_derived slots; gr tau converted ms -> s). The env var is read via
    juce::SystemStats::getEnvironmentVariable (wide environment on Windows).

    PARAMETER SURFACE (decision B/U3):
    21 hosted parameters with STABLE IDs and classifier-pattern display names
    (describe_chain.py _BAND_KEY_RE `Band \d+ (Used|Frequency|Q|Gain)` +
    _DYNAMICS_SUBSTRINGS Threshold/Ratio/Attack/Release/Makeup/Gain), so the
    T5 scan loop classifies this plugin the way it classifies the original:
      4 EQ band slots x (Band N Used / Band N Frequency / Band N Gain /
      Band N Q)  — IDs bandN_used / bandN_freq / bandN_gain / bandN_q,
      ranges 20..20000 Hz, -24..24 dB, 0.1..20.
      Threshold / Ratio / Attack / Release / Makeup Gain — IDs threshold /
      ratio / attack / release / makeup_gain; Attack/Release are exposed in ms
      (the chain consumes seconds; conversion happens in applyParamConfig).
    Param DEFAULTS are initialised from the spec: the first min(4, bands.size())
    spec bands fill slots 1..N (Used=1, their freq/gain/q); extra spec bands
    beyond 4 are dropped (fixed surface). Empty slots are bypassed (Used=0).
    With compression in the spec the compressor params default to its
    threshold/ratio/attack/release; without, they default to -20 dB / 4 / 5 ms /
    50 ms but the chain stays identity (see COMPRESSOR RULE).

    COMPRESSOR RULE (decision U6, deterministic):
    The compressor is ENABLED iff the spec had compression (hasCompression) OR
    at least one of Threshold/Ratio/Attack/Release has been explicitly set by
    the host to a value different from its default (tolerance 1e-6 normalized —
    a host re-applying the exact default at load never arms it). When enabled,
    the CURRENT parameter values are used. Makeup Gain only applies when the
    compressor is enabled (ReplicaChain semantics).

    DSP DRIVING (decision U6):
    At the START of each processBlock the live hosted-param values are read
    (the params' stored normalized values — setValue on any thread, read once
    per block on the audio thread; std::atomic, no locks) and the chain config
    is rebuilt if a dirty flag was set. A band slot is active when Used > 0.5
    and Frequency > 1.0 (guard against an automation-armed slot with a 0 Hz
    default frequency producing a degenerate biquad). prepareToPlay applies
    the initial config from the (spec-seeded) defaults and arms no dirty flag;
    reset() forwards to the chain. Zero latency, zero tail.
*/
class PluginLabReplica final : public juce::AudioPluginInstance
{
public:
    //==============================================================================
    /** A concrete HostedAudioProcessorParameter with a normalized <-> real
        mapping. Every setValue marks the shared dirty flag (block-start
        rebuild); the compression params additionally flip a shared "touched"
        flag when the new value differs from the default (compressor rule). */
    class ReplicaParameter final : public juce::HostedAudioProcessorParameter
    {
    public:
        ReplicaParameter (juce::String parameterIDToUse, juce::String parameterNameToUse,
                          juce::String parameterLabelToUse, float minRealToUse,
                          float maxRealToUse, float defaultReal,
                          std::atomic<bool>& dirtyToUpdate,
                          std::atomic<bool>* touchedToUpdate = nullptr)
            : parameterID (std::move (parameterIDToUse)),
              parameterName (std::move (parameterNameToUse)),
              parameterLabel (std::move (parameterLabelToUse)),
              minReal (minRealToUse),
              maxReal (maxRealToUse),
              dirty (dirtyToUpdate),
              touched (touchedToUpdate)
        {
            setDefaultReal (defaultReal);
        }

        juce::String getParameterID() const override               { return parameterID; }
        float getValue() const override                            { return value.load(); }
        float getDefaultValue() const override                     { return defaultNorm; }
        juce::String getName (int) const override                  { return parameterName; }
        juce::String getLabel() const override                     { return parameterLabel; }
        float getValueForText (const juce::String&) const override { return value.load(); }

        void setValue (float newValue) override
        {
            const float clamped = juce::jlimit (0.0f, 1.0f, newValue);
            value.store (clamped);
            dirty.store (true);
            if (touched != nullptr && std::abs (clamped - defaultNorm) > 1e-6f)
                touched->store (true);
        }

        /** Re-seeds the default AND current value (spec-driven defaults are
            applied after construction in the wrapper constructor). */
        void setDefaultReal (float newDefaultReal)
        {
            defaultReal = newDefaultReal;
            defaultNorm = (newDefaultReal - minReal) / (maxReal - minReal);
            value.store (defaultNorm);
        }

        /** Maps the stored normalized value back to the real domain. */
        float getRealValue() const noexcept
        {
            return minReal + value.load() * (maxReal - minReal);
        }

    private:
        juce::String parameterID;
        juce::String parameterName;
        juce::String parameterLabel;
        float minReal = 0.0f;
        float maxReal = 1.0f;
        float defaultReal = 0.0f;
        float defaultNorm = 0.0f;
        std::atomic<float> value { 0.0f };
        std::atomic<bool>& dirty;
        std::atomic<bool>* touched;
    };

    //==============================================================================
    PluginLabReplica();

    ~PluginLabReplica() override = default;

    //==============================================================================
    // AudioProcessor
    //==============================================================================

    const juce::String getName() const override                       { return "PluginLabReplica"; }
    double getTailLengthSeconds() const override                      { return 0.0; }
    bool acceptsMidi() const override                                 { return false; }
    bool producesMidi() const override                                { return false; }
    juce::AudioProcessorEditor* createEditor() override               { return nullptr; }
    bool hasEditor() const override                                   { return false; }

    int getNumPrograms() override                                     { return 0; }
    int getCurrentProgram() override                                  { return 0; }
    void setCurrentProgram (int) override                             {}
    const juce::String getProgramName (int) override                  { return {}; }
    void changeProgramName (int, const juce::String&) override        {}

    void prepareToPlay (double sampleRateToUse, int maximumExpectedSamplesPerBlock) override;
    void releaseResources() override                                 {}
    void reset() override;

    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages) override;
    void processBlock (juce::AudioBuffer<double>& buffer, juce::MidiBuffer& midiMessages) override;

    void getStateInformation (juce::MemoryBlock&) override            {}
    void setStateInformation (const void*, int) override               {}

    //==============================================================================
    // AudioPluginInstance
    //==============================================================================

    void fillInPluginDescription (juce::PluginDescription& description) const override;

private:
    //==============================================================================
    static constexpr int kNumBandSlots = 4;
    static constexpr float kDefaultThresholdDB = -20.0f;
    static constexpr float kDefaultRatio = 4.0f;
    static constexpr float kDefaultAttackMs = 5.0f;
    static constexpr float kDefaultReleaseMs = 50.0f;

    /** Reads the chain_doc (env var -> %APPDATA% fallback) and seeds the
        parameter defaults from it; on any failure warns via CRASH_LOG_WARN and
        falls back to the identity configuration (decision A). */
    void resolveSpec();

    /** Seeds the 21 parameter defaults from a spec (band slots first
        min(4, bands.size()), compressor values only when hasCompression). */
    void applySpec (const ReplicaSpec& spec);

    /** Reads the live param values into a ReplicaSpec and configures the
        chain from it (compressor rule U6). Called on the audio thread only. */
    void applyParamConfig();

    /** chain_.prepare(rate) + applyParamConfig() + chain_.reset(). */
    void rebuildChain();

    //==============================================================================
    juce::String specFilePath;
    std::array<ReplicaParameter*, kNumBandSlots> bandUsed {};
    std::array<ReplicaParameter*, kNumBandSlots> bandFreq {};
    std::array<ReplicaParameter*, kNumBandSlots> bandGain {};
    std::array<ReplicaParameter*, kNumBandSlots> bandQ {};
    ReplicaParameter* thresholdParam = nullptr;
    ReplicaParameter* ratioParam = nullptr;
    ReplicaParameter* attackParam = nullptr;
    ReplicaParameter* releaseParam = nullptr;
    ReplicaParameter* makeupGainParam = nullptr;

    // Written by any setValue (any thread), consumed once per block at the
    // block start on the audio thread; also consumed by prepareToPlay.
    std::atomic<bool> configDirty { false };

    // Compressor rule U6: true when the spec had compression, OR when any of
    // the four compressor params was explicitly moved off its default.
    bool hasCompressionFromSpec = false;
    std::atomic<bool> compressionTouched { false };

    ReplicaChain chain_;
    double sampleRate = 48000.0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginLabReplica)
};
