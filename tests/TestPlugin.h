#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

//==============================================================================
/**
    A configurable fake AudioPluginInstance used by unit tests.

    This plugin is only ever compiled into the test target (unit_tests) and is
    never part of the main application.

    Features:
      - Stereo input/output buses (configured in the constructor)
      - setGain(double) applies a constant gain in processBlock (wet = dry * gain)
      - setLatencySamples(int) reports a configurable latency via getLatencySamples()
        and implements a real N-sample delay in processBlock
      - setBlockOnProcess(...) optionally blocks inside processBlock until the
        caller releases it (used by cancellation tests)

    Implements every pure virtual of AudioPluginInstance/AudioProcessor for the
    JUCE version currently fetched (JUCE 9, audio_processors_headless layout).
*/
class TestPlugin final : public juce::AudioPluginInstance
{
public:
    //==============================================================================
    /** A concrete HostedAudioProcessorParameter for getParameters() tests. */
    class TestParameter final : public juce::HostedAudioProcessorParameter
    {
    public:
        TestParameter (juce::String parameterIDToUse, juce::String parameterNameToUse, float defaultValueToUse)
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
    TestPlugin()
        : AudioPluginInstance (AudioProcessor::BusesProperties()
                                   .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                                   .withOutput ("Output", juce::AudioChannelSet::stereo(), true))
    {
        addHostedParameter (std::make_unique<TestParameter> ("gain", "Gain", 1.0f));
    }

    ~TestPlugin() override = default;

    //==============================================================================
    // Configuration
    //==============================================================================

    /** Sets the constant gain applied to every sample (default: 1.0). */
    void setGain (double newGain) noexcept              { gain = newGain; }

    /** Returns the configured gain. */
    double getGain() const noexcept                     { return gain; }

    /** Sets the plugin latency in samples (default: 0). */
    void setLatencySamples (int newLatency) noexcept    { juce::AudioProcessor::setLatencySamples (newLatency); }

    /** Installs an optional processBlock blocker for cancellation tests.

        When both pointers are non-null, processBlock sets *entered = true on
        entry and then busy-waits until *release becomes true.
    */
    void setBlockOnProcess (std::atomic<bool>* entered, std::atomic<bool>* release) noexcept
    {
        blockEntered = entered;
        blockRelease = release;
    }

    //==============================================================================
    // AudioProcessor
    //==============================================================================

    const juce::String getName() const override                       { return "TestPlugin"; }
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

    void prepareToPlay (double, int) override
    {
        delayLines.assign (static_cast<size_t> (juce::jmax (getTotalNumOutputChannels(), 1)), DelayLine {});
        for (auto& line : delayLines)
            line.reset (getLatencySamples());
    }

    void releaseResources() override
    {
        delayLines.clear();
    }

    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
    {
        if (blockEntered != nullptr)
            blockEntered->store (true);

        if (blockRelease != nullptr)
            while (!blockRelease->load()) { /* busy-wait until released */ }

        const auto numChannels = buffer.getNumChannels();
        for (int ch = 0; ch < numChannels; ++ch)
        {
            auto* data = buffer.getWritePointer (ch);
            const bool hasDelayLine = (ch < static_cast<int> (delayLines.size()));

            for (int i = 0; i < buffer.getNumSamples(); ++i)
            {
                float sample = data[i];
                if (hasDelayLine)
                    sample = delayLines[static_cast<size_t> (ch)].process (sample);
                data[i] = sample * static_cast<float> (gain);
            }
        }
    }

    void processBlock (juce::AudioBuffer<double>& buffer, juce::MidiBuffer& midiMessages) override
    {
        juce::AudioBuffer<float> floatBuffer (buffer.getNumChannels(), buffer.getNumSamples());
        for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
        {
            const auto* src = buffer.getReadPointer (ch);
            auto* dst = floatBuffer.getWritePointer (ch);
            for (int i = 0; i < buffer.getNumSamples(); ++i)
                dst[i] = static_cast<float> (src[i]);
        }

        processBlock (floatBuffer, midiMessages);

        for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
        {
            const auto* src = floatBuffer.getReadPointer (ch);
            auto* dst = buffer.getWritePointer (ch);
            for (int i = 0; i < buffer.getNumSamples(); ++i)
                dst[i] = static_cast<double> (src[i]);
        }
    }

    void getStateInformation (juce::MemoryBlock&) override            {}
    void setStateInformation (const void*, int) override               {}

    //==============================================================================
    // AudioPluginInstance
    //==============================================================================

    void fillInPluginDescription (juce::PluginDescription& description) const override
    {
        description.name = getName();
        description.pluginFormatName = "TestPlugin";
        description.version = "1.0.0";
        description.fileOrIdentifier = "TestPlugin";
        description.uniqueId = 0x54455354; // 'TEST'
        description.numInputChannels = 2;
        description.numOutputChannels = 2;
    }

private:
    //==============================================================================
    /** Simple fixed-length delay line used to realise setLatencySamples(). */
    struct DelayLine
    {
        std::vector<float> history;
        int writePos = 0;

        void reset (int numSamples)
        {
            history.assign (static_cast<size_t> (juce::jmax (numSamples, 0)), 0.0f);
            writePos = 0;
        }

        float process (float input)
        {
            if (history.empty())
                return input;

            const float delayed = history[static_cast<size_t> (writePos)];
            history[static_cast<size_t> (writePos)] = input;
            writePos = (writePos + 1) % static_cast<int> (history.size());
            return delayed;
        }
    };

    double gain = 1.0;
    std::atomic<bool>* blockEntered = nullptr;
    std::atomic<bool>* blockRelease = nullptr;
    std::vector<DelayLine> delayLines;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TestPlugin)
};
