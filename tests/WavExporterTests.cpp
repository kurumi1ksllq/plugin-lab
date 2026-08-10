// tests/WavExporterTests.cpp
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include "../source/analysis/WavExporter.h"
#include <juce_audio_formats/juce_audio_formats.h>

// Helper: build a deterministic stereo dry buffer + wet = 2 × dry.
static juce::AudioBuffer<float> makeSignal (int numSamples, double gain)
{
    juce::AudioBuffer<float> buf (2, numSamples);
    for (int ch = 0; ch < 2; ++ch)
        for (int i = 0; i < numSamples; ++i)
            buf.setSample (ch, i, (float) (gain * std::sin (2.0 * juce::MathConstants<double>::pi * 440.0 * i / 48000.0)));
    return buf;
}

TEST_CASE ("WavExporter: exported file round-trips dry/wet/bypass exactly",
           "[wavexporter]")
{
    // Arrange — 1 s stereo, wet = 2× dry
    const auto dry = makeSignal (48000, 0.5);
    juce::AudioBuffer<float> wet (2, 48000);
    for (int ch = 0; ch < 2; ++ch)
        for (int i = 0; i < 48000; ++i)
            wet.setSample (ch, i, dry.getSample (ch, i) * 2.0f);

    const auto tmp = juce::File::getSpecialLocation (
        juce::File::SpecialLocationType::tempDirectory)
        .getNonexistentChildFile ("pluginlab_wav_export_test_", ".wav");

    // Act
    REQUIRE (WavExporter::exportTracks (dry, wet, 48000.0, tmp));

    // Assert — 6 channels, 24-bit, correct layout
    juce::AudioFormatManager mgr;
    mgr.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader (mgr.createReaderFor (tmp));
    REQUIRE (reader != nullptr);
    REQUIRE (reader->numChannels == 6);
    REQUIRE (reader->sampleRate == 48000.0);
    REQUIRE (reader->lengthInSamples == 48000);
    REQUIRE (reader->bitsPerSample == 24);

    juce::AudioBuffer<float> readBack (6, 48000);
    reader->read (&readBack, 0, 48000, 0, true, true);

    // dry L/R = 0/1, wet L/R = 2/3, bypass L/R = 4/5
    for (int i = 0; i < 48000; i += 1000)
    {
        REQUIRE (readBack.getSample (0, i) == Catch::Approx (dry.getSample (0, i)).margin (2e-4));
        REQUIRE (readBack.getSample (1, i) == Catch::Approx (dry.getSample (1, i)).margin (2e-4));
        REQUIRE (readBack.getSample (2, i) == Catch::Approx (wet.getSample (0, i)).margin (4e-4));
        REQUIRE (readBack.getSample (3, i) == Catch::Approx (wet.getSample (1, i)).margin (4e-4));
        REQUIRE (readBack.getSample (4, i) == Catch::Approx (dry.getSample (0, i)).margin (2e-4));
        REQUIRE (readBack.getSample (5, i) == Catch::Approx (dry.getSample (1, i)).margin (2e-4));
    }

    // Cleanup
    tmp.deleteFile();
}
