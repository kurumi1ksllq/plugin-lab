#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <cmath>
#include <vector>

#include "TestCompressorPlugin.h"

//==============================================================================
// Tests for TestCompressorPlugin — the reference compressor used as ground
// truth for dynamic-compression measurements.
//
// Model under test (see TestCompressorPlugin.h):
//   levelDB    = 20 * log10(|sample|)                      (static curve)
//   grTarget   = levelDB > threshold ? (1 - 1/ratio) * (levelDB - threshold) : 0
//   tauDir     = grTarget > grSmoothed ? attack : release
//   grSmoothed += (1 - exp(-1/(sr * tauDir))) * (grTarget - grSmoothed)
//   output     = sample * dBToGain(grSmoothed) * dBToGain(makeupGain)
//
// The GR smoothing is a single-pole filter, so for a step of GR_ss:
//   attack : GR(t) = GR_ss * (1 - e^(-t/tau_attack))
//   release: GR(t) = GR_ss * e^(-t/tau_release)
// which lets the time constants be measured exactly at t = tau.

namespace
{
constexpr double kSampleRate = 48000.0;
constexpr double kThresholdDB = -20.0;
constexpr double kRatio = 4.0;

/** dB -> linear amplitude gain. */
double dBToGain (double db)
{
    return std::pow (10.0, db / 20.0);
}

/** Static compression-curve GR (dB) for a constant amplitude level. */
double expectedGR (double level)
{
    const double levelDB = 20.0 * std::log10 (level);
    return (1.0 - 1.0 / kRatio) * (levelDB - kThresholdDB);
}

/**
    Fills a stereo buffer with a constant level and processes it through the
    plugin in `chunkSize`-sized blocks until `numSamples` samples have been
    consumed (the final block may be partial).

    If `lastChunkOutput` is non-null, the final chunk's channel-0 samples are
    copied into it.
*/
void processConstantLevel (juce::AudioProcessor& processor, float level,
                           int numSamples, int chunkSize,
                           std::vector<float>* lastChunkOutput = nullptr)
{
    juce::AudioBuffer<float> buffer (2, chunkSize);
    juce::MidiBuffer midi;

    for (int processed = 0; processed < numSamples; processed += chunkSize)
    {
        const int n = juce::jmin (chunkSize, numSamples - processed);
        if (n != chunkSize)
            buffer.setSize (2, n, false, true, false);

        for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
            for (int s = 0; s < buffer.getNumSamples(); ++s)
                buffer.setSample (ch, s, level);

        processor.processBlock (buffer, midi);
    }

    if (lastChunkOutput != nullptr)
    {
        lastChunkOutput->assign (static_cast<size_t> (buffer.getNumSamples()), 0.0f);
        for (int s = 0; s < buffer.getNumSamples(); ++s)
            (*lastChunkOutput)[static_cast<size_t> (s)] = buffer.getSample (0, s);
    }
}

/** RMS (linear amplitude) of a captured output chunk. */
double rmsOf (const std::vector<float>& samples)
{
    double sum = 0.0;
    for (float s : samples)
        sum += static_cast<double> (s) * static_cast<double> (s);
    return std::sqrt (sum / static_cast<double> (samples.size()));
}
} // namespace

//==============================================================================

TEST_CASE ("Compressor leaves below-threshold signals untouched", "[compressor][below-threshold]")
{
    TestCompressorPlugin plugin;
    plugin.setThresholdDB (kThresholdDB);
    plugin.setRatio (kRatio);
    plugin.setAttackSec (0.005);
    plugin.setReleaseSec (0.05);
    plugin.setMakeupGainDB (0.0);
    plugin.prepareToPlay (kSampleRate, 512);

    // 0.01 -> -40 dB, well below the -20 dB threshold: no gain reduction.
    std::vector<float> lastChunk;
    processConstantLevel (plugin, 0.01f, static_cast<int> (2 * kSampleRate), 512, &lastChunk);

    REQUIRE (plugin.getCurrentGRDB() == Catch::Approx (0.0).margin (0.001));
    for (float s : lastChunk)
        REQUIRE (s == Catch::Approx (0.01f).margin (0.001f));
}

TEST_CASE ("Compressor reaches steady-state gain reduction for a constant high level",
           "[compressor][steady-state]")
{
    TestCompressorPlugin plugin;
    plugin.setThresholdDB (kThresholdDB);
    plugin.setRatio (kRatio);
    plugin.setAttackSec (0.005);
    plugin.setReleaseSec (0.05);
    plugin.setMakeupGainDB (0.0);
    plugin.prepareToPlay (kSampleRate, 512);

    // 0.5 -> -6.02 dB; GR_ss = (1 - 1/4) * (-6.02 - (-20)) = 10.485 dB.
    const double grExpected = expectedGR (0.5);

    // 2 s is ~400 attack time constants: GR is fully converged.
    std::vector<float> lastChunk;
    processConstantLevel (plugin, 0.5f, static_cast<int> (2 * kSampleRate), 512, &lastChunk);

    REQUIRE (plugin.getCurrentGRDB() == Catch::Approx (grExpected).margin (0.1));

    // output RMS == input RMS / gain(GR_ss)
    const double expectedOut = 0.5 * dBToGain (-grExpected);
    REQUIRE (rmsOf (lastChunk) == Catch::Approx (expectedOut).epsilon (0.01));
}

TEST_CASE ("Compressor attack follows a 1-pole curve with the configured attack tau",
           "[compressor][attack-tau]")
{
    TestCompressorPlugin plugin;
    plugin.setThresholdDB (kThresholdDB);
    plugin.setRatio (kRatio);
    plugin.setAttackSec (0.005);
    plugin.setReleaseSec (0.05);
    plugin.setMakeupGainDB (0.0);
    plugin.prepareToPlay (kSampleRate, 512);

    // tau_attack = 5 ms = 240 samples at 48 kHz. A single 240-sample block of
    // 0.5 lands the final sample exactly at t = tau.
    const int tauSamples = static_cast<int> (0.005 * kSampleRate);
    std::vector<float> lastChunk;
    processConstantLevel (plugin, 0.5f, tauSamples, tauSamples, &lastChunk);

    // GR(tau) == GR_ss * (1 - e^-1) = 0.6321 * GR_ss (within +/- 5% relative).
    const double grExpected = expectedGR (0.5);
    REQUIRE (plugin.getCurrentGRDB() == Catch::Approx ((1.0 - std::exp (-1.0)) * grExpected).epsilon (0.05));
}

TEST_CASE ("Compressor release follows a 1-pole curve with the configured release tau",
           "[compressor][release-tau]")
{
    TestCompressorPlugin plugin;
    plugin.setThresholdDB (kThresholdDB);
    plugin.setRatio (kRatio);
    plugin.setAttackSec (0.005);
    plugin.setReleaseSec (0.05);
    plugin.setMakeupGainDB (0.0);
    plugin.prepareToPlay (kSampleRate, 512);

    // Reach steady state at 0.5 (GR = GR_ss), then drop below threshold.
    processConstantLevel (plugin, 0.5f, static_cast<int> (2 * kSampleRate), 512, nullptr);
    const double grBefore = plugin.getCurrentGRDB();

    // tau_release = 50 ms = 2400 samples at 48 kHz. One 2400-sample block of
    // 0.01 (-40 dB < threshold) makes the final sample sit at t = tau.
    const int tauSamples = static_cast<int> (0.05 * kSampleRate);
    std::vector<float> lastChunk;
    processConstantLevel (plugin, 0.01f, tauSamples, tauSamples, &lastChunk);

    // GR(tau) == GR_ss * e^-1 = 0.3679 * GR_ss (within +/- 5% relative).
    REQUIRE (plugin.getCurrentGRDB() == Catch::Approx (std::exp (-1.0) * grBefore).epsilon (0.05));
}

TEST_CASE ("Compressor reports zero latency and zero tail", "[compressor][no-latency-tail]")
{
    TestCompressorPlugin plugin;

    REQUIRE (plugin.getLatencySamples() == 0);
    REQUIRE (plugin.getTailLengthSeconds() == Catch::Approx (0.0));
}
