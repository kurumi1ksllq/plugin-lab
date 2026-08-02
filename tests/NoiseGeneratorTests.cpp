#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "../source/signal/NoiseGenerator.h"
#include "../source/utils/FftHelper.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <utility>
#include <vector>

//==============================================================================
// Helpers

// Generate 'numSamples' samples of noise into a mono buffer using a freshly
// prepared generator, then return the samples as a plain vector.
static std::vector<float> generateNoiseSamples (NoiseGenerator& ng,
                                                double sampleRate,
                                                int numSamples,
                                                int blockSize = 512)
{
    ng.prepare (sampleRate, blockSize);

    juce::AudioBuffer<float> buffer (1, numSamples);
    buffer.clear();

    int64_t pos = 0;
    while (pos < numSamples)
    {
        const int chunk = static_cast<int> (std::min<int64_t> (blockSize, numSamples - pos));
        ng.generate (buffer, static_cast<int> (pos), chunk);
        pos += chunk;
    }

    const float* data = buffer.getReadPointer (0);
    return std::vector<float> (data, data + buffer.getNumSamples());
}

// Average power (linear) within an octave band [lowHz, highHz), averaged over
// all contiguous Hann-windowed FFT frames of the buffer; returned in dB.
static double bandPowerDb (const std::vector<float>& samples,
                           double sampleRate,
                           double lowHz,
                           double highHz)
{
    constexpr int order = 12;         // 4096-point FFT
    const int fftSize = 1 << order;
    const int numFrames = static_cast<int> (samples.size()) / fftSize;
    REQUIRE (numFrames >= 1);

    FftHelper fft (order);
    const int numBins = fftSize / 2 + 1;
    std::vector<float> real (numBins), imag (numBins), mag (numBins);

    const double binWidth = sampleRate / fftSize;
    const int kStart = static_cast<int> (std::ceil (lowHz / binWidth));
    const int kEnd   = static_cast<int> (std::floor (highHz / binWidth));  // exclusive

    double power = 0.0;
    for (int frame = 0; frame < numFrames; ++frame)
    {
        fft.forwardReal (samples.data() + frame * fftSize, real.data(), imag.data(), true);
        FftHelper::getMagnitudes (mag.data(), real.data(), imag.data(), numBins);
        for (int k = kStart; k < kEnd && k < numBins; ++k)
            power += static_cast<double> (mag[k]) * mag[k];
    }
    // Average power per bin: bands contain different bin counts
    // (each octave band doubles in width), so normalise by band width.
    power /= (kEnd - kStart) > 0 ? (kEnd - kStart) : 1;
    power /= numFrames;

    REQUIRE (power > 0.0);
    return 10.0 * std::log10 (power);
}

// Linear-regression slope of band power (dB) vs log2(band centre), i.e. the
// spectrum's slope in dB per octave.
static double spectralSlopeDbPerOctave (const std::vector<float>& samples,
                                        double sampleRate,
                                        const std::vector<std::pair<double, double>>& bands)
{
    std::vector<double> xs, ys;
    for (const auto& band : bands)
    {
        const double center = std::sqrt (band.first * band.second);
        xs.push_back (std::log2 (center));
        ys.push_back (bandPowerDb (samples, sampleRate, band.first, band.second));
    }

    const size_t n = xs.size();
    REQUIRE (n >= 3);

    double xMean = 0.0, yMean = 0.0;
    for (size_t i = 0; i < n; ++i)
    {
        xMean += xs[i];
        yMean += ys[i];
    }
    xMean /= static_cast<double> (n);
    yMean /= static_cast<double> (n);

    double num = 0.0, den = 0.0;
    for (size_t i = 0; i < n; ++i)
    {
        num += (xs[i] - xMean) * (ys[i] - yMean);
        den += (xs[i] - xMean) * (xs[i] - xMean);
    }

    return num / den;   // dB per octave
}

//==============================================================================
// 1. White noise is bit-identical for equal seeds, different for different seeds.
TEST_CASE ("White noise is bit-identical for equal seeds and differs for different seeds",
           "[noise][white][deterministic]")
{
    constexpr uint32_t seedA = 0x2E42A5;
    constexpr uint32_t seedB = 0x2E42A6;

    NoiseGenerator a, b, c;
    a.setType (NoiseGenerator::Type::white);
    b.setType (NoiseGenerator::Type::white);
    c.setType (NoiseGenerator::Type::white);
    a.setSeed (seedA);
    b.setSeed (seedA);
    c.setSeed (seedB);

    const auto samplesA = generateNoiseSamples (a, 48000.0, 8192);
    const auto samplesB = generateNoiseSamples (b, 48000.0, 8192);
    const auto samplesC = generateNoiseSamples (c, 48000.0, 8192);

    REQUIRE (samplesA == samplesB);  // equal seeds -> bit-identical output
    REQUIRE (samplesA != samplesC);  // different seeds -> different output
}

//==============================================================================
// 2. Pink noise is bit-identical for equal seeds, different for different seeds.
TEST_CASE ("Pink noise is bit-identical for equal seeds and differs for different seeds",
           "[noise][pink][deterministic]")
{
    constexpr uint32_t seedA = 0x2E42A5;
    constexpr uint32_t seedB = 0x2E42A6;

    NoiseGenerator a, b, c;
    a.setType (NoiseGenerator::Type::pink);
    b.setType (NoiseGenerator::Type::pink);
    c.setType (NoiseGenerator::Type::pink);
    a.setSeed (seedA);
    b.setSeed (seedA);
    c.setSeed (seedB);

    const auto samplesA = generateNoiseSamples (a, 48000.0, 8192);
    const auto samplesB = generateNoiseSamples (b, 48000.0, 8192);
    const auto samplesC = generateNoiseSamples (c, 48000.0, 8192);

    REQUIRE (samplesA == samplesB);  // equal seeds -> bit-identical output
    REQUIRE (samplesA != samplesC);  // different seeds -> different output
}

//==============================================================================
// 3. setDuration makes getTotalLength finite (never -1); unset duration -> 0.
TEST_CASE ("setDuration makes getTotalLength finite and positive", "[noise][duration]")
{
    NoiseGenerator ng;
    ng.setType (NoiseGenerator::Type::white);
    ng.setDuration (2.0);
    ng.prepare (48000.0, 512);

    const int64_t total = ng.getTotalLength();
    REQUIRE (total > 0);                     // finite, never -1
    REQUIRE (total == 96000);                // round (48000 * 2.0)
}

TEST_CASE ("getTotalLength is zero when no duration is set", "[noise][duration]")
{
    NoiseGenerator ng;
    ng.setType (NoiseGenerator::Type::pink);
    ng.prepare (48000.0, 512);

    REQUIRE (ng.getTotalLength() == 0);
}

//==============================================================================
// 4. Peak amplitude never exceeds the configured amplitude (white and pink).
TEST_CASE ("Peak amplitude never exceeds the configured amplitude", "[noise][amplitude]")
{
    constexpr double amplitude = 0.5;
    constexpr double sampleRate = 48000.0;
    const int numSamples = static_cast<int> (sampleRate);   // 1 second

    NoiseGenerator white;
    white.setType (NoiseGenerator::Type::white);
    white.setAmplitude (amplitude);
    white.setDuration (1.0);

    const auto whiteSamples = generateNoiseSamples (white, sampleRate, numSamples);
    const float whitePeak = *std::max_element (whiteSamples.begin(), whiteSamples.end(),
                                               [](float x, float y) { return std::abs (x) < std::abs (y); });
    REQUIRE (whitePeak <= static_cast<float> (amplitude));

    NoiseGenerator pink;
    pink.setType (NoiseGenerator::Type::pink);
    pink.setAmplitude (amplitude);
    pink.setDuration (1.0);

    const auto pinkSamples = generateNoiseSamples (pink, sampleRate, numSamples);
    const float pinkPeak = *std::max_element (pinkSamples.begin(), pinkSamples.end(),
                                              [](float x, float y) { return std::abs (x) < std::abs (y); });
    REQUIRE (pinkPeak <= static_cast<float> (amplitude));
}

//==============================================================================
// 5. Pink noise spectrum falls at ~-3 dB/octave (500 Hz - 8 kHz).
TEST_CASE ("Pink noise spectrum slopes at approximately -3 dB per octave",
           "[noise][pink][psd]")
{
    NoiseGenerator ng;
    ng.setType (NoiseGenerator::Type::pink);
    ng.setSeed (0x2E42A5);
    ng.prepare (48000.0, 512);

    constexpr int totalSamples = 16384;   // 4 x 4096 FFT frames
    juce::AudioBuffer<float> buffer (1, totalSamples);
    buffer.clear();
    ng.generate (buffer, 0, totalSamples);

    const float* data = buffer.getReadPointer (0);
    const std::vector<float> samples (data, data + totalSamples);

    const std::vector<std::pair<double, double>> bands = {
        { 500.0, 1000.0 }, { 1000.0, 2000.0 }, { 2000.0, 4000.0 }, { 4000.0, 8000.0 }
    };

    const double slope = spectralSlopeDbPerOctave (samples, 48000.0, bands);
    REQUIRE (slope == Catch::Approx (-3.0).margin (0.6));
}

//==============================================================================
// 6. White noise spectrum is flat (~0 dB/octave).
TEST_CASE ("White noise spectrum is flat (approximately 0 dB per octave)",
           "[noise][white][flat]")
{
    NoiseGenerator ng;
    ng.setType (NoiseGenerator::Type::white);
    ng.setSeed (0x2E42A5);
    ng.prepare (48000.0, 512);

    constexpr int totalSamples = 16384;   // 4 x 4096 FFT frames
    juce::AudioBuffer<float> buffer (1, totalSamples);
    buffer.clear();
    ng.generate (buffer, 0, totalSamples);

    const float* data = buffer.getReadPointer (0);
    const std::vector<float> samples (data, data + totalSamples);

    const std::vector<std::pair<double, double>> bands = {
        { 500.0, 1000.0 }, { 1000.0, 2000.0 }, { 2000.0, 4000.0 }, { 4000.0, 8000.0 }
    };

    const double slope = spectralSlopeDbPerOctave (samples, 48000.0, bands);
    REQUIRE (slope == Catch::Approx (0.0).margin (0.6));
}

//==============================================================================
// 7. reset() re-seeds the generator: replaying from the start is bit-identical.
TEST_CASE ("reset reproduces the same samples from the start", "[noise][reset]")
{
    constexpr uint32_t seed = 0x2E42A5;
    constexpr int blockSamples = 4096;

    NoiseGenerator pink;
    pink.setType (NoiseGenerator::Type::pink);
    pink.setSeed (seed);
    pink.prepare (48000.0, 512);

    juce::AudioBuffer<float> first (1, blockSamples);
    pink.generate (first, 0, blockSamples);

    // Advance the generator state well past the first block.
    juce::AudioBuffer<float> scratch (1, blockSamples);
    pink.generate (scratch, 0, blockSamples);
    pink.generate (scratch, 0, blockSamples);

    pink.reset();

    juce::AudioBuffer<float> replay (1, blockSamples);
    pink.generate (replay, 0, blockSamples);

    const float* a = first.getReadPointer (0);
    const float* b = replay.getReadPointer (0);
    for (int i = 0; i < blockSamples; ++i)
        REQUIRE (a[i] == b[i]);
}
