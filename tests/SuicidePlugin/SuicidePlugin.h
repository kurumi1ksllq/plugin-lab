#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <atomic>
#include <cstdlib>

#if JUCE_WINDOWS
#include <windows.h>
#endif

//==============================================================================
/**
    A crash-injection VST3 fixture for out-of-process hosting tests
    (block D, ADR-D-2 / ticket D3b).

    The crash trigger is deliberately exposed as a normal hosted parameter
    ("crash_mode") instead of C++ setters: the out-of-process host loads this
    plugin through VST3PluginFormat, so the child process can only arm a crash
    through the parameter interface (VST3 host-side setValueNotifyingHost /
    parameter automation). No C++ setter survives the DLL boundary.

    crash_mode (normalized 0..1, default 0):
      0 -> normal passthrough (dry == wet, zero latency, zero tail)
      1 -> ExitProcess (1) on the first processBlock executed while armed
      2 -> abort () on the first processBlock executed while armed

    ExitProcess/abort bypass the C++ exception machinery — catch (...) under
    /EHa cannot intercept them — which is exactly the host-killer behaviour
    this fixture exists to simulate (Pianoteq-style plugins). The process
    dies on the first block where a crash mode is armed; the parent host must
    therefore run this plugin in a child process (block D design decision).

    This plugin is only built as its own VST3 target (tests/CMakeLists.txt,
    BUILD_TESTS only) and is never part of the main application or unit_tests.
*/
class SuicidePlugin final : public juce::AudioPluginInstance
{
public:
    //==============================================================================
    /** A concrete HostedAudioProcessorParameter for getParameters() tests.

        The normalized value 0..1 is mapped to the crash mode on every
        setValue: 0 -> passthrough, 1 -> ExitProcess, 2 -> abort.
    */
    class CrashModeParameter final : public juce::HostedAudioProcessorParameter
    {
    public:
        CrashModeParameter (juce::String parameterIDToUse, juce::String parameterNameToUse,
                            std::atomic<int>& modeToUpdate)
            : parameterID (std::move (parameterIDToUse)),
              parameterName (std::move (parameterNameToUse)),
              mode (modeToUpdate)
        {
        }

        juce::String getParameterID() const override               { return parameterID; }
        float getValue() const override                            { return value; }
        void setValue (float newValue) override
        {
            value = juce::jlimit (0.0f, 1.0f, newValue);
            mode.store (juce::roundToInt (value * 2.0f));
        }
        float getDefaultValue() const override                     { return 0.0f; }
        juce::String getName (int) const override                  { return parameterName; }
        juce::String getLabel() const override                     { return {}; }
        float getValueForText (const juce::String&) const override { return value; }

    private:
        juce::String parameterID;
        juce::String parameterName;
        float value = 0.0f;
        std::atomic<int>& mode;
    };

    //==============================================================================
    SuicidePlugin()
        : AudioPluginInstance (AudioProcessor::BusesProperties()
                                   .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                                   .withOutput ("Output", juce::AudioChannelSet::stereo(), true))
    {
        // "crash_mode": normalized 0..1 -> mode 0 (passthrough) / 1 (ExitProcess)
        // / 2 (abort). Default 0 keeps the plugin harmless until explicitly armed.
        addHostedParameter (std::make_unique<CrashModeParameter> ("crash_mode", "Crash Mode", crashMode));
    }

    ~SuicidePlugin() override = default;

    //==============================================================================
    // AudioProcessor
    //==============================================================================

    const juce::String getName() const override                       { return "SuicidePlugin"; }
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
        // Passthrough: nothing to prepare.
    }

    void releaseResources() override
    {
    }

    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages) override
    {
        juce::ignoreUnused (midiMessages);

        // Crash modes terminate the process on the first block in which they
        // are armed. ExitProcess/abort are immune to catch (...) (/EHa) — the
        // whole point of this fixture (ADR-D-2). The mode is re-read every
        // block, so the child can arm the crash at any time via the parameter.
        const int mode = crashMode.load();

        if (mode == 1)
        {
           #if JUCE_WINDOWS
            ExitProcess (1);
           #else
            std::abort();
           #endif
        }

        if (mode == 2)
            std::abort();

        // Normal mode: transparent passthrough — dry == wet, no processing.
        juce::ignoreUnused (buffer);
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
        description.pluginFormatName = "SuicidePlugin";
        description.version = "1.0.0";
        description.fileOrIdentifier = "SuicidePlugin";
        description.uniqueId = 0x53554944; // 'SUID'
        description.numInputChannels = 2;
        description.numOutputChannels = 2;
    }

private:
    // Cache of the armed crash mode, written by the parameter (any thread) and
    // read by processBlock (audio thread).
    std::atomic<int> crashMode = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SuicidePlugin)
};
