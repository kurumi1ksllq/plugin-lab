#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

#include "TestCompressorPlugin.h"
#include "../source/analysis/TimeConstants.h"

//==============================================================================
// Tests for TimeConstants — the attack/release time-constant estimator that
// runs on a GainReduction::Result timeline.
//
// Ground truth: TestCompressorPlugin applies exact single-pole smoothing
//   attack : GR(t) = GR_ss * (1 - e^(-t / tau_attack))
//   release: GR(t) = GR_ss * e^(-t / tau_release)
// so for a level step the GR timeline is a precisely predictable exponential
// and the estimated tau must come back within the configured value.
//
// All timelines here are captured per-sample (one point per sample) by
// processing 1-sample blocks and recording getCurrentGRDB() after each one —
// the plugin has zero latency, so the timeline time axis is sample/sr exactly.

namespace
{
constexpr double kSampleRate = 48000.0;
constexpr double kAttackSec = 0.005;
constexpr double kReleaseSec = 0.050;

//==============================================================================
/** Configures the reference compressor with the default known taus. */
void configureCompressor (TestCompressorPlugin& plugin)
{
    plugin.setThresholdDB (-20.0);
    plugin.setRatio (4.0);
    plugin.setAttackSec (kAttackSec);
    plugin.setReleaseSec (kReleaseSec);
    plugin.setMakeupGainDB (0.0);
    plugin.prepareToPlay (kSampleRate, 512);
}

/** Per-sample level profile: [0, stepSample) = level0, [stepSample, end) = level1. */
std::vector<float> makeStepProfile (double level0, double level1,
                                    int stepSample, int numSamples)
{
    std::vector<float> levels (static_cast<size_t> (numSamples), static_cast<float> (level0));
    for (int i = stepSample; i < numSamples; ++i)
        levels[static_cast<size_t> (i)] = static_cast<float> (level1);
    return levels;
}

/**
 * Processes the plugin sample-by-sample over the given level profile and
 * records (timeSec, getCurrentGRDB()) after every sample, producing a
 * per-sample GR timeline.
 */
GainReduction::Result captureGRTimeline (TestCompressorPlugin& plugin,
                                         const std::vector<float>& levels)
{
    GainReduction::Result result;
    result.sampleRate = kSampleRate;

    juce::AudioBuffer<float> buffer (2, 1);
    juce::MidiBuffer midi;
    for (size_t i = 0; i < levels.size(); ++i)
    {
        const float level = levels[i];
        buffer.setSample (0, 0, level);
        buffer.setSample (1, 0, level);
        plugin.processBlock (buffer, midi);

        GainReduction::Point p;
        p.timeSec = static_cast<double> (i) / kSampleRate;
        p.grDB = plugin.getCurrentGRDB();
        result.timeline.push_back (p);
    }

    result.numPoints = static_cast<int> (result.timeline.size());
    return result;
}

/** Points whose timeSec falls inside [startSample, endSample] (samples). */
std::vector<GainReduction::Point> pointsInRange (const GainReduction::Result& gr,
                                                 int64_t startSample, int64_t endSample)
{
    std::vector<GainReduction::Point> pts;
    const double t0 = static_cast<double> (startSample) / kSampleRate;
    const double t1 = static_cast<double> (endSample) / kSampleRate;
    for (const auto& p : gr.timeline)
        if (p.timeSec >= t0 && p.timeSec <= t1)
            pts.push_back (p);
    return pts;
}

/** Scenario shared by the combo / edge-time / fit-vs-edge tests. */
struct ComboScenario
{
    GainReduction::Result gr;
    TimeConstants::EventMarkers markers;
};

/**
 * Builds a full attack+release scenario: 0.01 s of silence, then 0.5 s at
 * level 0.5 (attack into steady state), then 1 s at 0.01 (below threshold,
 * release). The release edge runs to the end of the timeline.
 */
ComboScenario runComboScenario (TestCompressorPlugin& plugin)
{
    const int preRoll = static_cast<int> (0.01 * kSampleRate);
    const int highEnd = preRoll + static_cast<int> (0.5 * kSampleRate);
    const int total = highEnd + static_cast<int> (1.0 * kSampleRate);

    std::vector<float> levels (static_cast<size_t> (total), 0.0f);
    for (int i = preRoll; i < highEnd; ++i)
        levels[static_cast<size_t> (i)] = 0.5f;
    for (int i = highEnd; i < total; ++i)
        levels[static_cast<size_t> (i)] = 0.01f;

    ComboScenario sc;
    sc.gr = captureGRTimeline (plugin, levels);
    sc.markers.attackStart = static_cast<int64_t> (preRoll);
    sc.markers.attackEnd = static_cast<int64_t> (highEnd);
    sc.markers.releaseStart = static_cast<int64_t> (highEnd);
    // releaseEnd stays 0: the release edge runs to the end of the timeline.
    return sc;
}
} // namespace

//==============================================================================
// Test 1: Known attack tau — a 0 -> 0.5 step produces an attack edge whose
// fitted tau matches the configured 5 ms; no release edge is marked, so the
// release tau stays unestimated.
//==============================================================================

TEST_CASE ("TimeConstants: attack tau from a 0 -> 0.5 step matches the configured 5 ms",
           "[timeconstants][known-attack]")
{
    // Arrange — 1 s of constant 0.5 (-6 dB > -20 dB threshold): GR rises from
    // 0 to GR_ss with a pure single-pole attack edge starting at t = 0.
    TestCompressorPlugin plugin;
    configureCompressor (plugin);
    const auto gr = captureGRTimeline (plugin,
        makeStepProfile (0.5, 0.5, 0, static_cast<int> (1.0 * kSampleRate)));

    TimeConstants::EventMarkers markers;
    markers.attackStart = 0;
    markers.attackEnd = static_cast<int64_t> (1.0 * kSampleRate);

    // Act
    const auto result = TimeConstants::estimate (gr, markers, kSampleRate);

    // Assert
    REQUIRE (result.valid);
    REQUIRE (result.tauAttackSec == Catch::Approx (kAttackSec).epsilon (0.10));
    // No release edge marked -> release tau stays unestimated.
    REQUIRE (result.tauReleaseSec == 0.0);
}

//==============================================================================
// Test 2: Known release tau — after steady state, a 0.5 -> 0.01 step (below
// threshold) produces a release edge whose fitted tau matches 50 ms.
//==============================================================================

TEST_CASE ("TimeConstants: release tau from a 0.5 -> 0.01 step matches the configured 50 ms",
           "[timeconstants][known-release]")
{
    // Arrange — 1 s of 0.5 (steady state), then 1 s of 0.01 (below threshold).
    TestCompressorPlugin plugin;
    configureCompressor (plugin);
    const auto gr = captureGRTimeline (plugin,
        makeStepProfile (0.5, 0.01, static_cast<int> (1.0 * kSampleRate),
                         static_cast<int> (2.0 * kSampleRate)));

    TimeConstants::EventMarkers markers;
    markers.attackStart = 0;
    markers.attackEnd = static_cast<int64_t> (1.0 * kSampleRate);
    markers.releaseStart = static_cast<int64_t> (1.0 * kSampleRate);
    // releaseEnd left 0: the release edge runs to the end of the timeline.

    // Act
    const auto result = TimeConstants::estimate (gr, markers, kSampleRate);

    // Assert
    REQUIRE (result.valid);
    REQUIRE (result.tauReleaseSec == Catch::Approx (kReleaseSec).epsilon (0.10));
}

//==============================================================================
// Test 3: Full attack + release scenario — both taus estimated in one pass.
//==============================================================================

TEST_CASE ("TimeConstants: attack+release combo estimates both configured taus",
           "[timeconstants][attack-release-combo]")
{
    // Arrange
    TestCompressorPlugin plugin;
    configureCompressor (plugin);
    const auto sc = runComboScenario (plugin);

    // Act
    const auto result = TimeConstants::estimate (sc.gr, sc.markers, kSampleRate);

    // Assert — both edges present, both taus recovered within +/-10%.
    REQUIRE (result.valid);
    REQUIRE (result.tauAttackSec == Catch::Approx (kAttackSec).epsilon (0.10));
    REQUIRE (result.tauReleaseSec == Catch::Approx (kReleaseSec).epsilon (0.10));
}

//==============================================================================
// Test 4: edgeTime — for a single pole the 10% -> 90% transition time equals
// tau * ln(0.9/0.1) = tau * ln(9) ~= 2.1972 * tau.
//==============================================================================

TEST_CASE ("TimeConstants: edgeTime equals tau * ln(9) for single-pole edges",
           "[timeconstants][edge-time]")
{
    // Arrange
    TestCompressorPlugin plugin;
    configureCompressor (plugin);
    const auto sc = runComboScenario (plugin);

    // Act
    const double attackEdge = TimeConstants::edgeTime (sc.gr, sc.markers, "attack", kSampleRate);
    const double releaseEdge = TimeConstants::edgeTime (sc.gr, sc.markers, "release", kSampleRate);

    // Assert — 10%-90% time of an exact single pole = tau * ln(9).
    const double expectedAttack = kAttackSec * std::log (9.0);
    const double expectedRelease = kReleaseSec * std::log (9.0);
    INFO ("attack edge = " << attackEdge << " s, expected " << expectedAttack << " s");
    INFO ("release edge = " << releaseEdge << " s, expected " << expectedRelease << " s");
    REQUIRE (attackEdge == Catch::Approx (expectedAttack).epsilon (0.10));
    REQUIRE (releaseEdge == Catch::Approx (expectedRelease).epsilon (0.10));
}

//==============================================================================
// Test 5: fitTau vs edgeTime — the least-squares log-domain fit and the
// 10%-90% edge estimate measure the same single-pole tau (within 20%).
//==============================================================================

TEST_CASE ("TimeConstants: fitTau and edgeTime agree for attack and release",
           "[timeconstants][fit-vs-edge]")
{
    // Arrange
    TestCompressorPlugin plugin;
    configureCompressor (plugin);
    const auto sc = runComboScenario (plugin);

    // fitTau takes the marker interval points (the same rule estimate() uses);
    // the release edge runs to the end of the timeline (releaseEnd == 0).
    const auto attackPts = pointsInRange (sc.gr, sc.markers.attackStart, sc.markers.attackEnd);
    const auto releasePts = pointsInRange (sc.gr, sc.markers.releaseStart,
                                           std::numeric_limits<int64_t>::max());

    // Act
    const double tauFitAttack = TimeConstants::fitTau (attackPts, "attack", kSampleRate);
    const double tauFitRelease = TimeConstants::fitTau (releasePts, "release", kSampleRate);
    const double tauEdgeAttack = TimeConstants::edgeTime (sc.gr, sc.markers, "attack", kSampleRate) / std::log (9.0);
    const double tauEdgeRelease = TimeConstants::edgeTime (sc.gr, sc.markers, "release", kSampleRate) / std::log (9.0);

    // Assert — both estimators recover the same single-pole tau.
    INFO ("fit attack = " << tauFitAttack << " s, edge attack = " << tauEdgeAttack << " s");
    INFO ("fit release = " << tauFitRelease << " s, edge release = " << tauEdgeRelease << " s");
    REQUIRE (tauFitAttack == Catch::Approx (tauEdgeAttack).epsilon (0.20));
    REQUIRE (tauFitRelease == Catch::Approx (tauEdgeRelease).epsilon (0.20));
}

//==============================================================================
// Test 6: tau-by-level — the ideal compressor's attack tau is level-
// independent; the curve family has exactly numBins bins whose level axis is
// strictly increasing (level-ordered).
//==============================================================================

TEST_CASE ("TimeConstants: tau-by-level curve family is level-independent with numBins monotonic bins",
           "[timeconstants][tau-by-level]")
{
    // Three input levels spanning the compressor's working range.
    for (double level : { 0.3, 0.5, 0.8 })
    {
        TestCompressorPlugin plugin;
        configureCompressor (plugin);
        const auto gr = captureGRTimeline (plugin,
            makeStepProfile (level, level, 0, static_cast<int> (1.0 * kSampleRate)));

        TimeConstants::EventMarkers markers;
        markers.attackStart = 0;
        markers.attackEnd = static_cast<int64_t> (1.0 * kSampleRate);

        // Act
        const auto result = TimeConstants::estimate (gr, markers, kSampleRate);

        // Assert — tau independent of level (within +/-10%), structure intact.
        INFO ("input level = " << level);
        REQUIRE (result.valid);
        REQUIRE (result.tauAttackSec == Catch::Approx (kAttackSec).epsilon (0.10));

        REQUIRE (result.attackByLevel.levelDB.size() == 10);
        REQUIRE (result.attackByLevel.tauSec.size() == 10);
        for (size_t b = 1; b < result.attackByLevel.levelDB.size(); ++b)
        {
            INFO ("bin " << b << " levelDB = " << result.attackByLevel.levelDB[b]
                         << " (prev " << result.attackByLevel.levelDB[b - 1] << ")");
            REQUIRE (result.attackByLevel.levelDB[b] > result.attackByLevel.levelDB[b - 1]);
        }
        for (double tau : result.attackByLevel.tauSec)
            REQUIRE (std::isfinite (tau));
    }
}

//==============================================================================
// Test 7: Invalid input — a zero-GR timeline (no compression at all) must not
// crash and must yield no estimate and no NaN.
//==============================================================================

TEST_CASE ("TimeConstants: zero-GR timeline yields an invalid result without NaN",
           "[timeconstants][invalid]")
{
    // Arrange — an identity path produces a flat 0 dB GR timeline.
    GainReduction::Result gr;
    gr.sampleRate = kSampleRate;
    for (int i = 0; i < static_cast<int> (1.0 * kSampleRate); ++i)
    {
        GainReduction::Point p;
        p.timeSec = static_cast<double> (i) / kSampleRate;
        p.grDB = 0.0;
        gr.timeline.push_back (p);
    }
    gr.numPoints = static_cast<int> (gr.timeline.size());

    TimeConstants::EventMarkers markers;
    markers.attackStart = 0;
    markers.attackEnd = static_cast<int64_t> (1.0 * kSampleRate);
    markers.releaseStart = static_cast<int64_t> (0.5 * kSampleRate);

    // Act
    const auto result = TimeConstants::estimate (gr, markers, kSampleRate);

    // Assert — no estimate, flagged invalid, everything finite.
    REQUIRE (! result.valid);
    REQUIRE (result.tauAttackSec == 0.0);
    REQUIRE (result.tauReleaseSec == 0.0);
    REQUIRE (std::isfinite (result.tauAttackSec));
    REQUIRE (std::isfinite (result.tauReleaseSec));
    for (double tau : result.attackByLevel.tauSec)
        REQUIRE (std::isfinite (tau));
    for (double tau : result.releaseByLevel.tauSec)
        REQUIRE (std::isfinite (tau));
}
