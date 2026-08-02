#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <cmath>
#include <limits>

//==============================================================================
/**
    A deterministic test compressor used as ground truth for dynamic
    compression measurements.

    This plugin is only ever compiled into the test target (unit_tests) and is
    never part of the main application.

    Signal model (feed-forward, sample-by-sample, linked stereo):
      - Detector: the maximum instantaneous magnitude across channels,
            levelDB = 20 * log10 (level),  level = max_ch |sample_ch|
      - Gain computer (static compression curve):
            grTarget = (levelDB > thresholdDB) ? (1 - 1/ratio) * (levelDB - thresholdDB) : 0
      - Gain smoothing (single-pole, direction-dependent time constant,
        updated exactly once per sample):
            tauDir   = (grTarget > grSmoothed) ? attackSec : releaseSec
            grSmoothed += (1 - exp (-1 / (sr * tauDir))) * (grTarget - grSmoothed)
      - Output gain:
            output = input * dBToGain (-grSmoothed) * dBToGain (makeupGainDB)

    grSmoothed holds the gain reduction as a positive dB amount, so the output
    gain factor for it is dBToGain (-grSmoothed).

    Because grSmoothed is a true single-pole filter with a fixed time
    constant, a step in input level produces exactly measurable exponential
    attack/release curves:
      attack : GR(t) = GR_ss * (1 - e^(-t / tau_attack))
      release: GR(t) = GR_ss * e^(-t / tau_release)

    Stereo in/out, zero latency, zero tail. All DSP is double precision.

    Implements every pure virtual of AudioPluginInstance/AudioProcessor for the
    JUCE version currently fetched (JUCE 9, audio_processors_headless layout).
*/
class TestCompressorPlugin final : public juce::AudioPluginInstance
{
public:
    //==============================================================================
    /** A concrete HostedAudioProcessorParameter for getParameters() tests. */
    class CompressorParameter final : public juce::HostedAudioProcessorParameter
    {
    public:
        CompressorParameter (juce::String parameterIDToUse, juce::String parameterNameToUse, float defaultValueToUse)
            : parameterID (std::move (parameterIDToUse)),
              parameterName (std::move (parameterNameToUse)),
              defaultValue (defaultValueToUse)
        {
        }

        juce::String getParameterID() const override                 { return parameterID; }
        float getValue() const override                              { return value; }
        void setValue (float newValue) override                      { value = newValue; }
        float getDefaultValue() const override                       { return defaultValue; }
        juce::String getName (int) const override                    { return parameterName; }
        juce::String getLabel() const override                       { return {}; }
        float getValueForText (const juce::String&) const override   { return value; }

    private:
        juce::String parameterID;
        juce::String parameterName;
        float defaultValue = 0.0f;
        float value = 0.0f;
    };

    //==============================================================================
    TestCompressorPlugin()
        : AudioPluginInstance (AudioProcessor::BusesProperties()
                                   .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                                   .withOutput ("Output", juce::AudioChannelSet::stereo(), true))
    {
        // Display-only placeholder so the AudioPluginInstance parameter
        // contract is satisfied; the DSP is configured via the setters below.
        addHostedParameter (std::make_unique<CompressorParameter> ("threshold", "Threshold", -20.0f));
    }

    ~TestCompressorPlugin() override = default;

    //==============================================================================
    // Configuration (must be called before prepareToPlay)
    //==============================================================================

    /** Sets the compression threshold in dB (default: -20). */
    void setThresholdDB (double db) noexcept        { thresholdDB = db; }

    /** Sets the compression ratio, > 1 (default: 4). */
    void setRatio (double r) noexcept               { ratio = r; }

    /** Sets the attack time constant in seconds, a true tau (default: 0.005). */
    void setAttackSec (double sec) noexcept         { attackSec = sec; }

    /** Sets the release time constant in seconds, a true tau (default: 0.050). */
    void setReleaseSec (double sec) noexcept        { releaseSec = sec; }

    /** Sets a constant make-up gain in dB applied after the gain reduction (default: 0). */
    void setMakeupGainDB (double db) noexcept       { makeupGainDB = db; }

    //==============================================================================
    /** Returns the smoothed gain reduction in dB at the end of the last processed block. */
    double getCurrentGRDB() const noexcept          { return grSmoothed; }

    //==============================================================================
    // AudioProcessor
    //==============================================================================

    const juce::String getName() const override                       { return "TestCompressorPlugin"; }
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

    void prepareToPlay (double newSampleRate, int) override
    {
        sampleRate = newSampleRate;
    }

    void releaseResources() override
    {
    }

    void reset() override
    {
        grSmoothed = 0.0;
    }

    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
    {
        processSamples<float> (buffer);
    }

    void processBlock (juce::AudioBuffer<double>& buffer, juce::MidiBuffer&) override
    {
        processSamples<double> (buffer);
    }

    void getStateInformation (juce::MemoryBlock&) override            {}
    void setStateInformation (const void*, int) override               {}

    //==============================================================================
    // AudioPluginInstance
    //==============================================================================

    void fillInPluginDescription (juce::PluginDescription& description) const override
    {
        description.name = getName();
        description.pluginFormatName = "TestCompressorPlugin";
        description.version = "1.0.0";
        description.fileOrIdentifier = "TestCompressorPlugin";
        description.uniqueId = 0x54434D50; // 'TCMP'
        description.numInputChannels = 2;
        description.numOutputChannels = 2;
    }

private:
    //==============================================================================
    static double dBToGain (double db)
    {
        return std::pow (10.0, db / 20.0);
    }

    template <typename FloatType>
    void processSamples (juce::AudioBuffer<FloatType>& buffer)
    {
        const double sr = std::max (sampleRate, 1.0);
        const int numChannels = buffer.getNumChannels();
        const int numSamples = buffer.getNumSamples();

        for (int i = 0; i < numSamples; ++i)
        {
            // Linked stereo detector: the maximum magnitude across channels.
            double level = 0.0;
            for (int ch = 0; ch < numChannels; ++ch)
                level = std::max (level, std::abs (static_cast<double> (buffer.getSample (ch, i))));

            // Static compression curve (instantaneous detector).
            const double levelDB = (level > 0.0)
                                       ? 20.0 * std::log10 (level)
                                       : -std::numeric_limits<double>::infinity();
            const double grTarget = (levelDB > thresholdDB)
                                        ? (1.0 - 1.0 / ratio) * (levelDB - thresholdDB)
                                        : 0.0;

            // Single-pole smoothing with direction-dependent time constant,
            // updated exactly once per sample so tau is measurable.
            const double tau = (grTarget > grSmoothed) ? attackSec : releaseSec;
            grSmoothed += (1.0 - std::exp (-1.0 / (sr * tau))) * (grTarget - grSmoothed);

            const double gain = dBToGain (-grSmoothed) * dBToGain (makeupGainDB);
            for (int ch = 0; ch < numChannels; ++ch)
            {
                const double input = static_cast<double> (buffer.getSample (ch, i));
                buffer.setSample (ch, i, static_cast<FloatType> (input * gain));
            }
        }
    }

    double sampleRate = 0.0;
    double thresholdDB = -20.0;
    double ratio = 4.0;
    double attackSec = 0.005;
    double releaseSec = 0.050;
    double makeupGainDB = 0.0;
    double grSmoothed = 0.0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TestCompressorPlugin)
};
