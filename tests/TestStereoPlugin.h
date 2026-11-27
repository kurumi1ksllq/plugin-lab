#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <atomic>
#include <vector>

//==============================================================================
/**
    A deterministic stereo (MidSide) test plugin used by the stereo_width
    measurement tests (issue #104 / ADR-0003).

    Only ever compiled into the test target (unit_tests) - never part of the
    main application. It applies one of three configurable, exactly-known
    stereo behaviours to every sample pair, so a test can state the expected
    MidSide decode result in closed form:

      sideAttenuation  - scale the S component (width change): S *= sideGain.
                         sideGain 0 = pure mono (S removed), sideGain 1 =
                         untouched, sideGain 2 = width doubled.
      lrGainSkew       - scale L and R by different linear gains
                         (L *= leftGain, R *= rightGain), i.e. a level
                         imbalance that leaks mid into side and back.
      polarityFlip     - flip the right channel polarity (R = -R), i.e. a
                         pure side/mid swap that inverts the correlation.

    The three are independent flags; all default to "do nothing", so a
    default-constructed TestStereoPlugin is a bit-transparent passthrough.
*/
class TestStereoPlugin final : public juce::AudioPluginInstance
{
public:
    /** Which deterministic behaviour the plugin applies. */
    enum class Behaviour
    {
        passthrough,      // L/R unchanged
        sideAttenuation,  // S scaled by setSideGain()
        lrGainSkew,       // L/R scaled by setLeftGain()/setRightGain()
        polarityFlip      // R inverted
    };

    TestStereoPlugin()
        : AudioPluginInstance (AudioProcessor::BusesProperties()
                                   .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                                   .withOutput ("Output", juce::AudioChannelSet::stereo(), true))
    {
    }

    ~TestStereoPlugin() override = default;
    //==============================================================================
    // Configuration
    //==============================================================================

    /** Select the behaviour applied to every processed sample pair. */
    void setBehaviour (Behaviour b) noexcept { behaviour = b; }
    Behaviour getBehaviour() const noexcept  { return behaviour; }

    /** Side gain for Behaviour::sideAttenuation (S *= gain). Default 1.0. */
    void setSideGain (double g) noexcept  { sideGain = g; }
    double getSideGain() const noexcept   { return sideGain; }

    /** Per-channel linear gains for Behaviour::lrGainSkew. Default 1.0 each. */
    void setLeftGain (double g) noexcept   { leftGain = g; }
    void setRightGain (double g) noexcept  { rightGain = g; }
    double getLeftGain() const noexcept    { return leftGain; }
    double getRightGain() const noexcept   { return rightGain; }

    //==============================================================================
    // AudioProcessor
    //==============================================================================

    const juce::String getName() const override                { return "TestStereoPlugin"; }
    double getTailLengthSeconds() const override               { return 0.0; }
    bool acceptsMidi() const override                          { return false; }
    bool producesMidi() const override                         { return false; }
    juce::AudioProcessorEditor* createEditor() override        { return nullptr; }
    bool hasEditor() const override                            { return false; }

    int getNumPrograms() override                              { return 0; }
    int getCurrentProgram() override                           { return 0; }
    void setCurrentProgram (int) override                      {}
    const juce::String getProgramName (int) override           { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void prepareToPlay (double, int) override {}
    void releaseResources() override {}

    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
    {
        const int numChannels = buffer.getNumChannels();
        const int numSamples  = buffer.getNumSamples();

        if (numChannels < 2)
        {
            // Not a stereo pair: nothing the stereo behaviours can act on.
            if (behaviour == Behaviour::lrGainSkew && numChannels == 1)
                buffer.applyGain (0, numSamples, static_cast<float> (leftGain));

            return;
        }

        auto* left  = buffer.getWritePointer (0);
        auto* right = buffer.getWritePointer (1);

        for (int i = 0; i < numSamples; ++i)
        {
            const double l = static_cast<double> (left[i]);
            const double r = static_cast<double> (right[i]);

            double outL = l;
            double outR = r;

            switch (behaviour)
            {
                case Behaviour::passthrough:
                    break;

                case Behaviour::sideAttenuation:
                {
                    // MidSide decode, scale S, re-encode. L = M + S, R = M - S.
                    const double mid  = 0.5 * (l + r);
                    const double side = 0.5 * (l - r) * sideGain;
                    outL = mid + side;
                    outR = mid - side;
                    break;
                }

                case Behaviour::lrGainSkew:
                    outL = l * leftGain;
                    outR = r * rightGain;
                    break;

                case Behaviour::polarityFlip:
                    outR = -r;
                    break;
            }

            left[i]  = static_cast<float> (outL);
            right[i] = static_cast<float> (outR);
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

    void getStateInformation (juce::MemoryBlock&) override  {}
    void setStateInformation (const void*, int) override    {}

    //==============================================================================
    // AudioPluginInstance
    //==============================================================================

    void fillInPluginDescription (juce::PluginDescription& description) const override
    {
        description.name = getName();
        description.pluginFormatName = "TestStereoPlugin";
        description.version = "1.0.0";
        description.fileOrIdentifier = "TestStereoPlugin";
        description.uniqueId = 0x54535445; // 'TSTE'
        description.numInputChannels = 2;
        description.numOutputChannels = 2;
    }

private:
    Behaviour behaviour = Behaviour::passthrough;
    double sideGain = 1.0;
    double leftGain = 1.0;
    double rightGain = 1.0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TestStereoPlugin)
};
