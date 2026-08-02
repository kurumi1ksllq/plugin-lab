/**
 * CommandParser unit tests (TDD: RED phase).
 *
 * Tests the IPC command pipeline: loadPlugin, setParam, getParams,
 * measure (with analysis + export + callback), stop, and unknown commands.
 *
 * Thread-safety note:
 *   The CommandParser runs on the IPC thread. The measurement complete
 *   callback is dispatched via MessageManager::callAsync to the message
 *   thread. In these tests, MessageManager is auto-initialised by JUCE;
 *   we call runDispatchLoopUntil() after each test that triggers a
 *   callAsync to ensure the callback fires before assertions.
 */
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "TestPlugin.h"
#include "../source/ipc/CommandParser.h"
#include "../source/ipc/Protocol.h"
#include "../source/capture/MeasurementSession.h"
#include "../source/analysis/FreqResponse.h"
#include "../source/analysis/Export.h"
#include "../source/host/PluginManager.h"

#include <atomic>

//==============================================================================
// Helpers
//==============================================================================

/** Ensure the JUCE MessageManager exists (auto-created in console apps). */
static void ensureMessageManager()
{
    juce::MessageManager::getInstance();
}

/** Flush any pending MessageManager callbacks queued via callAsync. */
static void flushMessageManager (int timeoutMs = 100)
{
    juce::MessageManager::getInstance()->runDispatchLoopUntil (timeoutMs);
}

//==============================================================================
// 1. Measure — happy path: analysis + export + callback
//==============================================================================

TEST_CASE ("CommandParser: measure performs frequency analysis, exports JSON, and fires callback",
           "[commandparser][measure]")
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
    session.setMeasurementType (MeasurementSession::Type::frequencyResponse);

    CommandParser parser;
    parser.setPluginInstance (plugin.get());
    parser.setSession (&session);

    std::atomic<bool> callbackFired { false };
    MeasurementResults capturedResult;
    parser.setMeasurementCompleteCallback ([&] (const MeasurementResults& r) {
        callbackFired.store (true);
        capturedResult = r;
    });

    const juce::String exportPath =
        juce::File::getCurrentWorkingDirectory()
            .getChildFile ("test_measure_output.json")
            .getFullPathName();

    // Clean up from previous runs
    juce::File (exportPath).deleteFile();

    // ---- Act ----
    // Build JSON manually with proper escaping of the path string
    const juce::String jsonCmd =
        juce::String (R"({"cmd":"measure","type":"frequency_response","path":)")  // note: no opening quote yet
        + juce::JSON::toString (exportPath)                                       // escapes backslashes etc.
        + "}";
    auto response = parser.handleCommand (jsonCmd);

    // Flush MessageManager to process the callAsync callback
    flushMessageManager (200);

    // ---- Assert ----
    // 1. Response must indicate success with samples and export path
    REQUIRE (response.contains ("\"ok\":true"));
    REQUIRE (response.contains ("\"samples\":"));
    REQUIRE (response.contains ("\"export_path\":"));

    // 2. The measurement complete callback must have been invoked
    REQUIRE (callbackFired.load());

    // 3. The callback result must identify the measurement type and contain
    //    frequency response data
    REQUIRE (capturedResult.type == MeasurementSession::Type::frequencyResponse);
    REQUIRE_FALSE (capturedResult.freq.raw.empty());
    REQUIRE (capturedResult.freq.raw[0].frequency > 0.0);
    REQUIRE (capturedResult.freq.sampleRate > 0.0);

    // 4. Export file must exist and contain valid JSON
    juce::File exportFile (exportPath);
    REQUIRE (exportFile.existsAsFile());
    auto exportedJson = juce::JSON::parse (exportFile.loadFileAsString());
    auto* exportObj = exportedJson.getDynamicObject();
    REQUIRE (exportObj != nullptr);
    REQUIRE (exportObj->hasProperty ("type"));
    REQUIRE (exportObj->getProperty ("type").toString() == "frequency_response");

    // Cleanup
    exportFile.deleteFile();
}

//==============================================================================
// 1b. Measure — harmonic analysis: analysis + export + callback
//==============================================================================

TEST_CASE ("CommandParser: measure harmonic analysis exports JSON and fires callback",
           "[commandparser][measure]")
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
    session.setMeasurementType (MeasurementSession::Type::harmonicAnalysis);

    CommandParser parser;
    parser.setPluginInstance (plugin.get());
    parser.setSession (&session);

    std::atomic<bool> callbackFired { false };
    MeasurementResults capturedResult;
    parser.setMeasurementCompleteCallback ([&] (const MeasurementResults& r) {
        callbackFired.store (true);
        capturedResult = r;
    });

    const juce::String exportPath =
        juce::File::getCurrentWorkingDirectory()
            .getChildFile ("test_measure_harmonic.json")
            .getFullPathName();

    // Clean up from previous runs
    juce::File (exportPath).deleteFile();

    // ---- Act ----
    const juce::String jsonCmd =
        juce::String (R"({"cmd":"measure","type":"harmonic","path":)")
        + juce::JSON::toString (exportPath)
        + "}";
    auto response = parser.handleCommand (jsonCmd);

    // Flush MessageManager to process the callAsync callback
    flushMessageManager (200);

    // ---- Assert ----
    // 1. Response must indicate success with samples and export path
    REQUIRE (response.contains ("\"ok\":true"));
    REQUIRE (response.contains ("\"samples\":"));
    REQUIRE (response.contains ("\"export_path\":"));
    REQUIRE_FALSE (response.contains ("\"error\""));

    // 2. The measurement complete callback must have been invoked with the
    //    harmonic analysis result (tones detected from the multi-tone signal)
    REQUIRE (callbackFired.load());
    REQUIRE (capturedResult.type == MeasurementSession::Type::harmonicAnalysis);
    REQUIRE_FALSE (capturedResult.harmonic.tones.empty());
    // The analysis must have actually detected the fundamentals (real signal
    // energy), not emitted empty -120 dB placeholder tones from silence.
    REQUIRE (capturedResult.harmonic.tones[0].fundamentalDB > -100.0);

    // 3. Export file must exist, be parseable, and carry the harmonic payload
    juce::File exportFile (exportPath);
    REQUIRE (exportFile.existsAsFile());
    auto exportedJson = juce::JSON::parse (exportFile.loadFileAsString());
    auto* exportObj = exportedJson.getDynamicObject();
    REQUIRE (exportObj != nullptr);
    REQUIRE (exportObj->getProperty ("type").toString() == "harmonic_analysis");
    REQUIRE (exportObj->hasProperty ("class_id"));
    REQUIRE (exportObj->hasProperty ("tones"));
    REQUIRE (exportObj->getProperty ("tones").size() > 0);

    // Cleanup
    exportFile.deleteFile();
}

//==============================================================================
// 1c. Measure — compression curve: analysis + export + callback
//==============================================================================

TEST_CASE ("CommandParser: measure compression curve exports JSON and fires callback",
           "[commandparser][measure]")
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
    session.setMeasurementType (MeasurementSession::Type::compressionCurve);

    CommandParser parser;
    parser.setPluginInstance (plugin.get());
    parser.setSession (&session);

    std::atomic<bool> callbackFired { false };
    MeasurementResults capturedResult;
    parser.setMeasurementCompleteCallback ([&] (const MeasurementResults& r) {
        callbackFired.store (true);
        capturedResult = r;
    });

    const juce::String exportPath =
        juce::File::getCurrentWorkingDirectory()
            .getChildFile ("test_measure_compression.json")
            .getFullPathName();

    // Clean up from previous runs
    juce::File (exportPath).deleteFile();

    // ---- Act ----
    const juce::String jsonCmd =
        juce::String (R"({"cmd":"measure","type":"compression","path":)")
        + juce::JSON::toString (exportPath)
        + "}";
    auto response = parser.handleCommand (jsonCmd);

    // Flush MessageManager to process the callAsync callback
    flushMessageManager (200);

    // ---- Assert ----
    // 1. Response must indicate success with samples and export path
    REQUIRE (response.contains ("\"ok\":true"));
    REQUIRE (response.contains ("\"samples\":"));
    REQUIRE (response.contains ("\"export_path\":"));
    REQUIRE_FALSE (response.contains ("\"error\""));

    // 2. The measurement complete callback must have been invoked with the
    //    compression curve result (9 burst levels measured)
    REQUIRE (callbackFired.load());
    REQUIRE (capturedResult.type == MeasurementSession::Type::compressionCurve);
    REQUIRE_FALSE (capturedResult.compression.curve.empty());
    // The analysis must have measured actual burst energy: most windows must
    // land on tone bursts instead of empty buffer regions.
    int realPoints = 0;
    for (const auto& p : capturedResult.compression.curve)
        if (p.inputDB > -100.0)
            ++realPoints;
    REQUIRE (realPoints >= 8);

    // 3. Export file must exist, be parseable, and carry the curve + fitted data
    juce::File exportFile (exportPath);
    REQUIRE (exportFile.existsAsFile());
    auto exportedJson = juce::JSON::parse (exportFile.loadFileAsString());
    auto* exportObj = exportedJson.getDynamicObject();
    REQUIRE (exportObj != nullptr);
    REQUIRE (exportObj->getProperty ("type").toString() == "compression_curve");
    REQUIRE (exportObj->hasProperty ("class_id"));
    REQUIRE (exportObj->hasProperty ("curve"));
    REQUIRE (exportObj->getProperty ("curve").size() > 0);
    REQUIRE (exportObj->hasProperty ("fitted"));
    // Fitted params must be finite, parseable numbers (never "inf").
    REQUIRE (exportedJson["fitted"]["ratio"].isDouble());
    REQUIRE (static_cast<double> (exportedJson["fitted"]["ratio"]) >= 1.0);
    REQUIRE (exportedJson["fitted"]["threshold_db"].isDouble());
    REQUIRE (exportedJson["fitted"]["knee_db"].isDouble());

    // Cleanup
    exportFile.deleteFile();
}

//==============================================================================
// 2. Measure — error: no session or plugin
//==============================================================================

TEST_CASE ("CommandParser: measure fails when session is null", "[commandparser][measure]")
{
    CommandParser parser;
    auto response = parser.handleCommand (R"({"cmd":"measure"})");
    REQUIRE (response.contains ("\"ok\":false"));
    REQUIRE (response.contains ("\"error\""));
}

TEST_CASE ("CommandParser: measure fails when plugin is null", "[commandparser][measure]")
{
    ensureMessageManager();
    MeasurementSession session;
    CommandParser parser;
    parser.setSession (&session);
    // plugin NOT set

    auto response = parser.handleCommand (R"({"cmd":"measure"})");
    REQUIRE (response.contains ("\"ok\":false"));
    REQUIRE (response.contains ("\"error\""));
}

//==============================================================================
// 3. loadPlugin — callback fires with matching PluginDescription
//==============================================================================

TEST_CASE ("CommandParser: loadPlugin fires callback with matching description",
           "[commandparser][loadPlugin]")
{
    ensureMessageManager();

    // ---- Arrange ----
    // Register a fake plugin in PluginManager's known list
    PluginManager pm;
    juce::PluginDescription desc;
    desc.name             = "TestVST3";
    desc.pluginFormatName = "VST3";
    desc.fileOrIdentifier = "TestVST3.vst3";
    desc.uniqueId         = 0xABCD1234;
    desc.numInputChannels  = 2;
    desc.numOutputChannels = 2;
    pm.getKnownPlugins().addType (desc);

    CommandParser parser;
    parser.setPluginManager (&pm);

    std::atomic<bool> callbackFired { false };
    juce::PluginDescription capturedDesc;
    parser.setLoadPluginCallback ([&] (const juce::PluginDescription& d) {
        callbackFired.store (true);
        capturedDesc = d;
    });

    // ---- Act ----
    auto response = parser.handleCommand (R"({"cmd":"loadPlugin","path":"TestVST3"})");

    // Flush MessageManager for the callAsync in loadPlugin
    flushMessageManager (200);

    // ---- Assert ----
    REQUIRE (response.contains ("\"ok\":true"));
    REQUIRE (response.contains ("\"name\":"));
    REQUIRE (callbackFired.load());
    REQUIRE (capturedDesc.name == "TestVST3");
    REQUIRE (capturedDesc.fileOrIdentifier == "TestVST3.vst3");
}

TEST_CASE ("CommandParser: loadPlugin returns error for unknown plugin", "[commandparser][loadPlugin]")
{
    PluginManager pm;
    CommandParser parser;
    parser.setPluginManager (&pm);

    auto response = parser.handleCommand (R"({"cmd":"loadPlugin","path":"nonexistent"})");
    REQUIRE (response.contains ("\"ok\":false"));
    REQUIRE (response.contains ("\"error\""));
}

TEST_CASE ("CommandParser: loadPlugin requires path parameter", "[commandparser][loadPlugin]")
{
    PluginManager pm;
    CommandParser parser;
    parser.setPluginManager (&pm);

    auto response = parser.handleCommand (R"({"cmd":"loadPlugin"})");
    REQUIRE (response.contains ("\"ok\":false"));
    REQUIRE (response.contains ("\"error\""));
}

//==============================================================================
// 4. setParam — sets plugin parameter
//==============================================================================

TEST_CASE ("CommandParser: setParam updates plugin parameter value", "[commandparser][setParam]")
{
    ensureMessageManager();

    TestPlugin plugin;
    CommandParser parser;
    parser.setPluginInstance (&plugin);

    // TestPlugin has a "Gain" parameter (index 0, default 1.0)
    auto response = parser.handleCommand (R"({"cmd":"setParam","name":"Gain","value":0.5})");
    REQUIRE (response.contains ("\"ok\":true"));

    // Verify the parameter was actually changed
    REQUIRE (plugin.getParameters()[0]->getValue() == Catch::Approx (0.5f));
}

TEST_CASE ("CommandParser: setParam fails when no plugin loaded", "[commandparser][setParam]")
{
    CommandParser parser;
    auto response = parser.handleCommand (R"({"cmd":"setParam","name":"Gain","value":0.5})");
    REQUIRE (response.contains ("\"ok\":false"));
    REQUIRE (response.contains ("\"error\""));
}

TEST_CASE ("CommandParser: setParam returns error for unknown parameter", "[commandparser][setParam]")
{
    TestPlugin plugin;
    CommandParser parser;
    parser.setPluginInstance (&plugin);

    auto response = parser.handleCommand (R"({"cmd":"setParam","name":"nonexistent","value":0.5})");
    REQUIRE (response.contains ("\"ok\":false"));
    REQUIRE (response.contains ("\"error\""));
}

//==============================================================================
// 5. getParams — returns parameter list
//==============================================================================

TEST_CASE ("CommandParser: getParams returns plugin parameter list", "[commandparser][getParams]")
{
    TestPlugin plugin;
    CommandParser parser;
    parser.setPluginInstance (&plugin);

    auto response = parser.handleCommand (R"({"cmd":"getParams"})");
    REQUIRE (response.contains ("\"ok\":true"));
    REQUIRE (response.contains ("\"params\""));
    REQUIRE (response.contains ("\"Gain\""));   // TestPlugin's only parameter
    REQUIRE (response.contains ("\"index\":0"));
}

TEST_CASE ("CommandParser: getParams fails when no plugin loaded", "[commandparser][getParams]")
{
    CommandParser parser;
    auto response = parser.handleCommand (R"({"cmd":"getParams"})");
    REQUIRE (response.contains ("\"ok\":false"));
    REQUIRE (response.contains ("\"error\""));
}

//==============================================================================
// 6. Unknown command
//==============================================================================

TEST_CASE ("CommandParser: unknown command returns error", "[commandparser][unknown]")
{
    CommandParser parser;
    auto response = parser.handleCommand (R"({"cmd":"invalidCommand"})");
    REQUIRE (response.contains ("\"ok\":false"));
    REQUIRE (response.contains ("\"error\""));
}

TEST_CASE ("CommandParser: invalid JSON returns error", "[commandparser][error]")
{
    CommandParser parser;
    auto response = parser.handleCommand ("not json at all");
    REQUIRE (response.contains ("\"ok\":false"));
    REQUIRE (response.contains ("\"error\""));
}

//==============================================================================
// 7. stop — cancels measurement (non-destructive)
//==============================================================================

TEST_CASE ("CommandParser: stop command returns ok", "[commandparser][stop]")
{
    ensureMessageManager();

    auto plugin = std::make_unique<TestPlugin>();
    plugin->prepareToPlay (44100.0, 256);

    MeasurementSession session;
    session.setPluginInstance (plugin.get());
    session.setSampleRate (44100.0);
    session.setBlockSize (256);

    CommandParser parser;
    parser.setPluginInstance (plugin.get());
    parser.setSession (&session);

    auto response = parser.handleCommand (R"({"cmd":"stop"})");
    REQUIRE (response.contains ("\"ok\":true"));
}

TEST_CASE ("CommandParser: stop is safe when session is null", "[commandparser][stop]")
{
    CommandParser parser;
    auto response = parser.handleCommand (R"({"cmd":"stop"})");
    REQUIRE (response.contains ("\"ok\":true"));
}
