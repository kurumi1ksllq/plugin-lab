#include <catch2/catch_test_macros.hpp>

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
