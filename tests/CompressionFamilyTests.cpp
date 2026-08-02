/**
 * CompressionFamily unit tests (TDD: RED phase).
 *
 * Exercises the compression-response-family measurement: a grid over
 * (input level × envelope speed) producing, per cell, the static
 * compression curve, the gain-reduction timeline and the attack/release
 * time-constant estimates:
 *
 *   - [compression-family][grid-count] : levels { -20, -10 } × speeds
 *                                        { 0.5, 1, 2 } → 6 valid entries
 *   - [compression-family][tau-match]  : single cell against
 *                                        TestCompressorPlugin (attack 5 ms /
 *                                        release 50 ms). The attack estimate
 *                                        is compared against the effective
 *                                        AC-driven time constant
 *                                        2*tau_a*tau_r/(tau_a+tau_r) = 9.09 ms
 *                                        (see test body), the release against
 *                                        the configured 50 ms.
 *   - [compression-family][curve-present] : one cell carries a finite curve
 *   - [compression-family][gr-present]    : one cell carries a finite GR
 *                                           timeline
 *   - [compression-family][json-schema]   : compressionFamilyToJSON schema
 *   - [compression-family][cancel]        : cancel() from another thread
 *
 * Ground truth: TestCompressorPlugin applies exact single-pole smoothing
 *   attack : GR(t) = GR_ss * (1 - e^(-t / tau_attack))
 *   release: GR(t) = GR_ss * e^(-t / tau_release)
 * so a level step yields a precisely predictable exponential edge.
 */
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <atomic>
#include <chrono>
#include <cmath>
#include <memory>
#include <thread>
#include <vector>

#include "TestCompressorPlugin.h"
#include "../source/capture/MeasurementSession.h"
#include "../source/analysis/CompressionFamily.h"
#include "../source/analysis/Export.h"

namespace
{
constexpr double kSampleRate = 48000.0;
constexpr int kBlockSize = 512;
constexpr double kAttackSec = 0.005;
constexpr double kReleaseSec = 0.050;

/** Configures the reference compressor with the known taus (threshold -30 dB
 *  so both grid levels { -20, -10 } sit inside the compression region). */
void configureCompressor (TestCompressorPlugin& plugin)
{
    plugin.setThresholdDB (-30.0);
    plugin.setRatio (4.0);
    plugin.setAttackSec (kAttackSec);
    plugin.setReleaseSec (kReleaseSec);
    plugin.setMakeupGainDB (0.0);
    plugin.prepareToPlay (kSampleRate, kBlockSize);
}

/** Run a compression-family measurement over the given grid. */
CompressionFamily::FamilyResult runFamily (TestCompressorPlugin& plugin,
                                           MeasurementSession& session,
                                           const std::vector<double>& levelsDB,
                                           const std::vector<double>& speeds,
                                           std::function<void(int, int)> progress = {})
{
    configureCompressor (plugin);
    session.setPluginInstance (&plugin);
    session.setSampleRate (kSampleRate);
    session.setBlockSize (kBlockSize);
    return CompressionFamily::measure (&plugin, &session, levelsDB, speeds, progress);
}

/** Assert every GR timeline point is finite. */
void requireFiniteGR (const GainReduction::Result& gr)
{
    REQUIRE_FALSE (gr.timeline.empty());
    for (const auto& p : gr.timeline)
    {
        REQUIRE (std::isfinite (p.timeSec));
        REQUIRE (std::isfinite (p.grDB));
    }
}
} // namespace

//==============================================================================
// 1. Grid count — levels × speeds → one valid entry per cell
//==============================================================================

TEST_CASE ("CompressionFamily: level x speed grid yields one valid entry per cell",
           "[compression-family][grid-count]")
{
    TestCompressorPlugin plugin;
    MeasurementSession session;
    int progressCalls = 0;

    const auto result = runFamily (plugin, session,
                                   { -20.0, -10.0 }, { 0.5, 1.0, 2.0 },
                                   [&] (int, int) { ++progressCalls; });

    REQUIRE (result.levelsDB.size() == 2);
    REQUIRE (result.speeds.size() == 3);
    REQUIRE (result.entries.size() == 6);
    REQUIRE_FALSE (result.cancelled);
    REQUIRE (progressCalls == 6);

    // Grid order: outer loop levels, inner loop speeds.
    const std::vector<double> expectedLevels = { -20.0, -20.0, -20.0, -10.0, -10.0, -10.0 };
    const std::vector<double> expectedSpeeds = { 0.5, 1.0, 2.0, 0.5, 1.0, 2.0 };

    for (size_t i = 0; i < result.entries.size(); ++i)
    {
        INFO ("cell " << i);
        REQUIRE (result.entries[i].inputLevelDB == Catch::Approx (expectedLevels[i]));
        REQUIRE (result.entries[i].speed == Catch::Approx (expectedSpeeds[i]));
        // Every cell compresses (threshold -30 dB, both levels above it) and
        // yields a measurable attack edge → valid.
        REQUIRE (result.entries[i].valid);
    }
}

//==============================================================================
// 2. tau-match — the τ estimates recover the compressor's dynamics
//==============================================================================

TEST_CASE ("CompressionFamily: tau matches effective attack and configured release",
           "[compression-family][tau-match]")
{
    TestCompressorPlugin plugin;
    MeasurementSession session;

    const auto result = runFamily (plugin, session, { -10.0 }, { 1.0 });
    REQUIRE (result.entries.size() == 1);

    const auto& entry = result.entries[0];
    REQUIRE (entry.valid);
    REQUIRE (entry.tau.valid);

    // Attack. The direction-dependent single-pole detector drives the level
    // through an AC carrier: on the rising half-cycle it smooths with
    // tau_attack, on the falling half-cycle with tau_release. Averaged over a
    // carrier cycle the edge therefore charges with the effective time
    // constant 2*tau_a*tau_r/(tau_a+tau_r) = 9.09 ms — the configured 5 ms is
    // physically unreachable on this chain (the falling half-cycles release
    // with tau_r and slow the charge). The estimate must match the effective
    // value (±20%).
    const double effectiveAttack = 2.0 * kAttackSec * kReleaseSec / (kAttackSec + kReleaseSec);
    INFO ("tau_attack = " << (entry.tau.tauAttackSec * 1000.0) << " ms, effective = "
                          << (effectiveAttack * 1000.0) << " ms");
    REQUIRE (entry.tau.tauAttackSec == Catch::Approx (effectiveAttack).epsilon (0.20));

    // Release. Once the input drops below the threshold the target is a DC
    // zero and the decay is a pure single-pole with the configured 50 ms
    // (±30%).
    INFO ("tau_release = " << (entry.tau.tauReleaseSec * 1000.0) << " ms, expected = "
                           << (kReleaseSec * 1000.0) << " ms");
    REQUIRE (entry.tau.tauReleaseSec == Catch::Approx (kReleaseSec).epsilon (0.30));
}

//==============================================================================
// 3. curve-present — the static compression curve is populated and finite
//==============================================================================

TEST_CASE ("CompressionFamily: each entry carries a finite compression curve",
           "[compression-family][curve-present]")
{
    TestCompressorPlugin plugin;
    MeasurementSession session;

    const auto result = runFamily (plugin, session, { -10.0 }, { 1.0 });
    REQUIRE (result.entries.size() == 1);

    const auto& curve = result.entries[0].curve.curve;
    REQUIRE_FALSE (curve.empty());
    for (const auto& p : curve)
    {
        REQUIRE (std::isfinite (p.inputDB));
        REQUIRE (std::isfinite (p.outputDB));
        REQUIRE (std::isfinite (p.gainReductionDB));
    }
}

//==============================================================================
// 4. gr-present — the GR timeline is populated and finite
//==============================================================================

TEST_CASE ("CompressionFamily: each entry carries a finite GR timeline",
           "[compression-family][gr-present]")
{
    TestCompressorPlugin plugin;
    MeasurementSession session;

    const auto result = runFamily (plugin, session, { -10.0 }, { 1.0 });
    REQUIRE (result.entries.size() == 1);

    const auto& gr = result.entries[0].gr;
    requireFiniteGR (gr);
    REQUIRE (gr.numPoints == static_cast<int> (gr.timeline.size()));
    REQUIRE (gr.sampleRate == Catch::Approx (kSampleRate));
}

//==============================================================================
// 5. json-schema — compressionFamilyToJSON emits the documented schema
//==============================================================================

TEST_CASE ("compressionFamilyToJSON emits the compression_family schema",
           "[compression-family][json-schema]")
{
    CompressionFamily::FamilyResult result;
    result.levelsDB = { -20.0, -10.0 };
    result.speeds = { 1.0 };

    for (int i = 0; i < 2; ++i)
    {
        CompressionFamily::FamilyEntry entry;
        entry.inputLevelDB = (i == 0) ? -20.0 : -10.0;
        entry.speed = 1.0;
        entry.curve.curve.push_back ({ -24.0, -25.5, -1.5 });
        entry.curve.fitted.ratio = 4.0;
        entry.curve.fitted.thresholdDB = -30.0;
        entry.curve.fitted.kneeDB = 3.0;
        entry.gr.sampleRate = kSampleRate;
        entry.gr.timeline.push_back ({ 0.0, 0.0 });
        entry.gr.timeline.push_back ({ 0.5, -6.0 });
        entry.gr.numPoints = 2;
        entry.tau.tauAttackSec = 0.009;
        entry.tau.tauReleaseSec = 0.05;
        entry.tau.valid = true;
        entry.valid = true;
        result.entries.push_back (entry);
    }

    Export::Context ctx;
    ctx.pluginName = "UnitTest";
    ctx.classId = "com.example.family";
    ctx.latencySamples = 0;
    ctx.sampleRate = kSampleRate;
    ctx.blockSize = kBlockSize;
    ctx.paramSnapshot = "{\"threshold\": -30.0}";
    ctx.source.type = "dynamic";

    const auto json = Export::compressionFamilyToJSON (result, ctx);
    const auto parsed = juce::JSON::parse (json);

    REQUIRE (! parsed.isUndefined());
    REQUIRE (parsed["type"].toString() == "compression_family");

    // Context block reuses the shared measurement-context fields.
    REQUIRE (parsed["context"]["plugin"].toString() == ctx.pluginName);
    REQUIRE (parsed["context"]["class_id"].toString() == ctx.classId);
    REQUIRE (parsed["context"]["sample_rate"].equals (kSampleRate));
    REQUIRE (parsed["context"]["measurement"]["block_size"].equals (kBlockSize));
    REQUIRE (parsed["context"]["parameter_snapshot"]["threshold"].equals (-30.0));
    REQUIRE (parsed["context"]["source"]["type"].toString() == "dynamic");

    REQUIRE (parsed["family"].size() == 2);
    for (int i = 0; i < 2; ++i)
    {
        INFO ("family entry " << i);
        REQUIRE (static_cast<double> (parsed["family"][i]["input_level_db"])
                 == Catch::Approx (result.entries[i].inputLevelDB));
        REQUIRE (static_cast<double> (parsed["family"][i]["speed"])
                 == Catch::Approx (result.entries[i].speed));
        REQUIRE (parsed["family"][i]["curve"].size() == 1);
        REQUIRE (parsed["family"][i]["gr"]["timeline"].size() == 2);
        REQUIRE (static_cast<double> (parsed["family"][i]["tau"]["attack_sec"])
                 == Catch::Approx (result.entries[i].tau.tauAttackSec));
        REQUIRE (static_cast<double> (parsed["family"][i]["tau"]["release_sec"])
                 == Catch::Approx (result.entries[i].tau.tauReleaseSec));
    }
}

//==============================================================================
// 6. cancel — cancel() from another thread stops the grid, partial entries
//==============================================================================

TEST_CASE ("CompressionFamily: cancel from another thread stops the grid",
           "[compression-family][cancel]")
{
    juce::MessageManager::getInstance();

    TestCompressorPlugin plugin;
    MeasurementSession session;
    configureCompressor (plugin);
    session.setPluginInstance (&plugin);
    session.setSampleRate (kSampleRate);
    session.setBlockSize (kBlockSize);

    std::atomic<bool> firstRoundDone { false };
    CompressionFamily::FamilyResult result;
    bool runThrew = false;

    // ---- Act ----
    // Run the 6-cell grid on a worker thread; cancel from this ("main")
    // thread once the first cell has completed.
    std::thread worker ([&]
    {
        try
        {
            result = CompressionFamily::measure (&plugin, &session,
                                                 { -20.0, -10.0 }, { 0.5, 1.0, 2.0 },
                                                 [&] (int, int) { firstRoundDone.store (true); });
        }
        catch (...)
        {
            runThrew = true;
        }
    });

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds (60);
    while (! firstRoundDone.load() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for (std::chrono::milliseconds (5));
    REQUIRE (firstRoundDone.load());

    CompressionFamily::cancel();
    worker.join();

    // ---- Assert ----
    REQUIRE_FALSE (runThrew);
    REQUIRE (result.cancelled);
    REQUIRE_FALSE (result.entries.empty());
    REQUIRE (result.entries.size() < 6);
}
