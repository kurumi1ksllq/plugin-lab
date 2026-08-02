#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <algorithm>
#include <cmath>
#include <functional>

#include "../source/signal/FilePlayback.h"
#include "TestPlugin.h"
#include "../source/capture/SweepRunner.h"

//==============================================================================
// Helpers: generate test .wav files in the temp directory, read them back,
// and run a FilePlayback through a SweepRunner.

/** Writes a test .wav into the temp directory.
 *  Each channel is filled by sampleFn (channel, sampleIndex); 24-bit PCM.
 */
static juce::File writeTestWav (double sampleRate, int numChannels, int64_t numSamples,
                                const std::function<float (int ch, int64_t sample)>& sampleFn)
{
    const auto file = juce::File::getSpecialLocation (juce::File::tempDirectory)
                          .getChildFile ("fileplayback_test_" + juce::Uuid().toString() + ".wav");

    auto* stream = new juce::FileOutputStream (file);
    if (stream->failedToOpen())
    {
        delete stream;
        return {};
    }

    juce::WavAudioFormat wavFormat;
    auto writerOptions = juce::AudioFormatWriterOptions{}
                             .withSampleRate (sampleRate)
                             .withNumChannels (static_cast<int> (numChannels))
                             .withBitsPerSample (24);

    std::unique_ptr<juce::OutputStream> streamOwner (stream);
    std::unique_ptr<juce::AudioFormatWriter> writer (
        wavFormat.createWriterFor (streamOwner, writerOptions));

    if (writer == nullptr)
    {
        // Writer creation failed — the stream is still owned by the caller.
        return {};
    }

    constexpr int blockSize = 1024;
    juce::AudioBuffer<float> block (numChannels, blockSize);

    int64_t written = 0;
    while (written < numSamples)
    {
        const int n = static_cast<int> (std::min<int64_t> (blockSize, numSamples - written));
        block.clear();

        for (int ch = 0; ch < numChannels; ++ch)
            for (int s = 0; s < n; ++s)
                block.setSample (ch, s, sampleFn (ch, written + s));

        writer->writeFromAudioSampleBuffer (block, 0, n);
        written += n;
    }

    writer.reset();  // finalises the wav header
    return file;
}

/** Reads a .wav back into dest and reports its format. */
static bool readWavInto (const juce::File& file, juce::AudioBuffer<float>& dest,
                         double& sampleRate, int& numChannels)
{
    juce::AudioFormatManager formatManager;
    formatManager.registerBasicFormats();

    std::unique_ptr<juce::AudioFormatReader> reader (formatManager.createReaderFor (file));
    if (reader == nullptr)
        return false;

    sampleRate = reader->sampleRate;
    numChannels = static_cast<int> (reader->numChannels);
    dest.setSize (numChannels, static_cast<int> (reader->lengthInSamples));
    dest.clear();
    reader->read (&dest, 0, static_cast<int> (reader->lengthInSamples), 0, true, true);
    return true;
}

/** Generates the full FilePlayback signal into a 2-channel buffer in blocks. */
static void generateAll (FilePlayback& fp, double sampleRate, int blockSize,
                         juce::AudioBuffer<float>& buffer)
{
    (void) sampleRate;
    const auto totalLen = fp.getTotalLength();
    REQUIRE (totalLen > 0);

    buffer.setSize (2, static_cast<int> (totalLen));
    buffer.clear();

    int64_t pos = 0;
    while (pos < totalLen)
    {
        const int chunk = static_cast<int> (std::min<int64_t> (blockSize, totalLen - pos));
        fp.generate (buffer, static_cast<int> (pos), chunk);
        pos += chunk;
    }
}

//==============================================================================
TEST_CASE ("FilePlayback: wav helper writes a 1s 48k stereo file that reads back",
           "[fileplayback][wav-helper]")
{
    const double sr = 48000.0;
    const int64_t numSamples = static_cast<int64_t> (sr);  // 1 s

    const auto file = writeTestWav (sr, 2, numSamples,
                                    [] (int ch, int64_t s)
                                    {
                                        const double t = static_cast<double> (s) / 48000.0;
                                        const double freq = (ch == 0) ? 440.0 : 880.0;
                                        return static_cast<float> (0.5 * std::sin (2.0 * juce::MathConstants<double>::pi * freq * t));
                                    });
    REQUIRE (file.existsAsFile());

    juce::AudioBuffer<float> readBack;
    double readSr = 0.0;
    int readCh = 0;
    REQUIRE (readWavInto (file, readBack, readSr, readCh));

    REQUIRE (readSr == Catch::Approx (sr));
    REQUIRE (readCh == 2);
    REQUIRE (readBack.getNumSamples() == static_cast<int> (numSamples));

    // Spot-check a few samples on both channels against the source sine
    for (const int64_t s : { int64_t (0), int64_t (1000), int64_t (24000), int64_t (47999) })
    {
        for (int ch = 0; ch < 2; ++ch)
        {
            const double t = static_cast<double> (s) / 48000.0;
            const double freq = (ch == 0) ? 440.0 : 880.0;
            const float expected = static_cast<float> (0.5 * std::sin (2.0 * juce::MathConstants<double>::pi * freq * t));
            REQUIRE (readBack.getSample (ch, static_cast<int> (s)) == Catch::Approx (expected).margin (0.001f));
        }
    }
}

TEST_CASE ("FilePlayback: metadata after prepare of a 48k stereo file",
           "[fileplayback][metadata]")
{
    const double sr = 48000.0;
    const auto file = writeTestWav (sr, 2, static_cast<int64_t> (sr),
                                    [] (int ch, int64_t s)
                                    {
                                        return static_cast<float> (0.5 * std::sin (static_cast<double> (s) * 0.05 * (ch + 1)));
                                    });

    FilePlayback fp;
    fp.setFile (file);
    fp.prepare (48000.0, 512);

    REQUIRE (fp.isLoaded());
    REQUIRE (fp.getSourceSampleRate() == Catch::Approx (48000.0));
    REQUIRE (fp.getFileNumChannels() == 2);
    REQUIRE (fp.getTotalLength() == static_cast<int64_t> (sr));
    REQUIRE (fp.getDurationSec() == Catch::Approx (1.0));
    REQUIRE_FALSE (fp.getSourcePath().isEmpty());
    REQUIRE (fp.getResampleRatio() == 0.0);  // 1:1 — no resampling
}

TEST_CASE ("FilePlayback: 48k file through 48k session is a sample-accurate bypass",
           "[fileplayback][happy]")
{
    const double sr = 48000.0;
    const int64_t numSamples = static_cast<int64_t> (sr);  // 1 s

    const auto file = writeTestWav (sr, 2, numSamples,
                                    [] (int ch, int64_t s)
                                    {
                                        const double t = static_cast<double> (s) / 48000.0;
                                        const double freq = (ch == 0) ? 440.0 : 880.0;
                                        return static_cast<float> (0.5 * std::sin (2.0 * juce::MathConstants<double>::pi * freq * t));
                                    });

    juce::AudioBuffer<float> reference;
    double refSr = 0.0;
    int refCh = 0;
    REQUIRE (readWavInto (file, reference, refSr, refCh));

    // --- Part 1: direct generate() in blocks ===
    {
        FilePlayback fp;
        fp.setFile (file);
        fp.prepare (sr, 512);

        juce::AudioBuffer<float> out;
        generateAll (fp, sr, 512, out);

        REQUIRE (out.getNumSamples() == static_cast<int> (numSamples));
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < static_cast<int> (numSamples); ++i)
                REQUIRE (out.getSample (ch, i) == Catch::Approx (reference.getSample (ch, i)).margin (0.001f));
    }

    // --- Part 2: through SweepRunner + TestPlugin (gain 1.0, latency 0 → identity) ===
    {
        FilePlayback fp;
        fp.setFile (file);

        TestPlugin plugin;
        plugin.setGain (1.0);
        plugin.setLatencySamples (0);

        SweepRunner runner;
        runner.prepare (sr, 512);
        runner.setGenerator (&fp);
        runner.setPlugin (&plugin);

        REQUIRE (runner.run());

        const auto& wet = runner.getResult().getWetBuffer();
        REQUIRE (wet.getNumSamples() == static_cast<int> (numSamples));

        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < static_cast<int> (numSamples); ++i)
                REQUIRE (wet.getSample (ch, i) == Catch::Approx (reference.getSample (ch, i)).margin (0.001f));
    }
}

TEST_CASE ("FilePlayback: 44.1k file at 48k session reports resampled length and ratio",
           "[fileplayback][resample]")
{
    const auto file = writeTestWav (44100.0, 2, 44100,
                                    [] (int ch, int64_t s)
                                    {
                                        return static_cast<float> (0.5 * std::sin (static_cast<double> (s) * 0.05 * (ch + 1)));
                                    });

    FilePlayback fp;
    fp.setFile (file);
    fp.prepare (48000.0, 512);

    REQUIRE (fp.isLoaded());
    REQUIRE (fp.getSourceSampleRate() == Catch::Approx (44100.0));
    REQUIRE (fp.getTotalLength() == static_cast<int64_t> (std::llround (44100.0 * 48000.0 / 44100.0)));
    REQUIRE (fp.getResampleRatio() == Catch::Approx (44100.0 / 48000.0));
}

TEST_CASE ("FilePlayback: reset() after generating restarts with identical output",
           "[fileplayback][reset]")
{
    // Use a 44.1k file at a 48k session so the resampler flush path is exercised
    const auto file = writeTestWav (44100.0, 2, 44100,
                                    [] (int ch, int64_t s)
                                    {
                                        return static_cast<float> (0.5 * std::sin (static_cast<double> (s) * 0.05 * (ch + 1)));
                                    });

    FilePlayback fp;
    fp.setFile (file);
    fp.prepare (48000.0, 512);

    juce::AudioBuffer<float> first;
    generateAll (fp, 48000.0, 512, first);

    fp.reset();

    juce::AudioBuffer<float> second;
    generateAll (fp, 48000.0, 512, second);

    REQUIRE (first.getNumSamples() == second.getNumSamples());
    for (int ch = 0; ch < 2; ++ch)
        for (int i = 0; i < first.getNumSamples(); ++i)
            REQUIRE (first.getSample (ch, i) == Catch::Approx (second.getSample (ch, i)).margin (1e-6f));
}

TEST_CASE ("FilePlayback: mono file is duplicated to both output channels",
           "[fileplayback][mono-stereo]")
{
    const double sr = 48000.0;
    const int64_t numSamples = static_cast<int64_t> (sr);  // 1 s

    const auto file = writeTestWav (sr, 1, numSamples,
                                    [] (int, int64_t s)
                                    {
                                        return static_cast<float> (0.5 * std::sin (2.0 * juce::MathConstants<double>::pi * 440.0 * static_cast<double> (s) / 48000.0));
                                    });

    juce::AudioBuffer<float> reference;
    double refSr = 0.0;
    int refCh = 0;
    REQUIRE (readWavInto (file, reference, refSr, refCh));
    REQUIRE (refCh == 1);

    FilePlayback fp;
    fp.setFile (file);
    fp.prepare (sr, 512);

    juce::AudioBuffer<float> out;
    generateAll (fp, sr, 512, out);

    REQUIRE (out.getNumChannels() == 2);
    for (int i = 0; i < static_cast<int> (numSamples); ++i)
    {
        const float expected = reference.getSample (0, i);
        REQUIRE (out.getSample (0, i) == Catch::Approx (expected).margin (0.001f));
        REQUIRE (out.getSample (1, i) == Catch::Approx (expected).margin (0.001f));
    }
}

TEST_CASE ("FilePlayback: missing file reports not-loaded, zero length, and silence",
           "[fileplayback][error]")
{
    FilePlayback fp;
    fp.setFile (juce::File::getSpecialLocation (juce::File::tempDirectory)
                    .getChildFile ("fileplayback_missing_" + juce::Uuid().toString() + ".wav"));
    fp.prepare (48000.0, 512);

    REQUIRE_FALSE (fp.isLoaded());
    REQUIRE (fp.getTotalLength() == 0);
    REQUIRE (fp.getSourceSampleRate() == 0.0);
    REQUIRE (fp.getResampleRatio() == 0.0);
    REQUIRE (fp.getDurationSec() == 0.0);
    REQUIRE (fp.getFileNumChannels() == 0);
    REQUIRE (fp.getSourcePath().isEmpty());

    // generate() must fill silence and never throw
    juce::AudioBuffer<float> buffer (2, 512);
    buffer.clear();
    buffer.setSample (0, 0, 1.0f);  // dirty before the call to prove it is cleared
    buffer.setSample (1, 511, 1.0f);

    fp.generate (buffer, 0, 512);

    for (int ch = 0; ch < 2; ++ch)
        for (int i = 0; i < 512; ++i)
            REQUIRE (buffer.getSample (ch, i) == 0.0f);
}
