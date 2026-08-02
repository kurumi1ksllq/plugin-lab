#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <cmath>
#include <memory>

#include "TestPlugin.h"
#include "../source/capture/SweepRunner.h"
#include "../source/signal/SineSweep.h"
#include "../source/analysis/GainReduction.h"

//==============================================================================
// Helper: run a sweep through the given plugin and keep the runner alive so
// the recorded dry/wet buffers can be inspected.
static std::unique_ptr<SweepRunner> runSweep (TestPlugin& plugin,
                                              double durationSec,
                                              int tailPad)
{
    SineSweep sweep;
    sweep.setFrequencyRange (100.0, 5000.0);
    sweep.setDuration (durationSec);
    sweep.setAmplitude (0.5);

    auto runner = std::make_unique<SweepRunner>();
    runner->prepare (48000.0, 512);
    runner->setGenerator (&sweep);
    runner->setPlugin (&plugin);
    runner->setTailPadSamples (tailPad);
    REQUIRE (runner->run());
    return runner;
}

//==============================================================================
// Helper: mean |grDB - expected| over the middle region [t0, t1] seconds.
// Returns a huge value when no points fall inside the region.
static double meanGRAbsError (const GainReduction::Result& result,
                              double t0, double t1, double expected)
{
    double sum = 0.0;
    int count = 0;
    for (const auto& p : result.timeline)
    {
        if (p.timeSec >= t0 && p.timeSec <= t1)
        {
            sum += std::abs (p.grDB - expected);
            ++count;
        }
    }
    return count > 0 ? sum / static_cast<double> (count) : 1e9;
}

//==============================================================================
// Test 1: Constant gain — 20*log10(0.5) = -6.02 dB, flat in the middle.
//==============================================================================

TEST_CASE ("GainReduction: constant gain 0.5 gives flat -6.02 dB mid-signal",
           "[gainreduction][constant-gain]")
{
    // Arrange
    TestPlugin plugin;
    plugin.setGain (0.5);

    auto runner = runSweep (plugin, 2.0, 100);
    const auto& dry = runner->getResult().getDryBuffer();
    const auto& wet = runner->getResult().getWetBuffer();

    // Act
    auto result = GainReduction::analyze (dry, wet, 48000.0, 0);

    // Assert — populated, valid metadata
    REQUIRE (! result.timeline.empty());
    REQUIRE (result.numPoints == static_cast<int> (result.timeline.size()));
    REQUIRE (result.sampleRate == Catch::Approx (48000.0));

    // Assert — middle region flat at -6.02 dB (±0.05)
    const double expected = 20.0 * std::log10 (0.5);
    const double meanErr = meanGRAbsError (result, 0.2, 1.8, expected);
    INFO ("Mean |GR - expected| over middle = " << meanErr << " dB");
    REQUIRE (meanErr < 0.05);
}

//==============================================================================
// Test 2: Latency alignment — correct alignment removes edge errors.
//==============================================================================

TEST_CASE ("GainReduction: latency alignment keeps GR flat with no edge spikes",
           "[gainreduction][latency-align]")
{
    // Arrange — plugin reports and realises 100 samples of latency
    TestPlugin plugin;
    plugin.setGain (0.5);
    plugin.setLatencySamples (100);

    auto runner = runSweep (plugin, 2.0, 100);  // tailPad ≥ latency
    const auto& dry = runner->getResult().getDryBuffer();
    const auto& wet = runner->getResult().getWetBuffer();

    // Act — analyze with the matching latency
    auto result = GainReduction::analyze (dry, wet, 48000.0, 100);

    // Assert — every measured point is ≈ -6.02 dB; correct alignment means
    // no edge spikes anywhere (the wet window [i+latency, ...) matches the
    // dry window [i, ...) sample-for-sample).
    const double expected = 20.0 * std::log10 (0.5);
    REQUIRE (! result.timeline.empty());
    int measured = 0;
    for (const auto& p : result.timeline)
    {
        if (p.grDB != 0.0)  // skip silence-protected points (grDB forced to 0)
        {
            INFO ("Point at t=" << p.timeSec << " s, GR=" << p.grDB << " dB");
            REQUIRE (std::abs (p.grDB - expected) < 0.05);
            ++measured;
        }
    }
    REQUIRE (measured > 0);
}

//==============================================================================
// Test 3: Identity — unity gain ⇒ 0 dB.
//==============================================================================

TEST_CASE ("GainReduction: unity gain gives approx 0 dB", "[gainreduction][identity]")
{
    // Arrange — TestPlugin default gain is 1.0
    TestPlugin plugin;

    auto runner = runSweep (plugin, 2.0, 100);
    const auto& dry = runner->getResult().getDryBuffer();
    const auto& wet = runner->getResult().getWetBuffer();

    // Act
    auto result = GainReduction::analyze (dry, wet, 48000.0, 0);

    // Assert — middle region flat at 0 dB (±0.05)
    const double meanErr = meanGRAbsError (result, 0.2, 1.8, 0.0);
    INFO ("Mean |GR| over middle = " << meanErr << " dB");
    REQUIRE (meanErr < 0.05);
}

//==============================================================================
// Test 4: Silence tail — no NaN/inf in the padded tail region.
//==============================================================================

TEST_CASE ("GainReduction: silence tail produces no NaN/inf", "[gainreduction][silence-tail]")
{
    // Arrange — long tail so several windows fall entirely in the silence
    TestPlugin plugin;
    plugin.setGain (1.0);

    auto runner = runSweep (plugin, 2.0, 1000);
    const auto& dry = runner->getResult().getDryBuffer();
    const auto& wet = runner->getResult().getWetBuffer();

    // Act
    auto result = GainReduction::analyze (dry, wet, 48000.0, 0);

    // Assert — every point is finite
    REQUIRE (! result.timeline.empty());
    for (const auto& p : result.timeline)
    {
        REQUIRE (std::isfinite (p.grDB));
        REQUIRE (std::isfinite (p.timeSec));
    }

    // Assert — points inside the pure-silence tail (after the 2 s signal)
    // are protected: grDB is forced to 0 instead of -inf/NaN.
    int tailPoints = 0;
    for (const auto& p : result.timeline)
    {
        if (p.timeSec > 2.0)
        {
            REQUIRE (p.grDB == 0.0);
            ++tailPoints;
        }
    }
    REQUIRE (tailPoints > 0);
}
