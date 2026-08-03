#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <cmath>

#include "../source/capture/AudioBuffer.h"

//==============================================================================
// Helpers: read a flushed .wav back through juce::WavAudioFormat (mirrors the
// read-back helper in FilePlaybackTests.cpp), and generate deterministic
// dry/wet test blocks.

static constexpr double kFlushTestSampleRate = 48000.0;

/** Expected dry sample value for the deterministic test signal: a sine whose
 *  frequency differs per channel (440 Hz / 550 Hz) at 0.8 amplitude.
 */
static float expectedDrySample (int ch, int64_t sampleIndex)
{
    const double t = static_cast<double> (sampleIndex) / kFlushTestSampleRate;
    const double freq = 440.0 * (1.0 + 0.25 * ch);
    return static_cast<float> (0.8 * std::sin (2.0 * juce::MathConstants<double>::pi * freq * t));
}

/** Fills a dry/wet block pair; wet = 0.5 * dry (a different constant gain so
 *  dry and wet channels are distinguishable in the flushed wav).
 */
static void fillTestBlock (juce::AudioBuffer<float>& dry, juce::AudioBuffer<float>& wet,
                           int64_t startSample)
{
    const int numSamples = dry.getNumSamples();

    for (int ch = 0; ch < dry.getNumChannels(); ++ch)
    {
        auto* dryPtr = dry.getWritePointer (ch);
        auto* wetPtr = wet.getWritePointer (ch);

        for (int s = 0; s < numSamples; ++s)
        {
            const float sample = expectedDrySample (ch, startSample + s);
            dryPtr[s] = sample;
            wetPtr[s] = 0.5f * sample;
        }
    }
}

/** Appends numBlocks blocks of blockSize samples with the deterministic signal. */
static void appendBlocks (CaptureBuffer& buffer, int numChannels, int blockSize, int numBlocks)
{
    juce::AudioBuffer<float> dry (numChannels, blockSize);
    juce::AudioBuffer<float> wet (numChannels, blockSize);

    for (int b = 0; b < numBlocks; ++b)
    {
        fillTestBlock (dry, wet, static_cast<int64_t> (b) * blockSize);
        buffer.append (dry, wet, blockSize);
    }
}

/** Asserts that readBack (channels [dry ch0..N-1, wet ch0..N-1]) matches the
 *  deterministic signal sample-accurately.
 */
static void requireWavMatchesSignal (const juce::AudioBuffer<float>& readBack,
                                     int numChannels, int64_t numSamples)
{
    REQUIRE (readBack.getNumSamples() == static_cast<int> (numSamples));

    for (int s = 0; s < static_cast<int> (numSamples); ++s)
    {
        for (int ch = 0; ch < numChannels; ++ch)
        {
            const float dryExpected = expectedDrySample (ch, s);
            REQUIRE (readBack.getSample (ch, s) == Catch::Approx (dryExpected).margin (0.001f));
            REQUIRE (readBack.getSample (numChannels + ch, s) == Catch::Approx (0.5f * dryExpected).margin (0.001f));
        }
    }
}

/** Reads a .wav back into dest and reports its format (mirrors FilePlaybackTests). */
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

/** Creates a unique temp wav path for a flush test. */
static juce::File makeFlushWavFile (const juce::String& tag)
{
    return juce::File::getSpecialLocation (juce::File::tempDirectory)
               .getChildFile ("capturebuffer_flush_" + tag + "_" + juce::Uuid().toString() + ".wav");
}

//==============================================================================
TEST_CASE ("CaptureBuffer: flush mirrors dry and wet into a readable 24-bit wav",
           "[capturebuffer][flush]")
{
    // Arrange
    constexpr int numChannels = 2;
    constexpr int blockSize = 2048;
    constexpr int numBlocks = 8;
    const int64_t totalSamples = static_cast<int64_t> (numBlocks) * blockSize;

    CaptureBuffer buffer;
    buffer.prepare (numChannels, blockSize);
    buffer.setSampleRate (kFlushTestSampleRate);

    const auto wavFile = makeFlushWavFile ("flush");
    buffer.setFlushConfig (wavFile, 0.05);   // flush every 2400 samples → mid-run flushes

    // Act
    appendBlocks (buffer, numChannels, blockSize, numBlocks);
    buffer.trim();

    // Assert
    REQUIRE (buffer.getNumRecordedSamples() == totalSamples);

    juce::AudioBuffer<float> readBack;
    double readSr = 0.0;
    int readCh = 0;
    REQUIRE (readWavInto (wavFile, readBack, readSr, readCh));

    REQUIRE (readSr == Catch::Approx (kFlushTestSampleRate));
    REQUIRE (readCh == 2 * numChannels);   // [dry ch0..N-1, wet ch0..N-1]
    requireWavMatchesSignal (readBack, numChannels, totalSamples);

    wavFile.deleteFile();
}

TEST_CASE ("CaptureBuffer: multiple mid-run flushes plus the trim tail capture every sample",
           "[capturebuffer][flush-interval]")
{
    // Arrange — 7 blocks of 500 + 1 of 300 = 3800 samples; flush interval of
    // 960 samples (0.02 s) triggers mid-run flushes at 1000, 2000 and 3000;
    // the remaining 800 samples must be flushed by trim().
    constexpr int numChannels = 2;
    constexpr int blockSize = 500;
    constexpr int numBlocks = 7;
    constexpr int tailBlockSize = 300;
    const int64_t totalSamples = static_cast<int64_t> (numBlocks) * blockSize + tailBlockSize;

    CaptureBuffer buffer;
    buffer.prepare (numChannels, blockSize);
    buffer.setSampleRate (kFlushTestSampleRate);

    const auto wavFile = makeFlushWavFile ("flush-interval");
    buffer.setFlushConfig (wavFile, 0.02);   // 48000 * 0.02 = 960 samples

    // Act
    appendBlocks (buffer, numChannels, blockSize, numBlocks);

    juce::AudioBuffer<float> dry (numChannels, tailBlockSize);
    juce::AudioBuffer<float> wet (numChannels, tailBlockSize);
    fillTestBlock (dry, wet, static_cast<int64_t> (numBlocks) * blockSize);
    buffer.append (dry, wet, tailBlockSize);

    buffer.trim();

    // Assert — all 3800 samples present, sample-accurate
    juce::AudioBuffer<float> readBack;
    double readSr = 0.0;
    int readCh = 0;
    REQUIRE (readWavInto (wavFile, readBack, readSr, readCh));

    REQUIRE (readCh == 2 * numChannels);
    requireWavMatchesSignal (readBack, numChannels, totalSamples);

    wavFile.deleteFile();
}

TEST_CASE ("CaptureBuffer: without flush config the in-memory contract is unchanged",
           "[capturebuffer][flush-disabled]")
{
    // Arrange
    constexpr int numChannels = 2;
    constexpr int blockSize = 1024;
    constexpr int numBlocks = 4;
    const int64_t totalSamples = static_cast<int64_t> (numBlocks) * blockSize;

    CaptureBuffer buffer;
    buffer.prepare (numChannels, blockSize);
    buffer.setFlushConfig (juce::File(), 0.05);   // empty path → flush disabled

    // Act
    appendBlocks (buffer, numChannels, blockSize, numBlocks);
    buffer.trim();

    // Assert — buffers stay complete and trimmed to the exact length
    REQUIRE (buffer.getNumRecordedSamples() == totalSamples);
    REQUIRE (buffer.getDryBuffer().getNumSamples() == static_cast<int> (totalSamples));
    REQUIRE (buffer.getWetBuffer().getNumSamples() == static_cast<int> (totalSamples));

    for (int s = 0; s < static_cast<int> (totalSamples); ++s)
    {
        for (int ch = 0; ch < numChannels; ++ch)
        {
            const float dryExpected = expectedDrySample (ch, s);
            REQUIRE (buffer.getDryBuffer().getSample (ch, s) == Catch::Approx (dryExpected).margin (0.001f));
            REQUIRE (buffer.getWetBuffer().getSample (ch, s) == Catch::Approx (0.5f * dryExpected).margin (0.001f));
        }
    }
}

TEST_CASE ("CaptureBuffer: clear() finalises the old run; a new run on the same path truncates",
           "[capturebuffer][flush-clear]")
{
    // Arrange
    constexpr int numChannels = 2;
    constexpr int blockSize = 500;

    CaptureBuffer buffer;
    buffer.prepare (numChannels, blockSize);
    buffer.setSampleRate (kFlushTestSampleRate);

    const auto wavFile = makeFlushWavFile ("flush-clear");
    buffer.setFlushConfig (wavFile, 0.05);   // 2400 samples → no mid-run flush below that

    // Act — run A: 4 blocks of 500 = 2000 samples, then clear()
    appendBlocks (buffer, numChannels, blockSize, 4);
    buffer.clear();   // must flush run A's tail and close the writer

    // Act — run B on the same path: 3 blocks of 500 = 1500 samples
    appendBlocks (buffer, numChannels, blockSize, 3);
    buffer.trim();

    // Assert — the file contains ONLY run B (re-opening must truncate)
    juce::AudioBuffer<float> readBack;
    double readSr = 0.0;
    int readCh = 0;
    REQUIRE (readWavInto (wavFile, readBack, readSr, readCh));

    REQUIRE (readCh == 2 * numChannels);
    REQUIRE (readBack.getNumSamples() == 3 * blockSize);
    requireWavMatchesSignal (readBack, numChannels, static_cast<int64_t> (3 * blockSize));

    wavFile.deleteFile();
}

TEST_CASE ("CaptureBuffer: file is valid mid-capture even without trim() (crash simulation)",
           "[capturebuffer][flush-no-trim]")
{
    // Arrange
    constexpr int numChannels = 2;
    constexpr int blockSize = 1000;
    constexpr int numBlocks = 3;
    const int64_t totalSamples = static_cast<int64_t> (numBlocks) * blockSize;

    const auto wavFile = makeFlushWavFile ("flush-no-trim");

    // Act — capture in an inner scope and never call trim(); the buffer is
    // destroyed (stream closed) exactly like a process death after a crash.
    juce::AudioBuffer<float> readBack;
    double readSr = 0.0;
    int readCh = 0;

    {
        CaptureBuffer buffer;
        buffer.prepare (numChannels, blockSize);
        buffer.setSampleRate (kFlushTestSampleRate);
        buffer.setFlushConfig (wavFile, 0.0);   // interval 0 → flush on every append

        appendBlocks (buffer, numChannels, blockSize, numBlocks);

        // Assert — already readable mid-capture (header patched at each flush)
        REQUIRE (readWavInto (wavFile, readBack, readSr, readCh));
        REQUIRE (readCh == 2 * numChannels);
        requireWavMatchesSignal (readBack, numChannels, totalSamples);
    }

    // Assert — still readable after the writer is gone
    REQUIRE (readSr == Catch::Approx (kFlushTestSampleRate));
    REQUIRE (readWavInto (wavFile, readBack, readSr, readCh));
    requireWavMatchesSignal (readBack, numChannels, totalSamples);

    wavFile.deleteFile();
}

TEST_CASE ("CaptureBuffer: full-scale sine survives the 24-bit PCM conversion",
           "[capturebuffer][flush-24bit-conversion]")
{
    // Arrange — a 1 kHz sine at unity amplitude; sample 12 is exactly
    // sin (pi/2) = 1.0, so the peak exercises the 24-bit conversion limit
    // (8388607/8388608 ≈ 0.99999988 after the round trip).
    constexpr int numChannels = 1;
    constexpr int numSamples = 4800;   // 0.1 s at 48 kHz = 100 cycles of 1 kHz

    CaptureBuffer buffer;
    buffer.prepare (numChannels, 512);
    buffer.setSampleRate (kFlushTestSampleRate);

    const auto wavFile = makeFlushWavFile ("flush-24bit");
    buffer.setFlushConfig (wavFile, 0.05);

    juce::AudioBuffer<float> dry (numChannels, numSamples);
    juce::AudioBuffer<float> wet (numChannels, numSamples);
    for (int ch = 0; ch < numChannels; ++ch)
    {
        auto* dryPtr = dry.getWritePointer (ch);
        auto* wetPtr = wet.getWritePointer (ch);

        for (int s = 0; s < numSamples; ++s)
        {
            const double t = static_cast<double> (s) / kFlushTestSampleRate;
            const float sample = static_cast<float> (std::sin (2.0 * juce::MathConstants<double>::pi * 1000.0 * t));
            dryPtr[s] = sample;
            wetPtr[s] = sample;
        }
    }

    // Act
    buffer.append (dry, wet, numSamples);
    buffer.trim();

    // Assert
    juce::AudioBuffer<float> readBack;
    double readSr = 0.0;
    int readCh = 0;
    REQUIRE (readWavInto (wavFile, readBack, readSr, readCh));

    REQUIRE (readBack.getNumSamples() == numSamples);

    float peak = 0.0f;
    for (int s = 0; s < numSamples; ++s)
        peak = juce::jmax (peak, std::fabs (readBack.getSample (0, s)));

    REQUIRE (peak == Catch::Approx (0.99999).margin (0.0001f));

    wavFile.deleteFile();
}
