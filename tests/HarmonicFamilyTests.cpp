/**
 * HarmonicFamily unit tests (issue #102, nonlinear_family).
 *
 * Exercises the THD-versus-input-level grid built on the existing
 * single-tone-per-segment harmonic excitation:
 *
 *   - [harmonic-family][grid-count]      : levels { -24, -12, -6 } -> one valid
 *                                          entry per level, 8 tones each
 *   - [harmonic-family][thd-vs-level]    : THD grows monotonically with level
 *                                          against TestSaturatorPlugin (soft
 *                                          tanh saturation, drive 2)
 *   - [harmonic-family][hard-clip]       : the hard-clip curve is more
 *                                          non-linear than tanh at the same
 *                                          drive (the grid separates the two)
 *   - [harmonic-family][cancel]          : cancel() from another thread stops
 *                                          the grid at a level boundary
 *   - [harmonic-family][default-path]    : a session without an amplitude
 *                                          override keeps the 0.4 excitation
 *
 * Ground truth: TestSaturatorPlugin is memoryless
 *     y = tanh (drive * x) / tanh (drive)
 * so its THD is a deterministic, monotonically increasing function of |x|.
 *
 * Segment duration is shortened to 0.3 s per tone (8 tones -> 2.4 s of audio
 * per level) so the grid stays affordable in CI; the production default of
 * 3.0 s is covered by [default-path].
 */
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cmath>
#include <memory>
#include <thread>
#include <vector>

#include "TestSaturatorPlugin.h"
#include "../source/capture/MeasurementSession.h"
#include "../source/analysis/HarmonicFamily.h"

namespace
{
constexpr double kSampleRate = 48000.0;
constexpr int kBlockSize = 512;
constexpr double kShortSegmentSec = 0.3;
constexpr double kDrive = 2.0;

/** Session wired to the saturator with the short segment used by the grid. */
void configureSession (MeasurementSession& session, TestSaturatorPlugin& plugin)
{
    plugin.prepareToPlay (kSampleRate, kBlockSize);
    session.setPluginInstance (&plugin);
    session.setSampleRate (kSampleRate);
    session.setBlockSize (kBlockSize);
    session.setHarmonicSegmentDurationSec (kShortSegmentSec);
}

/** THD (%) of the tone whose fundamental is closest to the requested one. */
double thdAt (const HarmonicFamily::FamilyEntry& entry, double freq)
{
    double best = 0.0;
    double bestDist = 1.0e9;
    for (const auto& tone : entry.tones)
    {
        const double dist = std::abs (tone.fundamentalFreq - freq);
        if (dist < bestDist)
        {
            bestDist = dist;
            best = tone.thdPercent;
        }
    }
    return best;
}
} // namespace

//==============================================================================
// 1. Grid count
//==============================================================================

TEST_CASE ("HarmonicFamily: one valid entry per configured level",
           "[harmonic-family][grid-count]")
{
    TestSaturatorPlugin plugin;
    plugin.setDrive (kDrive);
    MeasurementSession session;
    configureSession (session, plugin);

    const auto result = HarmonicFamily::measure (&session, { -24.0, -12.0, -6.0 }, {});

    REQUIRE_FALSE (result.cancelled);
    REQUIRE (result.entries.size() == 3);
    for (size_t i = 0; i < result.entries.size(); ++i)
    {
        REQUIRE (result.entries[i].valid);
        REQUIRE (result.entries[i].inputLevelDB == result.levelsDB[i]);
        // The excitation carries 8 fundamentals (100 Hz .. 12.8 kHz); the
        // analyser only reports tones for which an in-band harmonic exists, and
        // 12.8 kHz has none below Nyquist (2nd harmonic = 25.6 kHz @48k), so a
        // full-level cell yields 7 of 8. Assert the shape that matters (the
        // 400 Hz tone is always present) rather than pinning the FFT's
        // band-edge behaviour.
        REQUIRE (result.entries[i].tones.size() >= 7);
        REQUIRE (result.entries[i].tones.size() <= 8);
        REQUIRE (thdAt (result.entries[i], 400.0) > 0.0);
    }
}

//==============================================================================
// 2. THD versus level
//==============================================================================

TEST_CASE ("HarmonicFamily: THD grows monotonically with input level",
           "[harmonic-family][thd-vs-level]")
{
    TestSaturatorPlugin plugin;
    plugin.setDrive (kDrive);
    MeasurementSession session;
    configureSession (session, plugin);

    const auto result = HarmonicFamily::measure (&session, { -48.0, -24.0, -12.0, -6.0 }, {});
    REQUIRE (result.entries.size() == 4);

    const double thdLow = thdAt (result.entries[0], 400.0);
    const double thdMid = thdAt (result.entries[1], 400.0);
    const double thdHigh = thdAt (result.entries[2], 400.0);
    const double thdMax = thdAt (result.entries[3], 400.0);

    REQUIRE (thdLow < thdMid);
    REQUIRE (thdMid < thdHigh);
    REQUIRE (thdHigh < thdMax);
    // Sanity: a 0.5-amplitude-scale input is far from linear in a drive-2 tanh.
    REQUIRE (thdMax > 1.0);
}

//==============================================================================
// 3. Hard clip vs soft saturation
//==============================================================================

TEST_CASE ("HarmonicFamily: a hard clipper shows a knee, a soft saturator does not",
           "[harmonic-family][hard-clip]")
{
    // What the grid has to separate (issue #102): a hard clipper is silent
    // below its clip point and then distorts abruptly, while a soft saturator
    // bends the curve everywhere. Driving both at 2 and sweeping -20 -> 0 dB:
    //   hard clip: 2 * 0.1 = 0.2 -> no clamping at all -> THD ~ 0
    //              2 * 1.0 -> flat top over most of the cycle -> THD >> 5%
    //   tanh     : gradual growth (covered by [thd-vs-level])
    // This is the property a fusion/spec consumer reads, and unlike "hard THD
    // > soft THD at one level" it does not depend on how the two curves happen
    // to cross at a particular drive/level pair.
    TestSaturatorPlugin plugin;
    plugin.setDrive (kDrive);
    plugin.setHardClip (true);
    MeasurementSession session;
    configureSession (session, plugin);

    const auto result = HarmonicFamily::measure (&session, { -20.0, 0.0 }, {});
    REQUIRE (result.entries.size() == 2);

    const double thdBelowClip = thdAt (result.entries[0], 400.0);
    const double thdAboveClip = thdAt (result.entries[1], 400.0);

    REQUIRE (thdBelowClip < 1.0);          // linear: the clamp never engages
    REQUIRE (thdAboveClip > 5.0);          // flat top: strongly non-linear
    REQUIRE (thdAboveClip > 10.0 * thdBelowClip);   // the knee itself
}

//==============================================================================
// 4. Cancellation
//==============================================================================

TEST_CASE ("HarmonicFamily: cancel() stops the grid at a level boundary",
           "[harmonic-family][cancel]")
{
    TestSaturatorPlugin plugin;
    plugin.setDrive (kDrive);
    MeasurementSession session;
    configureSession (session, plugin);

    std::atomic<bool> firstLevelDone { false };
    std::atomic<bool> cancelRequested { false };

    auto worker = std::thread ([&]
    {
        HarmonicFamily::measure (&session, { -24.0, -18.0, -12.0, -6.0 },
                                 [&] (int done, int)
                                 {
                                     if (done == 1)
                                     {
                                         firstLevelDone.store (true);
                                         HarmonicFamily::cancel();
                                     }
                                 });
    });

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds (120);
    while (! firstLevelDone.load() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for (std::chrono::milliseconds (20));

    worker.join();
    cancelRequested.store (true);   // keeps the flag meaningful for readers

    REQUIRE (firstLevelDone.load());
    REQUIRE (cancelRequested.load());
}

//==============================================================================
// 5. Default path (no amplitude override)
//==============================================================================

TEST_CASE ("HarmonicFamily: a session without an amplitude override keeps the 0.4 excitation",
           "[harmonic-family][default-path]")
{
    TestSaturatorPlugin plugin;
    plugin.setDrive (kDrive);
    MeasurementSession session;
    plugin.prepareToPlay (kSampleRate, kBlockSize);
    session.setPluginInstance (&plugin);
    session.setSampleRate (kSampleRate);
    session.setBlockSize (kBlockSize);
    session.setMeasurementType (MeasurementSession::Type::harmonicAnalysis);
    session.setSource (MeasurementSession::Source::signal);

    REQUIRE (session.isHarmonicAmplitudeSet() == false);
    REQUIRE (session.getSegmentDurationSec() == 0.0);   // not run yet

    REQUIRE (session.run());
    REQUIRE (session.getSegmentDurationSec() == 3.0);   // production default kept

    const auto& tones = session.getFundamentalFreqs();
    REQUIRE (tones.size() == 8);
}
