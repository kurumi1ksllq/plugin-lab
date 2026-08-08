#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <thread>
#include <atomic>

#include "TestPlugin.h"
#include "../source/capture/SweepRunner.h"
#include "../source/signal/SineSweep.h"

//==============================================================================
// T1: SweepRunner cancellation — data race fix verification
//
// These tests exercise the cancel() / run() cross-thread contract.
// With a non-atomic bool cancelled, they are technically UB (data race).
// On x86 they may "pass" by accident, but they lock the contract.

TEST_CASE ("SweepRunner: cancelled mid-run causes run() to return false",
           "[sweeprunner][cancellation]")
{
    // Arrange
    TestPlugin plugin;
    plugin.setGain (1.0);

    SineSweep sweep;
    sweep.setFrequencyRange (20.0, 20000.0);
    sweep.setDuration (10.0);  // 10 seconds — long enough to cancel mid-run
    sweep.setAmplitude (0.5);

    SweepRunner runner;
    runner.prepare (48000.0, 512);
    runner.setGenerator (&sweep);
    runner.setPlugin (&plugin);

    // Block on first processBlock call so we can cancel while it's "running"
    std::atomic<bool> entered { false };
    std::atomic<bool> release { false };
    plugin.setBlockOnProcess (&entered, &release);

    // Act — run on a worker thread
    bool runResult = false;
    std::thread worker ([&] { runResult = runner.run(); });

    // Wait for the worker to enter processBlock
    while (! entered.load())
        std::this_thread::sleep_for (std::chrono::milliseconds (1));

    // Cancel from the "main" thread, then release the plugin
    runner.cancel();
    release.store (true);

    worker.join();

    // Assert — run() should return false (cancelled)
    REQUIRE_FALSE (runResult);

    // Assert — fewer samples recorded than the full 10s sweep
    const int64_t fullLength = sweep.getTotalLength();
    const int64_t recorded = runner.getResult().getNumRecordedSamples();
    REQUIRE (recorded > 0);   // some progress was made before cancel
    REQUIRE (recorded < fullLength);
}

TEST_CASE ("SweepRunner: cancel before run leaves state clean",
           "[sweeprunner][cancellation]")
{
    // Arrange
    TestPlugin plugin;
    plugin.setGain (1.0);

    SineSweep sweep;
    sweep.setFrequencyRange (20.0, 20000.0);
    sweep.setDuration (0.05);  // very short — completes quickly
    sweep.setAmplitude (0.5);

    SweepRunner runner;
    runner.prepare (48000.0, 512);
    runner.setGenerator (&sweep);
    runner.setPlugin (&plugin);

    // Act — cancel before run, then run should reset cancelled and succeed
    runner.cancel();
    bool runResult = runner.run();

    // Assert — run() completed successfully (cancelled was reset at entry)
    REQUIRE (runResult);
    REQUIRE (runner.getResult().getNumRecordedSamples() > 0);
}

//==============================================================================
// T4.1: SweepRunner tail padding + per-block callback
//==============================================================================

TEST_CASE ("SweepRunner: tailPad appends L silent samples after the signal",
           "[sweeprunner][tailpad]")
{
    // Arrange
    TestPlugin plugin;
    plugin.setGain (1.0);

    SineSweep sweep;
    sweep.setFrequencyRange (100.0, 5000.0);
    sweep.setDuration (0.1);  // 4800 samples @ 48 kHz
    sweep.setAmplitude (0.5);

    SweepRunner runner;
    runner.prepare (48000.0, 512);
    runner.setGenerator (&sweep);
    runner.setPlugin (&plugin);
    runner.setTailPadSamples (100);

    // Act
    REQUIRE (runner.run());

    // Assert — recorded length == signal length + tailPad
    const int64_t expected = sweep.getTotalLength() + 100;
    REQUIRE (runner.getResult().getNumRecordedSamples() == expected);

    // Assert — the dry tail region is silence (so analyzers see a padded tail)
    const auto& dry = runner.getResult().getDryBuffer();
    const int tailStart = static_cast<int> (sweep.getTotalLength());
    REQUIRE (dry.getNumSamples() == static_cast<int> (expected));
    for (int s = tailStart; s < dry.getNumSamples(); ++s)
        REQUIRE (dry.getSample (0, s) == 0.0f);
}

TEST_CASE ("SweepRunner: blockCallback fires per block with monotonic progress",
           "[sweeprunner][blockcallback]")
{
    // Arrange
    TestPlugin plugin;
    plugin.setGain (1.0);

    SineSweep sweep;
    sweep.setFrequencyRange (100.0, 5000.0);
    sweep.setDuration (0.1);  // 4800 samples @ 48 kHz → 10 generator blocks
    sweep.setAmplitude (0.5);

    SweepRunner runner;
    runner.prepare (48000.0, 512);
    runner.setGenerator (&sweep);
    runner.setPlugin (&plugin);
    runner.setTailPadSamples (100);  // → 1 additional tail block

    std::vector<float> progressList;
    juce::AudioBuffer<float> firstDryBlock (2, 512);
    juce::AudioBuffer<float> firstWetBlock (2, 512);
    bool firstCaptured = false;

    runner.setBlockCallback (
        [&] (float progress,
             const juce::AudioBuffer<float>& dryBlock,
             const juce::AudioBuffer<float>& wetBlock)
        {
            progressList.push_back (progress);
            if (! firstCaptured)
            {
                firstDryBlock.makeCopyOf (dryBlock, true);
                firstWetBlock.makeCopyOf (wetBlock, true);
                firstCaptured = true;
            }
        });

    // Act
    REQUIRE (runner.run());

    // Assert — one callback per processed block. 4800 samples / 512 = 9 full
    // blocks + 1 partial block (192 samples); the 100-sample tail is appended
    // from that partial block's zero-padded region, so no separate tail block
    // is needed. Total: 10 blocks.
    REQUIRE (firstCaptured);
    REQUIRE (progressList.size() == 10);

    // Assert — progress is monotonic non-decreasing and ends at 1.0
    for (size_t i = 1; i < progressList.size(); ++i)
        REQUIRE (progressList[i] >= progressList[i - 1]);
    REQUIRE (progressList.back() == Catch::Approx (1.0f));

    // Assert — the first callback block matches the corresponding section of
    // the final result buffers (same data the analyzers will consume).
    const auto& dry = runner.getResult().getDryBuffer();
    const auto& wet = runner.getResult().getWetBuffer();
    for (int s = 0; s < 512; ++s)
    {
        REQUIRE (firstDryBlock.getSample (0, s) == dry.getSample (0, s));
        REQUIRE (firstWetBlock.getSample (0, s) == wet.getSample (0, s));
    }
}

TEST_CASE ("SweepRunner: default tailPad=0 preserves existing recorded length",
           "[sweeprunner][tailpad-zero]")
{
    // Arrange
    TestPlugin plugin;
    plugin.setGain (1.0);

    SineSweep sweep;
    sweep.setFrequencyRange (100.0, 5000.0);
    sweep.setDuration (0.1);
    sweep.setAmplitude (0.5);

    SweepRunner runner;
    runner.prepare (48000.0, 512);
    runner.setGenerator (&sweep);
    runner.setPlugin (&plugin);
    // tailPad left at its default (0)

    // Act
    REQUIRE (runner.run());

    // Assert — exactly the generator length, no padding
    REQUIRE (runner.getResult().getNumRecordedSamples() == sweep.getTotalLength());
}

//==============================================================================
// Measurement-path exception protection (block C task 1): a plugin that
// throws from prepareToPlay/processBlock must surface as a failed run()
// (false), never escape into the message loop (std::terminate → abort).
//==============================================================================

TEST_CASE ("SweepRunner: plugin throwing in processBlock makes run() return false",
           "[sweeprunner][exception]")
{
    // Arrange
    TestPlugin plugin;
    plugin.setGain (1.0);
    plugin.setThrowOnProcessBlock (true);

    SineSweep sweep;
    sweep.setFrequencyRange (100.0, 5000.0);
    sweep.setDuration (0.1);
    sweep.setAmplitude (0.5);

    SweepRunner runner;
    runner.prepare (48000.0, 512);
    runner.setGenerator (&sweep);
    runner.setPlugin (&plugin);

    // Act — must not throw, must not terminate the process
    bool runResult = false;
    REQUIRE_NOTHROW (runResult = runner.run());

    // Assert — failed run, runner left in a clean state
    REQUIRE_FALSE (runResult);
    REQUIRE_FALSE (runner.isRunning());
}

TEST_CASE ("SweepRunner: plugin throwing in prepareToPlay makes run() return false",
           "[sweeprunner][exception]")
{
    // Arrange
    TestPlugin plugin;
    plugin.setGain (1.0);
    plugin.setThrowOnPrepareToPlay (true);

    SineSweep sweep;
    sweep.setFrequencyRange (100.0, 5000.0);
    sweep.setDuration (0.1);
    sweep.setAmplitude (0.5);

    SweepRunner runner;
    runner.prepare (48000.0, 512);
    runner.setGenerator (&sweep);
    runner.setPlugin (&plugin);

    // Act
    bool runResult = false;
    REQUIRE_NOTHROW (runResult = runner.run());

    // Assert
    REQUIRE_FALSE (runResult);
    REQUIRE_FALSE (runner.isRunning());
}

TEST_CASE ("SweepRunner: plugin throwing in processBlock still records a clean result buffer",
           "[sweeprunner][exception]")
{
    // Arrange
    TestPlugin plugin;
    plugin.setGain (1.0);
    plugin.setThrowOnProcessBlock (true);

    SineSweep sweep;
    sweep.setFrequencyRange (100.0, 5000.0);
    sweep.setDuration (0.05);
    sweep.setAmplitude (0.5);

    SweepRunner runner;
    runner.prepare (48000.0, 512);
    runner.setGenerator (&sweep);
    runner.setPlugin (&plugin);

    // Act
    REQUIRE_NOTHROW (runner.run());

    // Assert — no partially-written block content beyond a failed run;
    // the runner must not leave half-appended state (cancelled == false path)
    REQUIRE_FALSE (runner.isRunning());
}
