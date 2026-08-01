#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "TestPlugin.h"

//==============================================================================
// Smoke tests: verify the TestPlugin fake behaves as documented so that all
// later unit tests can rely on it.

TEST_CASE ("TestPlugin applies a configurable gain", "[testplugin][smoke]")
{
    TestPlugin plugin;
    plugin.setGain (0.5);
    plugin.prepareToPlay (44100.0, 512);

    juce::AudioBuffer<float> buffer (2, 256);
    buffer.clear();
    for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
        for (int s = 0; s < buffer.getNumSamples(); ++s)
            buffer.setSample (ch, s, 0.25f * static_cast<float> (ch + 1));

    juce::MidiBuffer midi;
    plugin.processBlock (buffer, midi);

    for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
        for (int s = 0; s < buffer.getNumSamples(); ++s)
            REQUIRE (buffer.getSample (ch, s) == Catch::Approx (0.125f * static_cast<float> (ch + 1)));
}

TEST_CASE ("TestPlugin reports the configured latency", "[testplugin][smoke]")
{
    TestPlugin plugin;
    plugin.setLatencySamples (64);

    REQUIRE (plugin.getLatencySamples() == 64);
}

TEST_CASE ("TestPlugin exposes a dummy parameter", "[testplugin][smoke]")
{
    TestPlugin plugin;

    REQUIRE (plugin.getParameters().size() == 1);
    REQUIRE (plugin.getParameters()[0]->getName (32) == juce::String ("Gain"));
}
