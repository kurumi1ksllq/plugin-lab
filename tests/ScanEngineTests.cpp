/**
 * ScanEngine unit tests (TDD: RED phase).
 *
 * Exercises the parameter-scanning engine:
 *   - [scanengine][gain-scan]      : 5-point gain sweep → one family entry per
 *                                    value, each with a frequency response whose
 *                                    peak magnitude ≈ 20*log10(gain)
 *   - [scanengine][params-restored]: all plugin parameters restored after scan
 *   - [scanengine][cancel]         : cancel() from another thread stops the
 *                                    scan mid-way and still restores parameters
 *   - [scanengine][latency-scan]   : latency parameter → per-round re-read of
 *                                    getLatencySamples() matches round(v*1000)
 *
 * Thread-safety note: the cancel test runs the scan on a std::thread (same
 * pattern as SweepRunnerTests); the ScanEngine must only touch the message
 * loop when it is actually running on the message thread.
 */
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <atomic>
#include <chrono>
#include <thread>

#include "TestPlugin.h"
#include "../source/scan/ScanEngine.h"
#include "../source/analysis/FreqResponse.h"
#include "../source/capture/MeasurementSession.h"

//==============================================================================
// Helpers
//==============================================================================

/** Ensure the JUCE MessageManager exists (auto-created in console apps). */
static void ensureMessageManager()
{
    juce::MessageManager::getInstance();
}

/** Find a parameter by ID, or nullptr. */
static juce::AudioProcessorParameter* findParam (juce::AudioPluginInstance& plugin,
                                                 const juce::String& id)
{
    for (auto* p : plugin.getParameters())
    {
        auto* hosted = dynamic_cast<juce::HostedAudioProcessorParameter*> (p);
        if (hosted != nullptr && hosted->getParameterID() == id)
            return p;
    }
    return nullptr;
}

/** Peak magnitude (dB) across the raw frequency response curve. */
static double peakMagnitudeDB (const FreqResponse::Result& r)
{
    double peak = -1.0e9;
    for (const auto& p : r.raw)
        peak = std::max (peak, p.magnitudeDB);
    return peak;
}

/** Snapshot of every parameter's normalized value. */
static std::vector<float> snapshotParamValues (juce::AudioPluginInstance& plugin)
{
    std::vector<float> values;
    values.reserve (static_cast<size_t> (plugin.getParameters().size()));
    for (auto* p : plugin.getParameters())
        values.push_back (p->getValue());
    return values;
}

/** Assert every parameter equals the given snapshot. */
static void requireParamsRestored (juce::AudioPluginInstance& plugin,
                                   const std::vector<float>& before)
{
    const auto after = snapshotParamValues (plugin);
    REQUIRE (after.size() == before.size());
    for (size_t i = 0; i < before.size(); ++i)
    {
        INFO ("parameter " << i << " not restored: "
              << after[i] << " != " << before[i]);
        REQUIRE (after[i] == Catch::Approx (before[i]).margin (0.001));
    }
}

//==============================================================================
// 1. Gain scan — one family entry per value, magnitude follows the gain
//==============================================================================

TEST_CASE ("ScanEngine: gain scan yields one entry per value with 20*log10(gain) magnitude",
           "[scanengine][gain-scan]")
{
    ensureMessageManager();

    // ---- Arrange ----
    auto plugin = std::make_unique<TestPlugin>();
    plugin->setGain (1.0);
    plugin->prepareToPlay (44100.0, 256);

    MeasurementSession session;
    session.setPluginInstance (plugin.get());
    session.setSampleRate (44100.0);
    session.setBlockSize (256);

    ScanEngine engine;
    engine.setPluginInstance (plugin.get());
    engine.setSession (&session);

    const std::vector<float> values = { 0.1f, 0.3f, 0.5f, 0.7f, 1.0f };

    // ---- Act ----
    int progressCalls = 0;
    auto result = engine.run ("gain", values,
                              MeasurementSession::Type::frequencyResponse,
                              [&] (int, int) { ++progressCalls; });

    // ---- Assert ----
    REQUIRE (result.paramId == juce::String ("gain"));
    REQUIRE (result.paramName == juce::String ("Gain"));
    REQUIRE (result.values.size() == values.size());
    REQUIRE (result.family.size() == values.size());
    REQUIRE (progressCalls == static_cast<int> (values.size()));
    REQUIRE_FALSE (result.cancelled);

    for (size_t i = 0; i < values.size(); ++i)
    {
        INFO ("round " << i << " gain=" << values[i]);

        const auto& entry = result.family[i];
        REQUIRE (entry.paramValue == Catch::Approx (static_cast<double> (values[i])).margin (0.001));
        REQUIRE_FALSE (entry.paramValueText.isEmpty());

        REQUIRE_FALSE (entry.freq.raw.empty());
        const double expectedDB = 20.0 * std::log10 (static_cast<double> (values[i]));
        REQUIRE (peakMagnitudeDB (entry.freq) == Catch::Approx (expectedDB).margin (0.5));
    }
}

//==============================================================================
// 2. Parameter restoration — scan must leave every parameter unchanged
//==============================================================================

TEST_CASE ("ScanEngine: restores every parameter after the scan",
           "[scanengine][params-restored]")
{
    ensureMessageManager();

    // ---- Arrange ----
    auto plugin = std::make_unique<TestPlugin>();
    plugin->prepareToPlay (48000.0, 512);

    // Move both parameters away from their defaults before the scan.
    auto* gain    = findParam (*plugin, "gain");
    auto* latency = findParam (*plugin, "latency");
    REQUIRE (gain != nullptr);
    REQUIRE (latency != nullptr);
    gain->setValue (0.42f);
    latency->setValue (0.5f);   // → 500 samples of plugin latency
    REQUIRE (plugin->getLatencySamples() == 500);

    const auto before = snapshotParamValues (*plugin);

    MeasurementSession session;
    session.setPluginInstance (plugin.get());
    session.setSampleRate (48000.0);
    session.setBlockSize (512);

    ScanEngine engine;
    engine.setPluginInstance (plugin.get());
    engine.setSession (&session);

    // compressionCurve = shortest built-in signal (~1.8 s), keeps the test fast.
    // ---- Act ----
    auto result = engine.run ("gain", { 0.3f, 0.7f },
                              MeasurementSession::Type::compressionCurve, {});

    // ---- Assert ----
    REQUIRE (result.family.size() == 2);
    REQUIRE_FALSE (result.cancelled);

    requireParamsRestored (*plugin, before);

    // The latency side-effect (0.5 → 500 samples) must be restored too.
    REQUIRE (plugin->getLatencySamples() == 500);
}

//==============================================================================
// 3. Cancellation — cancel() from another thread, parameters still restored
//==============================================================================

TEST_CASE ("ScanEngine: cancel() from another thread stops the scan and restores parameters",
           "[scanengine][cancel]")
{
    ensureMessageManager();

    // ---- Arrange ----
    auto plugin = std::make_unique<TestPlugin>();
    plugin->prepareToPlay (44100.0, 256);

    auto* gain = findParam (*plugin, "gain");
    REQUIRE (gain != nullptr);
    gain->setValue (0.42f);
    const auto before = snapshotParamValues (*plugin);

    MeasurementSession session;
    session.setPluginInstance (plugin.get());
    session.setSampleRate (44100.0);
    session.setBlockSize (256);

    ScanEngine engine;
    engine.setPluginInstance (plugin.get());
    engine.setSession (&session);

    std::atomic<bool> firstRoundDone { false };
    ScanEngine::ScanResult result;
    bool runThrew = false;

    // ---- Act ----
    // Scan on a worker thread; cancel from this ("main") thread once the
    // first round has completed.
    std::thread worker ([&]
    {
        try
        {
            result = engine.run ("gain", { 0.1f, 0.3f, 0.5f, 0.7f, 1.0f },
                                 MeasurementSession::Type::frequencyResponse,
                                 [&] (int, int) { firstRoundDone.store (true); });
        }
        catch (...)
        {
            runThrew = true;
        }
    });

    while (! firstRoundDone.load())
        std::this_thread::sleep_for (std::chrono::milliseconds (1));

    engine.cancel();

    worker.join();

    // ---- Assert ----
    REQUIRE_FALSE (runThrew);
    REQUIRE (result.cancelled);
    REQUIRE_FALSE (result.family.empty());
    REQUIRE (result.family.size() < 5);   // stopped before the last round

    requireParamsRestored (*plugin, before);
}

//==============================================================================
// 4. Latency scan — getLatencySamples() re-read after every round
//==============================================================================

TEST_CASE ("ScanEngine: latency scan re-reads getLatencySamples() each round",
           "[scanengine][latency-scan]")
{
    ensureMessageManager();

    // ---- Arrange ----
    auto plugin = std::make_unique<TestPlugin>();
    plugin->prepareToPlay (48000.0, 512);

    MeasurementSession session;
    session.setPluginInstance (plugin.get());
    session.setSampleRate (48000.0);
    session.setBlockSize (512);

    ScanEngine engine;
    engine.setPluginInstance (plugin.get());
    engine.setSession (&session);

    const std::vector<float> values = { 0.0f, 0.25f, 0.5f, 0.75f, 1.0f };

    // ---- Act ----
    auto result = engine.run ("latency", values,
                              MeasurementSession::Type::compressionCurve, {});

    // ---- Assert ----
    REQUIRE (result.paramId == juce::String ("latency"));
    REQUIRE (result.family.size() == values.size());
    REQUIRE_FALSE (result.cancelled);

    for (size_t i = 0; i < values.size(); ++i)
    {
        INFO ("round " << i << " v=" << values[i]);

        const auto& entry = result.family[i];
        REQUIRE (entry.paramValue == Catch::Approx (static_cast<double> (values[i])).margin (0.001));
        REQUIRE_FALSE (entry.paramValueText.isEmpty());
        REQUIRE (entry.latencySamples == juce::roundToInt (values[i] * 1000.0f));
    }

    // The scan must restore the latency parameter afterwards (default 0).
    REQUIRE (plugin->getLatencySamples() == 0);
}
