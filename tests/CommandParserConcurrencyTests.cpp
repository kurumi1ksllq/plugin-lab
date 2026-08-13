/**
 * CommandParser concurrency tests (issue #33): plugin load/unload vs an
 * in-flight measurement, and the plugin/session pointer swap vs worker
 * commands.
 *
 * D1 (use-after-free: plugin destroyed under a running measurement):
 *   The host refuses plugin load/unload while a measurement run is in
 *   flight — the sweep dereferences the plugin on the message thread and
 *   yields the message loop per block, so load/unload events CAN interleave
 *   mid-job. The guard condition is MeasurementSession::isRunning() (a
 *   wrapper over SweepRunner's flag; the refusal itself lives in Main.cpp,
 *   which is not compiled into the test target). These tests verify the
 *   seam the guard relies on: isRunning() is asserted for the whole
 *   duration of a run and cleared only after it finishes.
 *
 * D2 (data race: raw plugin pointer written on the message thread,
 *   dereferenced on the worker thread):
 *   The pointer swap (setPluginInstance/setSession) and every worker-side
 *   dereference in handleCommand are serialized by a mutex; the host
 *   nulls the pointer BEFORE destroying the instance. The stress test
 *   exercises the swap against concurrent setParam/getParams commands.
 *
 * Thread-safety note for the seam test: session.isRunning() is written
 * once by the runner thread before the first processBlock (and not
 * rewritten until the run ends), and the test observes it only after
 * synchronizing on the atomic `entered` flag — practically race-free, and
 * the production guards read it on the message thread (the thread that runs
 * the measurement), so they are formally race-free.
 */
#include <catch2/catch_test_macros.hpp>

#include "TestPlugin.h"
#include "../source/ipc/CommandParser.h"
#include "../source/ipc/Protocol.h"
#include "../source/capture/MeasurementSession.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>
#include <vector>

/** Ensure the JUCE MessageManager exists (auto-created in console apps). */
static void ensureMessageManager()
{
    juce::MessageManager::getInstance();
}

//==============================================================================
// 1. D1 seam: MeasurementSession::isRunning() stays asserted while a
//    measurement run executes (the condition the host's load/unload
//    refusal checks).
//==============================================================================

TEST_CASE ("MeasurementSession: isRunning() is asserted while the sweep executes (issue #33 D1)",
           "[commandparser][concurrency][issue33]")
{
    // ---- Arrange ----
    auto plugin = std::make_unique<TestPlugin>();
    plugin->setGain (1.0);

    MeasurementSession session;
    session.setPluginInstance (plugin.get());
    session.setSampleRate (48000.0);
    session.setBlockSize (512);
    session.setMeasurementType (MeasurementSession::Type::frequencyResponse);

    // Block the first processBlock so the run is observably in flight.
    std::atomic<bool> entered { false };
    std::atomic<bool> release { false };
    plugin->setBlockOnProcess (&entered, &release);

    REQUIRE_FALSE (session.isRunning());

    // ---- Act ----
    // Run on a worker thread (SweepRunner skips the message-loop yield on
    // non-message threads, so the test thread stays free to release the
    // plugin). Same pattern as SweepRunnerTests cancellation tests.
    bool runOk = false;
    std::thread worker ([&] { runOk = session.run(); });

    // Wait for the sweep to be observably in flight.
    for (int i = 0; i < 500 && ! entered.load(); ++i)
        std::this_thread::sleep_for (std::chrono::milliseconds (2));
    REQUIRE (entered.load());

    // ---- Assert ----
    // The run is mid-sweep (blocked inside processBlock): the guard
    // condition must be asserted — this is exactly when the host refuses
    // plugin load/unload.
    REQUIRE (session.isRunning());

    // Let the sweep finish; the flag must clear with it.
    release.store (true);
    worker.join();

    REQUIRE (runOk);
    REQUIRE_FALSE (session.isRunning());

    plugin->setBlockOnProcess (nullptr, nullptr);
}

//==============================================================================
// 2. D2: the plugin pointer swap is synchronized against worker commands
//    (stress). The swap thread emulates the host's unload ordering —
//    setPluginInstance(nullptr) FIRST, destroy the instance AFTER.
//==============================================================================

TEST_CASE ("CommandParser: plugin pointer swap is synchronized against worker commands (stress, issue #33 D2)",
           "[commandparser][concurrency][issue33]")
{
    // ---- Arrange ----
    ensureMessageManager();

    CommandParser parser;
    constexpr int kPluginCount = 3;
    constexpr int kIterations  = 2000;

    std::vector<std::unique_ptr<TestPlugin>> instances;
    for (int i = 0; i < kPluginCount; ++i)
    {
        instances.push_back (std::make_unique<TestPlugin>());
        instances.back()->setGain (1.0);
    }
    parser.setPluginInstance (instances[0].get());

    std::atomic<bool> stop { false };
    std::atomic<int> unexpectedResponses { 0 };

    // Worker (production role: the IPC worker thread): issue fast plugin
    // commands while the main thread (production role: the message thread)
    // swaps the pointer.
    std::thread worker ([&]
    {
        int i = 0;
        while (! stop.load())
        {
            const auto response = parser.handleCommand (
                (i % 2 == 0) ? R"({"cmd":"setParam","name":"Gain","value":0.5})"
                             : R"({"cmd":"getParams"})");

            // A torn read or a dereference of a destroyed instance manifests
            // as a crash; a sane parser answers ok (live plugin) or a clean
            // "no plugin loaded" (swap in progress). Anything else is a
            // synchronization regression.
            if (! (response.contains ("\"ok\":true")
                   || response.contains ("no plugin loaded")))
                unexpectedResponses.fetch_add (1);
            ++i;
        }
    });

    // ---- Act ----
    for (int i = 0; i < kIterations; ++i)
    {
        const size_t idx = static_cast<size_t> (i % kPluginCount);

        // Unload: null the pointer first (under the parser's lock), then
        // destroy the instance a worker may have observed before the swap.
        parser.setPluginInstance (nullptr);
        instances[idx] = std::make_unique<TestPlugin>();   // destroys the old instance
        instances[idx]->setGain (1.0);
        parser.setPluginInstance (instances[idx].get());
    }

    stop.store (true);
    worker.join();

    // ---- Assert ----
    REQUIRE (unexpectedResponses.load() == 0);
}
