#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <cmath>
#include <limits>

//==============================================================================
/**
    A deterministic saturator used as ground truth for multi-level
    non-linearity measurements (issue #102, nonlinear_family).

    This plugin is only ever compiled into the test target (unit_tests) and is
    never part of the main application.

    Signal model (memoryless, sample-by-sample, linked stereo):
        y = tanh (drive * x) / tanh (drive)

    The 1/tanh(drive) normalisation keeps the full-scale output at exactly 1.0,
    so the transfer curve is the plain soft-clip shape scaled to unity gain at
    full scale. It is monotonic, odd, and its deviation from linearity grows
    with |x| — therefore THD rises monotonically with input level, which is the
    ground truth the nonlinear_family grid is verified against:

        level -> -inf : THD -> 0 (linear region)
        level = 0 dBFS: THD maximal for the configured drive

    A hard-clip variant is available (setHardClip) for checking that the grid
    separates hard clipping (THD plateaus abruptly) from soft saturation (THD
    grows smoothly): with hardClip the curve becomes
        y = clamp (drive * x, -1, +1) / clamp (drive, -1, +1)
    i.e. a brick-wall limiter at 1/drive.

    Zero latency, zero tail, no internal state (reset() is a no-op kept for the
    AudioProcessor contract). All DSP is double precision.

    Implements every pure virtual of AudioPluginInstance/AudioProcessor for the
    JUCE version currently fetched (JUCE 9, audio_processors_headless layout).
*/
class TestSaturatorPlugin final : public juce::AudioPluginInstance
{
public:
    //==============================================================================
    /** A concrete HostedAudioProcessorParameter for getParameters() tests. */
    class SaturatorParameter final : public juce::HostedAudioProcessorParameter
    {
    public:
        SaturatorParameter (juce::String parameterIDToUse, juce::String parameterNameToUse, float defaultValueToUse)
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
    TestSaturatorPlugin()
        : AudioPluginInstance (AudioProcessor::BusesProperties()
                                   .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                                   .withOutput ("Output", juce::AudioChannelSet::stereo(), true))
    {
        // Display-only placeholder so the AudioPluginInstance parameter
        // contract is satisfied; the DSP is configured via the setters below.
        addHostedParameter (std::make_unique<SaturatorParameter> ("drive", "Drive", 2.0f));
    }

    ~TestSaturatorPlugin() override = default;

    //==============================================================================
    // Configuration (must be called before prepareToPlay)
    //==============================================================================

    /** Sets the drive (pre-gain into the non-linearity), > 0 (default: 2). */
    void setDrive (double d) noexcept           { drive = d; }

    /** Selects the hard-clip curve instead of tanh (default: false = tanh). */
    void setHardClip (bool shouldHardClip) noexcept { hardClip = shouldHardClip; }

    //==============================================================================
    // AudioProcessor
    //==============================================================================

    const juce::String getName() const override                       { return "TestSaturatorPlugin"; }
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
        // Memoryless: nothing to clear.
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
        description.pluginFormatName = "TestSaturatorPlugin";
        description.version = "1.0.0";
        description.fileOrIdentifier = "TestSaturatorPlugin";
        description.uniqueId = 0x54534154; // 'TSAT'
        description.numInputChannels = 2;
        description.numOutputChannels = 2;
    }

private:
    //==============================================================================
    template <typename FloatType>
    void processSamples (juce::AudioBuffer<FloatType>& buffer)
    {
        const int numChannels = buffer.getNumChannels();
        const int numSamples = buffer.getNumSamples();

        // Normalisation constants, computed once per block.
        const double tanhDrive = std::tanh (drive);
        const double clipDrive = std::min (drive, 1.0);

        for (int i = 0; i < numSamples; ++i)
        {
            for (int ch = 0; ch < numChannels; ++ch)
            {
                const double in = static_cast<double> (buffer.getSample (ch, i));
                double out;

                if (hardClip)
                {
                    out = (clipDrive > 0.0)
                              ? std::min (std::max (drive * in, -1.0), 1.0) / clipDrive
                              : in;
                }
                else
                {
                    out = (tanhDrive > 0.0) ? std::tanh (drive * in) / tanhDrive : in;
                }

                buffer.setSample (ch, i, static_cast<FloatType> (out));
            }
        }
    }

    double sampleRate = 0.0;
    double drive = 2.0;
    bool hardClip = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TestSaturatorPlugin)
};
