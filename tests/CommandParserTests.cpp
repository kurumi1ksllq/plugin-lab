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
#include "TestCompressorPlugin.h"
#include "../source/ipc/CommandParser.h"
#include "../source/ipc/Protocol.h"
#include "../source/capture/MeasurementSession.h"
#include "../source/analysis/FreqResponse.h"
#include "../source/analysis/Export.h"
#include "../source/host/PluginManager.h"
#include "../source/scan/ScanEngine.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <functional>

// CrashLog recording-stub helpers (defined in CommandParserStubs.cpp)
extern void clearCrashLog();
extern int crashLogErrorCount();
extern bool crashLogContains (const juce::String& substr);

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
    juce::File (exportPath).withFileExtension (".wav").deleteFile();

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
    juce::File (exportPath).withFileExtension (".wav").deleteFile();
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
    juce::File (exportPath).withFileExtension (".wav").deleteFile();

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
    juce::File (exportPath).withFileExtension (".wav").deleteFile();
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
    juce::File (exportPath).withFileExtension (".wav").deleteFile();

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
    juce::File (exportPath).withFileExtension (".wav").deleteFile();
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
// 2b. getScanStatus — plugin-scan state snapshot (plan step 5)
//==============================================================================

TEST_CASE ("CommandParser: getScanStatus returns pre-scan state",
           "[commandparser][getScanStatus]")
{
    ensureMessageManager();

    // ---- Arrange ----
    PluginManager pm;
    CommandParser parser;
    parser.setPluginManager (&pm);

    // ---- Act ----
    auto response = parser.handleCommand (R"({"cmd":"getScanStatus"})");

    // ---- Assert ----
    REQUIRE (response.contains ("\"ok\":true"));
    REQUIRE (response.contains ("\"running\":false"));
    REQUIRE (response.contains ("\"done\":false"));
    REQUIRE (response.contains ("\"progress\":0.000"));
    REQUIRE (response.contains ("\"count\":0"));
}

TEST_CASE ("CommandParser: getScanStatus reports an in-progress scan",
           "[commandparser][getScanStatus]")
{
    ensureMessageManager();

    // ---- Arrange ----
    PluginManager pm;
    pm.beginScan();
    pm.updateScanProgress (0.5f, "C:\\plugins\\Mid.vst3");
    CommandParser parser;
    parser.setPluginManager (&pm);

    // ---- Act ----
    auto response = parser.handleCommand (R"({"cmd":"getScanStatus"})");

    // ---- Assert: the response must PARSE as valid JSON (verifier V1: Windows
    // backslash paths break .quoted(); substring asserts can't catch it) ----
    auto json = juce::JSON::parse (response);
    auto* obj = json.getDynamicObject();
    REQUIRE (obj != nullptr);
    REQUIRE (obj->getProperty ("ok") == juce::var (true));
    REQUIRE (obj->getProperty ("running") == juce::var (true));
    REQUIRE (obj->getProperty ("done") == juce::var (false));
    REQUIRE (obj->getProperty ("progress") == juce::var (0.5));
    REQUIRE (obj->getProperty ("currentFile").toString() == "C:\\plugins\\Mid.vst3");
}

TEST_CASE ("CommandParser: getScanStatus reports a finished scan",
           "[commandparser][getScanStatus]")
{
    ensureMessageManager();

    // ---- Arrange ----
    PluginManager pm;
    pm.beginScan();
    pm.updateScanProgress (0.9f, "C:\\plugins\\Almost.vst3");
    pm.endScan();
    CommandParser parser;
    parser.setPluginManager (&pm);

    // ---- Act ----
    auto response = parser.handleCommand (R"({"cmd":"getScanStatus"})");

    // ---- Assert: parse as JSON too (finished state must also be valid) ----
    auto json = juce::JSON::parse (response);
    auto* obj = json.getDynamicObject();
    REQUIRE (obj != nullptr);
    REQUIRE (obj->getProperty ("running") == juce::var (false));
    REQUIRE (obj->getProperty ("done") == juce::var (true));
    REQUIRE (obj->getProperty ("progress") == juce::var (1.0));
}

TEST_CASE ("CommandParser: getScanStatus errors without a plugin manager",
           "[commandparser][getScanStatus]")
{
    ensureMessageManager();

    // ---- Arrange ----
    CommandParser parser;   // no plugin manager set

    // ---- Act ----
    auto response = parser.handleCommand (R"({"cmd":"getScanStatus"})");

    // ---- Assert ----
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

TEST_CASE ("CommandParser: loadPlugin/setParam/getParams responses are strict JSON",
           "[commandparser][response-json]")
{
    ensureMessageManager();

    // ---- Arrange ----
    // A strict consumer (the Python batch driver) parses every response line
    // with json.loads — the name/param fields must be properly quoted JSON,
    // not double-quoted via String::quoted().
    PluginManager pm;
    juce::PluginDescription desc;
    desc.name             = "TestVST3";
    desc.pluginFormatName = "VST3";
    desc.fileOrIdentifier = "TestVST3.vst3";
    desc.uniqueId         = 0xABCD1234;
    desc.numInputChannels  = 2;
    desc.numOutputChannels = 2;
    pm.getKnownPlugins().addType (desc);

    auto plugin = std::make_unique<TestPlugin>();
    plugin->setGain (1.0);
    plugin->prepareToPlay (48000.0, 256);

    CommandParser parser;
    parser.setPluginManager (&pm);
    parser.setPluginInstance (plugin.get());

    // ---- Act & Assert ----
    // loadPlugin: the "name" field must strict-parse to the plugin name.
    auto loadResp = parser.handleCommand (R"({"cmd":"loadPlugin","path":"TestVST3"})");
    auto loadJson = juce::JSON::parse (loadResp);
    REQUIRE (! loadJson.isUndefined());
    REQUIRE (loadJson["name"].toString() == "TestVST3");

    // setParam: the "param" field strict-parses to the parameter display name.
    auto setResp = parser.handleCommand (R"({"cmd":"setParam","name":"Gain","value":0.5})");
    auto setJson = juce::JSON::parse (setResp);
    REQUIRE (! setJson.isUndefined());
    REQUIRE (setJson["param"].toString() == "Gain");

    // getParams: every entry's "name" strict-parses.
    auto getResp = parser.handleCommand (R"({"cmd":"getParams"})");
    auto getJson = juce::JSON::parse (getResp);
    REQUIRE (! getJson.isUndefined());
    REQUIRE (getJson["params"][0]["name"].toString() == "Gain");
}

TEST_CASE ("CommandParser: loadPlugin matches name case-insensitively",
           "[commandparser][loadPlugin-case-insensitive]")
{
    ensureMessageManager();

    // ---- Arrange ----
    // Registered under "TestVST3" — the request addresses it in lowercase.
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
    auto response = parser.handleCommand (R"({"cmd":"loadPlugin","path":"testvst3"})");

    // Flush MessageManager for the callAsync in loadPlugin
    flushMessageManager (200);

    // ---- Assert ----
    REQUIRE (response.contains ("\"ok\":true"));
    REQUIRE (callbackFired.load());
    REQUIRE (capturedDesc.name == "TestVST3");
    REQUIRE (capturedDesc.fileOrIdentifier == "TestVST3.vst3");
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

TEST_CASE ("CommandParser: setParam resolves by param_id with ambiguity lock",
           "[commandparser][setParam-param-id]")
{
    ensureMessageManager();

    // ---- Arrange ----
    // Four EQ-band-like parameters whose stable IDs differ from their display
    // names; every name starts with "Band 1" so name-only resolution is
    // ambiguous. Distinct starting values make "untouched" assertions exact.
    TestPlugin plugin;
    auto* pUsed = plugin.addTestParameter ("b1used", "Band 1 Used", 1.0f);
    auto* pFreq = plugin.addTestParameter ("b1freq", "Band 1 Frequency", 0.5f);
    auto* pGain = plugin.addTestParameter ("b1gain", "Band 1 Gain", 0.5f);
    auto* pQ    = plugin.addTestParameter ("b1q", "Band 1 Q", 0.5f);

    pUsed->setValue (0.1f);
    pFreq->setValue (0.2f);
    pGain->setValue (0.3f);
    pQ->setValue (0.4f);

    CommandParser parser;
    parser.setPluginInstance (&plugin);

    // ---- Act / Assert ----
    // (i) param_id alone must select the Q parameter by stable ID, never the
    //     gain parameter that an empty-name contains-match would hit.
    auto resp1 = parser.handleCommand (R"({"cmd":"setParam","param_id":"b1q","value":0.5})");
    REQUIRE (resp1.contains ("\"ok\":true"));
    REQUIRE (pQ->getValue() == Catch::Approx (0.5f));
    REQUIRE (pUsed->getValue() == Catch::Approx (0.1f));  // untouched

    // (ii) param_id + ambiguous name: the stable ID wins. Must NOT hit
    //      "Band 1 Gain" via name nor "Band 1 Used" via the contains fallback.
    auto resp2 = parser.handleCommand (R"({"cmd":"setParam","param_id":"b1gain","name":"Band 1","value":0.9})");
    REQUIRE (resp2.contains ("\"ok\":true"));
    REQUIRE (pGain->getValue() == Catch::Approx (0.9f));
    REQUIRE (pUsed->getValue() == Catch::Approx (0.1f));  // untouched
    REQUIRE (pQ->getValue() == Catch::Approx (0.5f));     // untouched

    // (iii) exact display name without param_id still resolves.
    auto resp3 = parser.handleCommand (R"({"cmd":"setParam","name":"Band 1 Used","value":0.25})");
    REQUIRE (resp3.contains ("\"ok\":true"));
    REQUIRE (pUsed->getValue() == Catch::Approx (0.25f));

    // (iv) ambiguous name without param_id keeps the legacy first-contains
    //      behaviour ("Band 1 Used" is the first parameter containing "Band 1").
    auto resp4 = parser.handleCommand (R"({"cmd":"setParam","name":"Band 1","value":0.15})");
    REQUIRE (resp4.contains ("\"ok\":true"));
    REQUIRE (pUsed->getValue() == Catch::Approx (0.15f));
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

TEST_CASE ("CommandParser: getParams includes stable param_id distinct from display name",
           "[commandparser][getParams-param-id]")
{
    // ---- Arrange ----
    // TestPlugin exposes two hosted parameters whose stable IDs differ from
    // their display names: "gain" ↔ "Gain" (index 0), "latency" ↔ "Latency"
    // (index 1). The param_id field lets callers address parameters without
    // parsing display names.
    TestPlugin plugin;
    CommandParser parser;
    parser.setPluginInstance (&plugin);

    // ---- Act ----
    auto response = parser.handleCommand (R"({"cmd":"getParams"})");

    // ---- Assert ----
    REQUIRE (response.contains ("\"ok\":true"));
    REQUIRE (response.contains (R"("param_id":"gain")"));
    REQUIRE (response.contains (R"("param_id":"latency")"));
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
// 6b. Measure — plugin exception protection (block C task 1)
//
// A plugin that throws from prepareToPlay/processBlock must surface as an
// ok:false error response — never escape into the message loop (which would
// std::terminate → abort the whole host).
//==============================================================================

TEST_CASE ("CommandParser: measure returns ok:false when plugin processBlock throws",
           "[commandparser][measure][exception]")
{
    ensureMessageManager();

    // ---- Arrange ----
    auto plugin = std::make_unique<TestPlugin>();
    plugin->setGain (1.0);
    plugin->setThrowOnProcessBlock (true);

    MeasurementSession session;
    session.setPluginInstance (plugin.get());
    session.setSampleRate (44100.0);
    session.setBlockSize (256);
    session.setMeasurementType (MeasurementSession::Type::frequencyResponse);

    CommandParser parser;
    parser.setPluginInstance (plugin.get());
    parser.setSession (&session);

    clearCrashLog();

    // ---- Act ----
    auto response = parser.handleCommand (R"({"cmd":"measure","type":"frequency_response"})");

    // ---- Assert ----
    // 1. Failed measurement surfaces as an error response
    REQUIRE (response.contains ("\"ok\":false"));
    REQUIRE (response.contains ("measurement failed"));

    // 2. The exception was recorded in the crash log
    REQUIRE (crashLogErrorCount() >= 1);
    REQUIRE (crashLogContains ("Sweep"));
}

TEST_CASE ("CommandParser: measure returns ok:false when plugin prepareToPlay throws",
           "[commandparser][measure][exception]")
{
    ensureMessageManager();

    // ---- Arrange ----
    auto plugin = std::make_unique<TestPlugin>();
    plugin->setGain (1.0);
    plugin->setThrowOnPrepareToPlay (true);

    MeasurementSession session;
    session.setPluginInstance (plugin.get());
    session.setSampleRate (44100.0);
    session.setBlockSize (256);
    session.setMeasurementType (MeasurementSession::Type::frequencyResponse);

    CommandParser parser;
    parser.setPluginInstance (plugin.get());
    parser.setSession (&session);

    clearCrashLog();

    // ---- Act ----
    auto response = parser.handleCommand (R"({"cmd":"measure","type":"frequency_response"})");

    // ---- Assert ----
    REQUIRE (response.contains ("\"ok\":false"));
    REQUIRE (response.contains ("measurement failed"));
    REQUIRE (crashLogErrorCount() >= 1);
    REQUIRE (crashLogContains ("Sweep"));
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

//==============================================================================
// 8. Measure — input source axis (signal | file | noise | dynamic)
//
// Non-signal sources skip analysis (that is phase 4) and export a
// raw_capture JSON carrying the source metadata.
//==============================================================================

/** Writes a test .wav into the temp directory; 24-bit PCM. Mirrors the
 *  helper in FilePlaybackTests. */
static juce::File writeSourceTestWav (double sampleRate, int numChannels, int64_t numSamples,
                                      const std::function<float (int ch, int64_t sample)>& sampleFn)
{
    const auto file = juce::File::getSpecialLocation (juce::File::tempDirectory)
                          .getChildFile ("commandparser_source_" + juce::Uuid().toString() + ".wav");

    auto* stream = new juce::FileOutputStream (file);
    if (stream->failedToOpen())
    {
        delete stream;
        return {};
    }

    juce::WavAudioFormat wavFormat;
    auto writerOptions = juce::AudioFormatWriterOptions{}
                             .withSampleRate (sampleRate)
                             .withNumChannels (numChannels)
                             .withBitsPerSample (24);

    std::unique_ptr<juce::OutputStream> streamOwner (stream);
    std::unique_ptr<juce::AudioFormatWriter> writer (
        wavFormat.createWriterFor (streamOwner, writerOptions));

    if (writer == nullptr)
        return {};

    constexpr int blockSize = 1024;
    juce::AudioBuffer<float> block (numChannels, blockSize);

    int64_t written = 0;
    while (written < numSamples)
    {
        const int n = static_cast<int> (std::min<int64_t> (blockSize, numSamples - written));
        block.clear();

        for (int ch = 0; ch < numChannels; ++ch)
            for (int s = 0; s < n; ++s)
                block.setSample (ch, s, sampleFn (ch, written + s));

        writer->writeFromAudioSampleBuffer (block, 0, n);
        written += n;
    }

    writer.reset();  // finalises the wav header
    return file;
}

TEST_CASE ("CommandParser: file source captures raw audio and exports raw_capture JSON",
           "[commandparser][source-file]")
{
    ensureMessageManager();

    // 1 s stereo 48 kHz wav (tone per channel).
    const auto wavFile = writeSourceTestWav (48000.0, 2, 48000,
        [] (int ch, int64_t s)
        {
            const double t = static_cast<double> (s) / 48000.0;
            const double freq = (ch == 0) ? 440.0 : 880.0;
            return static_cast<float> (0.5 * std::sin (2.0 * juce::MathConstants<double>::pi * freq * t));
        });
    REQUIRE (wavFile.existsAsFile());

    auto plugin = std::make_unique<TestPlugin>();
    plugin->setGain (1.0);
    plugin->prepareToPlay (48000.0, 256);

    MeasurementSession session;
    session.setPluginInstance (plugin.get());
    session.setSampleRate (48000.0);
    session.setBlockSize (256);

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
            .getChildFile ("test_measure_raw_file.json")
            .getFullPathName();
    juce::File (exportPath).deleteFile();
    juce::File (exportPath).withFileExtension (".wav").deleteFile();

    // For the file source "path" names the INPUT audio file; the export path
    // is carried by the disambiguated "export_path" field.
    const juce::String jsonCmd =
        juce::String (R"({"cmd":"measure","source":"file","path":)")
        + juce::JSON::toString (wavFile.getFullPathName())
        + juce::String (R"(,"export_path":)") + juce::JSON::toString (exportPath)
        + "}";
    auto response = parser.handleCommand (jsonCmd);

    flushMessageManager (200);

    // ---- Assert ----
    // 1. Response: success, 1 s @ 48 kHz -> 48000 samples.
    REQUIRE (response.contains ("\"ok\":true"));
    REQUIRE (response.contains ("\"samples\":48000"));
    REQUIRE (response.contains ("\"export_path\":"));
    REQUIRE_FALSE (response.contains ("\"error\""));

    // 2. Callback fired with raw-capture metadata (no analysis payload).
    REQUIRE (callbackFired.load());
    REQUIRE (capturedResult.source == "file");
    REQUIRE (capturedResult.rawSamples == 48000);
    REQUIRE (capturedResult.rawSampleRate == Catch::Approx (48000.0));

    // 3. Export: raw_capture JSON with source metadata.
    juce::File exportFile (exportPath);
    REQUIRE (exportFile.existsAsFile());
    auto exportedJson = juce::JSON::parse (exportFile.loadFileAsString());
    auto* exportObj = exportedJson.getDynamicObject();
    REQUIRE (exportObj != nullptr);
    REQUIRE (exportObj->getProperty ("type").toString() == "raw_capture");
    REQUIRE (static_cast<int64_t> (exportedJson["samples"]) == 48000);
    REQUIRE (exportedJson["sample_rate"].equals (48000.0));
    REQUIRE (exportedJson["block_size"].equals (256));
    REQUIRE (exportedJson["source"]["type"].toString() == "file");
    REQUIRE (exportedJson["source"]["file_path"].toString() == wavFile.getFullPathName());
    REQUIRE (exportedJson["source"]["sample_rate"].equals (48000.0));
    REQUIRE (static_cast<double> (exportedJson["source"]["duration_sec"]) == Catch::Approx (1.0));

    exportFile.deleteFile();
    juce::File (exportPath).withFileExtension (".wav").deleteFile();
    wavFile.deleteFile();
}

TEST_CASE ("CommandParser: noise source captures deterministic noise and exports raw_capture JSON",
           "[commandparser][source-noise]")
{
    ensureMessageManager();

    auto plugin = std::make_unique<TestPlugin>();
    plugin->setGain (1.0);
    plugin->prepareToPlay (48000.0, 256);

    MeasurementSession session;
    session.setPluginInstance (plugin.get());
    session.setSampleRate (48000.0);
    session.setBlockSize (256);

    CommandParser parser;
    parser.setPluginInstance (plugin.get());
    parser.setSession (&session);

    const juce::String exportPath =
        juce::File::getCurrentWorkingDirectory()
            .getChildFile ("test_measure_raw_noise.json")
            .getFullPathName();
    juce::File (exportPath).deleteFile();
    juce::File (exportPath).withFileExtension (".wav").deleteFile();

    // 2 s @ 48 kHz -> 96000 samples; deterministic via seed 42.
    const juce::String jsonCmd =
        juce::String (R"({"cmd":"measure","source":"noise","duration":2,"seed":42,"path":)")
        + juce::JSON::toString (exportPath) + "}";
    auto response = parser.handleCommand (jsonCmd);

    flushMessageManager (200);

    // ---- Assert ----
    REQUIRE (response.contains ("\"ok\":true"));
    REQUIRE (response.contains ("\"samples\":96000"));
    REQUIRE (response.contains ("\"rate\":48000"));
    REQUIRE_FALSE (response.contains ("\"error\""));

    juce::File exportFile (exportPath);
    REQUIRE (exportFile.existsAsFile());
    auto exportedJson = juce::JSON::parse (exportFile.loadFileAsString());
    REQUIRE (exportedJson["type"].toString() == "raw_capture");
    REQUIRE (exportedJson["source"]["type"].toString() == "noise");
    REQUIRE (exportedJson["source"]["seed"].equals (42));
    REQUIRE (exportedJson["source"]["noise_type"].toString() == "white");
    REQUIRE (static_cast<double> (exportedJson["source"]["duration_sec"]) == Catch::Approx (2.0));

    exportFile.deleteFile();
    juce::File (exportPath).withFileExtension (".wav").deleteFile();
}

TEST_CASE ("CommandParser: measure with noise source flushes dry/wet capture to a WAV file",
           "[commandparser][measure-flush-wav]")
{
    ensureMessageManager();

    auto plugin = std::make_unique<TestPlugin>();
    plugin->setGain (1.0);
    plugin->prepareToPlay (48000.0, 256);

    MeasurementSession session;
    session.setPluginInstance (plugin.get());
    session.setSampleRate (48000.0);
    session.setBlockSize (256);

    CommandParser parser;
    parser.setPluginInstance (plugin.get());
    parser.setSession (&session);

    // Explicit export path inside the temp directory; the crash-protection
    // WAV mirror is derived from it by swapping ".json" for ".wav".
    const juce::File tempDir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                   .getChildFile ("pluginlab_flush_measure_test");
    tempDir.createDirectory();
    const juce::File jsonPath = tempDir.getChildFile ("flush_measure_test.json");
    const juce::File wavPath  = tempDir.getChildFile ("flush_measure_test.wav");
    jsonPath.deleteFile();
    wavPath.deleteFile();

    // 2 s @ 48 kHz -> 96000 samples; short enough that no 5 s flush boundary
    // is crossed mid-capture (the file is still created — SweepRunner's
    // result.trim() finalises it).
    const juce::String jsonCmd =
        juce::String (R"({"cmd":"measure","source":"noise","duration":2,"seed":42,"path":)")
        + juce::JSON::toString (jsonPath.getFullPathName()) + "}";
    auto response = parser.handleCommand (jsonCmd);

    flushMessageManager (200);

    // ---- Assert ----
    REQUIRE (response.contains ("\"ok\":true"));
    REQUIRE (response.contains ("\"samples\":96000"));
    REQUIRE (response.contains ("\"wav_path\":"));
    REQUIRE_FALSE (response.contains ("\"error\""));

    // The derived .wav must exist and be readable as a WAV: 2 plugin
    // channels -> 4 interleaved channels (dry 2 + wet 2), 2 s @ 48 kHz.
    REQUIRE (wavPath.existsAsFile());
    juce::WavAudioFormat wavFormat;
    std::unique_ptr<juce::AudioFormatReader> reader (
        wavFormat.createReaderFor (new juce::FileInputStream (wavPath), true));
    REQUIRE (reader != nullptr);
    REQUIRE (reader->numChannels == 4);
    REQUIRE (reader->sampleRate == Catch::Approx (48000.0));
    REQUIRE (reader->lengthInSamples == 96000);

    // Clean up
    jsonPath.deleteFile();
    wavPath.deleteFile();
    tempDir.deleteRecursively();
}

TEST_CASE ("CommandParser: dynamic source wraps carrier in envelope and exports raw_capture JSON",
           "[commandparser][source-dynamic]")
{
    ensureMessageManager();

    auto plugin = std::make_unique<TestPlugin>();
    plugin->setGain (1.0);
    plugin->prepareToPlay (48000.0, 256);

    MeasurementSession session;
    session.setPluginInstance (plugin.get());
    session.setSampleRate (48000.0);
    session.setBlockSize (256);

    CommandParser parser;
    parser.setPluginInstance (plugin.get());
    parser.setSession (&session);

    const juce::String exportPath =
        juce::File::getCurrentWorkingDirectory()
            .getChildFile ("test_measure_raw_dynamic.json")
            .getFullPathName();
    juce::File (exportPath).deleteFile();
    juce::File (exportPath).withFileExtension (".wav").deleteFile();

    // Enveloped 2 s sweep -> 96000 samples.
    const juce::String jsonCmd =
        juce::String (R"({"cmd":"measure","source":"dynamic","path":)")
        + juce::JSON::toString (exportPath) + "}";
    auto response = parser.handleCommand (jsonCmd);

    flushMessageManager (200);

    // ---- Assert ----
    REQUIRE (response.contains ("\"ok\":true"));
    REQUIRE (response.contains ("\"samples\":96000"));
    REQUIRE_FALSE (response.contains ("\"error\""));

    juce::File exportFile (exportPath);
    REQUIRE (exportFile.existsAsFile());
    auto exportedJson = juce::JSON::parse (exportFile.loadFileAsString());
    REQUIRE (exportedJson["type"].toString() == "raw_capture");
    REQUIRE (exportedJson["source"]["type"].toString() == "dynamic");

    exportFile.deleteFile();
    juce::File (exportPath).withFileExtension (".wav").deleteFile();
}

TEST_CASE ("CommandParser: measure without source defaults to signal and analyses normally",
           "[commandparser][source-default]")
{
    ensureMessageManager();

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
            .getChildFile ("test_measure_default_signal.json")
            .getFullPathName();
    juce::File (exportPath).deleteFile();
    juce::File (exportPath).withFileExtension (".wav").deleteFile();

    // No "source" field -> identical to the pre-existing signal behaviour.
    const juce::String jsonCmd =
        juce::String (R"({"cmd":"measure","type":"frequency_response","path":)")
        + juce::JSON::toString (exportPath) + "}";
    auto response = parser.handleCommand (jsonCmd);

    flushMessageManager (200);

    // ---- Assert ----
    REQUIRE (response.contains ("\"ok\":true"));
    REQUIRE (response.contains ("\"samples\":"));
    REQUIRE (response.contains ("\"export_path\":"));
    REQUIRE (callbackFired.load());
    REQUIRE (capturedResult.type == MeasurementSession::Type::frequencyResponse);
    REQUIRE (capturedResult.source == "signal");
    REQUIRE_FALSE (capturedResult.freq.raw.empty());
    REQUIRE (capturedResult.freq.raw[0].frequency > 0.0);

    // The export carries the signal source metadata block.
    juce::File exportFile (exportPath);
    REQUIRE (exportFile.existsAsFile());
    auto exportedJson = juce::JSON::parse (exportFile.loadFileAsString());
    REQUIRE (exportedJson["type"].toString() == "frequency_response");
    REQUIRE (exportedJson["source"]["type"].toString() == "signal");

    exportFile.deleteFile();
    juce::File (exportPath).withFileExtension (".wav").deleteFile();
}

TEST_CASE ("CommandParser: unknown source returns error", "[commandparser][source-invalid]")
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

    auto response = parser.handleCommand (
        R"({"cmd":"measure","type":"frequency_response","source":"bogus"})");
    REQUIRE (response.contains ("\"ok\":false"));
    REQUIRE (response.contains ("\"error\""));
}

TEST_CASE ("CommandParser: file source without path returns error", "[commandparser][source-file-missing]")
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

    auto response = parser.handleCommand (R"({"cmd":"measure","source":"file"})");
    REQUIRE (response.contains ("\"ok\":false"));
    REQUIRE (response.contains ("\"error\""));
}

TEST_CASE ("CommandParser: file source with nonexistent path returns error without crashing",
           "[commandparser][source-file-missing]")
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

    auto response = parser.handleCommand (
        R"({"cmd":"measure","source":"file","path":"Z:\nonexistent\missing.wav"})");
    REQUIRE (response.contains ("\"ok\":false"));
    REQUIRE (response.contains ("\"error\""));
}

//==============================================================================
// 9. Scan (T3.3) — parameter sweep: IPC command + ScanEngine + export + callback
//
// The scan command runs one measurement round per parameter value and
// exports a family JSON via Export::scanToJSON. A dedicated
// setScanCompleteCallback(ScanResult) fires on completion (same synchronous
// timing as measurementCompleteCallback).
//==============================================================================

TEST_CASE ("CommandParser: scan sweeps gain across 3 values and exports family JSON",
           "[commandparser][scan-happy]")
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

    std::atomic<bool> scanCompleteFired { false };
    ScanEngine::ScanResult capturedScan;
    parser.setScanCompleteCallback ([&] (const ScanEngine::ScanResult& r) {
        scanCompleteFired.store (true);
        capturedScan = r;
    });

    const juce::String exportPath =
        juce::File::getCurrentWorkingDirectory()
            .getChildFile ("test_scan_output.json")
            .getFullPathName();
    juce::File (exportPath).deleteFile();
    juce::File (exportPath).withFileExtension (".wav").deleteFile();

    // ---- Act ----
    // 3 gain values (all non-zero so every round yields a valid response).
    const juce::String jsonCmd =
        juce::String (R"({"cmd":"scan","type":"frequency_response","param_id":"gain",)"
                      R"("values":[0.1,0.5,0.9],"path":)")
        + juce::JSON::toString (exportPath) + "}";
    auto response = parser.handleCommand (jsonCmd);

    flushMessageManager (200);

    // ---- Assert ----
    // 1. Response: ok, 3 runs, export path present.
    REQUIRE (response.contains ("\"ok\":true"));
    REQUIRE (response.contains ("\"runs\":3"));
    REQUIRE (response.contains ("\"export_path\":"));
    REQUIRE_FALSE (response.contains ("\"error\""));

    // 2. Scan-complete callback fired with the full family (3 entries).
    REQUIRE (scanCompleteFired.load());
    REQUIRE (capturedScan.paramId == "gain");
    REQUIRE (capturedScan.family.size() == 3);
    REQUIRE (capturedScan.values.size() == 3);

    // 3. Export: top-level type "scan", family length 3, every round has a
    //    valid frequency-response result (non-empty raw) + latency re-read.
    juce::File exportFile (exportPath);
    REQUIRE (exportFile.existsAsFile());
    auto exportedJson = juce::JSON::parse (exportFile.loadFileAsString());
    REQUIRE (exportedJson.getDynamicObject() != nullptr);
    REQUIRE (exportedJson["type"].toString() == "scan");
    REQUIRE (exportedJson["scan"]["param_id"].toString() == "gain");
    REQUIRE (exportedJson["scan"]["param_name"].toString() == "Gain");
    REQUIRE (exportedJson["scan"]["values"].size() == 3);
    REQUIRE (exportedJson["family"].size() == 3);
    for (int i = 0; i < 3; ++i)
    {
        REQUIRE (exportedJson["family"][i]["param_value_normalized"].isDouble());
        REQUIRE (exportedJson["family"][i]["result"]["raw"].size() > 0);
    }

    // Cleanup
    exportFile.deleteFile();
    juce::File (exportPath).withFileExtension (".wav").deleteFile();
}

TEST_CASE ("CommandParser: scan fails for unknown param_id", "[commandparser][scan-unknown-param]")
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

    auto response = parser.handleCommand (
        R"({"cmd":"scan","param_id":"nonexistent","values":[0.5]})");
    REQUIRE (response.contains ("\"ok\":false"));
    REQUIRE (response.contains ("\"error\""));
}

TEST_CASE ("CommandParser: scan fails for empty values array", "[commandparser][scan-empty-values]")
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

    auto response = parser.handleCommand (R"({"cmd":"scan","param_id":"gain","values":[]})");
    REQUIRE (response.contains ("\"ok\":false"));
    REQUIRE (response.contains ("\"error\""));
}

TEST_CASE ("CommandParser: scan rejects values outside 0..1", "[commandparser][scan-out-of-range]")
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

    auto response = parser.handleCommand (
        R"({"cmd":"scan","param_id":"gain","values":[0.5,1.5]})");
    REQUIRE (response.contains ("\"ok\":false"));
    REQUIRE (response.contains ("\"error\""));
}

TEST_CASE ("CommandParser: scan with noise source runs noise rounds and exports valid family",
           "[commandparser][scan-source-noise]")
{
    ensureMessageManager();

    // ---- Arrange ----
    auto plugin = std::make_unique<TestPlugin>();
    plugin->setGain (1.0);
    plugin->prepareToPlay (48000.0, 256);

    MeasurementSession session;
    session.setPluginInstance (plugin.get());
    session.setSampleRate (48000.0);
    session.setBlockSize (256);
    session.setMeasurementType (MeasurementSession::Type::frequencyResponse);

    CommandParser parser;
    parser.setPluginInstance (plugin.get());
    parser.setSession (&session);

    const juce::String exportPath =
        juce::File::getCurrentWorkingDirectory()
            .getChildFile ("test_scan_noise.json")
            .getFullPathName();
    juce::File (exportPath).deleteFile();
    juce::File (exportPath).withFileExtension (".wav").deleteFile();

    // ---- Act ----
    // 2 gain values, 1 s deterministic white noise per round (seed 42).
    const juce::String jsonCmd =
        juce::String (R"({"cmd":"scan","type":"frequency_response","param_id":"gain",)"
                      R"("values":[0.5,0.9],"source":"noise","duration":1,"seed":42,"path":)")
        + juce::JSON::toString (exportPath) + "}";
    auto response = parser.handleCommand (jsonCmd);

    flushMessageManager (200);

    // ---- Assert ----
    REQUIRE (response.contains ("\"ok\":true"));
    REQUIRE (response.contains ("\"runs\":2"));
    REQUIRE_FALSE (response.contains ("\"error\""));

    juce::File exportFile (exportPath);
    REQUIRE (exportFile.existsAsFile());
    auto exportedJson = juce::JSON::parse (exportFile.loadFileAsString());
    REQUIRE (exportedJson["type"].toString() == "scan");
    REQUIRE (exportedJson["family"].size() == 2);
    // Noise has broadband energy -> every round yields a non-empty result.
    REQUIRE (exportedJson["family"][0]["result"]["raw"].size() > 0);
    REQUIRE (exportedJson["family"][1]["result"]["raw"].size() > 0);
    // The noise source metadata is attached to the scan export context.
    REQUIRE (exportedJson["context"]["source"]["type"].toString() == "noise");

    // Cleanup
    exportFile.deleteFile();
    juce::File (exportPath).withFileExtension (".wav").deleteFile();
}

//==============================================================================
// 10. GR timeline (T4.4) — measure gr_timeline: GainReduction over the
// dry/wet pair + TimeConstants; dynamic source auto-detects the envelope
// edges, file source has no controlled edges (tau stays invalid).
//==============================================================================

TEST_CASE ("CommandParser: dynamic source + gr_timeline exports GR timeline JSON",
           "[commandparser][measure-gr-dynamic]")
{
    ensureMessageManager();

    // ---- Arrange ----
    // Constant -6.02 dB gain: a flat GR timeline with no dynamic edges.
    auto plugin = std::make_unique<TestPlugin>();
    plugin->setGain (0.5);
    plugin->prepareToPlay (48000.0, 256);

    MeasurementSession session;
    session.setPluginInstance (plugin.get());
    session.setSampleRate (48000.0);
    session.setBlockSize (256);

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
            .getChildFile ("test_measure_gr_dynamic.json")
            .getFullPathName();
    juce::File (exportPath).deleteFile();
    juce::File (exportPath).withFileExtension (".wav").deleteFile();

    // ---- Act ----
    const juce::String jsonCmd =
        juce::String (R"({"cmd":"measure","source":"dynamic","type":"gr_timeline","path":)")
        + juce::JSON::toString (exportPath) + "}";
    auto response = parser.handleCommand (jsonCmd);

    flushMessageManager (200);

    // ---- Assert ----
    // 1. Response: success, 2 s @ 48 kHz -> 96000 samples.
    REQUIRE (response.contains ("\"ok\":true"));
    REQUIRE (response.contains ("\"samples\":96000"));
    REQUIRE (response.contains ("\"export_path\":"));
    REQUIRE_FALSE (response.contains ("\"error\""));

    // 2. Callback carried the grTimeline variant (GR + tau).
    REQUIRE (callbackFired.load());
    REQUIRE (capturedResult.type == MeasurementSession::Type::grTimeline);
    REQUIRE_FALSE (capturedResult.gr.timeline.empty());

    // 3. Export: gr_timeline JSON with a non-empty GR timeline, GR ≈ -6.02 dB
    //    in the driven region (silence windows report 0 dB, so check the
    //    deepest reduction), and a tau block whose valid flag is a bool.
    juce::File exportFile (exportPath);
    REQUIRE (exportFile.existsAsFile());
    auto exportedJson = juce::JSON::parse (exportFile.loadFileAsString());
    REQUIRE (exportedJson["type"].toString() == "gr_timeline");
    REQUIRE (exportedJson["gr"]["timeline"].size() > 0);

    double minGR = 0.0;
    for (int i = 0; i < exportedJson["gr"]["timeline"].size(); ++i)
        minGR = std::min (minGR, static_cast<double> (exportedJson["gr"]["timeline"][i]["gr_db"]));
    REQUIRE (minGR == Catch::Approx (20.0 * std::log10 (0.5)).margin (0.3));

    REQUIRE (exportedJson["tau"]["valid"].isBool());
    REQUIRE (exportedJson["tau"]["attack_sec"].isDouble());
    REQUIRE (exportedJson["tau"]["release_sec"].isDouble());

    // Cleanup
    exportFile.deleteFile();
    juce::File (exportPath).withFileExtension (".wav").deleteFile();
}

TEST_CASE ("CommandParser: file source + gr_timeline exports GR timeline without tau",
           "[commandparser][measure-gr-file]")
{
    ensureMessageManager();

    // ---- Arrange ----
    // Real-vocal-like file: no controlled attack/release edges, so the
    // markers stay empty and the tau estimate is invalid by design.
    const auto wavFile = writeSourceTestWav (48000.0, 2, 48000,
        [] (int ch, int64_t s)
        {
            const double t = static_cast<double> (s) / 48000.0;
            const double freq = (ch == 0) ? 440.0 : 880.0;
            return static_cast<float> (0.5 * std::sin (2.0 * juce::MathConstants<double>::pi * freq * t));
        });
    REQUIRE (wavFile.existsAsFile());

    auto plugin = std::make_unique<TestPlugin>();
    plugin->setGain (1.0);
    plugin->prepareToPlay (48000.0, 256);

    MeasurementSession session;
    session.setPluginInstance (plugin.get());
    session.setSampleRate (48000.0);
    session.setBlockSize (256);

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
            .getChildFile ("test_measure_gr_file.json")
            .getFullPathName();
    juce::File (exportPath).deleteFile();
    juce::File (exportPath).withFileExtension (".wav").deleteFile();

    // ---- Act ----
    // "path" names the INPUT audio file; "export_path" the JSON output.
    const juce::String jsonCmd =
        juce::String (R"({"cmd":"measure","source":"file","type":"gr_timeline","path":)")
        + juce::JSON::toString (wavFile.getFullPathName())
        + juce::String (R"(,"export_path":)") + juce::JSON::toString (exportPath)
        + "}";
    auto response = parser.handleCommand (jsonCmd);

    flushMessageManager (200);

    // ---- Assert ----
    // 1. Response: success, 1 s @ 48 kHz -> 48000 samples.
    REQUIRE (response.contains ("\"ok\":true"));
    REQUIRE (response.contains ("\"samples\":48000"));
    REQUIRE (response.contains ("\"export_path\":"));
    REQUIRE_FALSE (response.contains ("\"error\""));

    // 2. Callback carried the grTimeline variant with a non-empty GR timeline.
    REQUIRE (callbackFired.load());
    REQUIRE (capturedResult.type == MeasurementSession::Type::grTimeline);
    REQUIRE_FALSE (capturedResult.gr.timeline.empty());

    // 3. Export: GR timeline present; no controlled edges -> tau invalid
    //    (zeros + valid=false + empty curve families) — a design lock.
    juce::File exportFile (exportPath);
    REQUIRE (exportFile.existsAsFile());
    auto exportedJson = juce::JSON::parse (exportFile.loadFileAsString());
    REQUIRE (exportedJson["type"].toString() == "gr_timeline");
    REQUIRE (exportedJson["gr"]["timeline"].size() > 0);
    REQUIRE (! (bool) exportedJson["tau"]["valid"]);
    REQUIRE (static_cast<double> (exportedJson["tau"]["attack_sec"]) == Catch::Approx (0.0));
    REQUIRE (static_cast<double> (exportedJson["tau"]["release_sec"]) == Catch::Approx (0.0));
    REQUIRE (exportedJson["tau"]["attack_by_level"].size() == 0);
    REQUIRE (exportedJson["tau"]["release_by_level"].size() == 0);

    // Cleanup
    exportFile.deleteFile();
    juce::File (exportPath).withFileExtension (".wav").deleteFile();
    wavFile.deleteFile();
}

//==============================================================================
// 11. GR carrier start frequency (T5.4 fix) — the dynamic carrier sweep must
// start at a frequency high enough that the sample-instantaneous detector is
// not polluted by low-frequency carrier wobble, otherwise the GR attack edge
// is corrupted and tau comes back invalid. The IPC command exposes an optional
// `carrier_start_hz` field; when absent it defaults to 10000 Hz (matching the
// CompressionFamily internal configuration).
//==============================================================================

TEST_CASE ("CommandParser: measure dynamic honours explicit carrier_start_hz",
           "[commandparser][measure-dynamic-carrier-start-explicit]")
{
    ensureMessageManager();

    auto plugin = std::make_unique<TestPlugin>();
    plugin->setGain (1.0);
    plugin->prepareToPlay (48000.0, 256);

    MeasurementSession session;
    session.setPluginInstance (plugin.get());
    session.setSampleRate (48000.0);
    session.setBlockSize (256);

    CommandParser parser;
    parser.setPluginInstance (plugin.get());
    parser.setSession (&session);

    const juce::String exportPath =
        juce::File::getCurrentWorkingDirectory()
            .getChildFile ("test_measure_dynamic_carrier_start.json")
            .getFullPathName();
    juce::File (exportPath).deleteFile();
    juce::File (exportPath).withFileExtension (".wav").deleteFile();

    // 2.5 kHz sweep start — must be forwarded to the session configuration.
    const juce::String jsonCmd =
        juce::String (R"({"cmd":"measure","source":"dynamic","carrier_start_hz":2500,"path":)")
        + juce::JSON::toString (exportPath) + "}";
    auto response = parser.handleCommand (jsonCmd);

    flushMessageManager (200);

    REQUIRE (response.contains ("\"ok\":true"));
    REQUIRE_FALSE (response.contains ("\"error\""));
    REQUIRE (session.getDynamicCarrierStartHz() == Catch::Approx (2500.0));

    juce::File exportFile (exportPath);
    REQUIRE (exportFile.existsAsFile());
    exportFile.deleteFile();
    juce::File (exportPath).withFileExtension (".wav").deleteFile();
}

TEST_CASE ("CommandParser: measure dynamic defaults carrier_start_hz to 10000 Hz",
           "[commandparser][measure-dynamic-carrier-start-default]")
{
    ensureMessageManager();

    auto plugin = std::make_unique<TestPlugin>();
    plugin->setGain (1.0);
    plugin->prepareToPlay (48000.0, 256);

    MeasurementSession session;
    session.setPluginInstance (plugin.get());
    session.setSampleRate (48000.0);
    session.setBlockSize (256);

    CommandParser parser;
    parser.setPluginInstance (plugin.get());
    parser.setSession (&session);

    const juce::String exportPath =
        juce::File::getCurrentWorkingDirectory()
            .getChildFile ("test_measure_dynamic_carrier_default.json")
            .getFullPathName();
    juce::File (exportPath).deleteFile();
    juce::File (exportPath).withFileExtension (".wav").deleteFile();

    // No carrier_start_hz: the parser must apply the 10000 Hz default so the
    // GR timeline is measurable out of the box (matching CompressionFamily).
    const juce::String jsonCmd =
        juce::String (R"({"cmd":"measure","source":"dynamic","path":)")
        + juce::JSON::toString (exportPath) + "}";
    auto response = parser.handleCommand (jsonCmd);

    flushMessageManager (200);

    REQUIRE (response.contains ("\"ok\":true"));
    REQUIRE_FALSE (response.contains ("\"error\""));
    REQUIRE (session.getDynamicCarrierStartHz() == Catch::Approx (10000.0));

    juce::File exportFile (exportPath);
    REQUIRE (exportFile.existsAsFile());
    exportFile.deleteFile();
    juce::File (exportPath).withFileExtension (".wav").deleteFile();
}

TEST_CASE ("CommandParser: gr_timeline on dynamic source yields valid tau with high carrier start",
           "[commandparser][measure-gr-tau-valid-dynamic]")
{
    ensureMessageManager();

    // A real compressor (one-pole detector + gain computer): with the carrier
    // sweep starting at 10 kHz the attack/release edges are clean, so the
    // tau estimate must come back valid (T4.3 locked the same configuration
    // in CompressionFamily). At the old 20 Hz default the low-frequency wobble
    // pollutes the attack edge and tau is invalid.
    auto plugin = std::make_unique<TestCompressorPlugin>();
    plugin->setThresholdDB (-30.0);
    plugin->setRatio (4.0);
    plugin->setAttackSec (0.005);
    plugin->setReleaseSec (0.05);
    plugin->prepareToPlay (48000.0, 256);

    MeasurementSession session;
    session.setPluginInstance (plugin.get());
    session.setSampleRate (48000.0);
    session.setBlockSize (256);

    CommandParser parser;
    parser.setPluginInstance (plugin.get());
    parser.setSession (&session);

    const juce::String exportPath =
        juce::File::getCurrentWorkingDirectory()
            .getChildFile ("test_measure_gr_tau_valid.json")
            .getFullPathName();
    juce::File (exportPath).deleteFile();
    juce::File (exportPath).withFileExtension (".wav").deleteFile();

    const juce::String jsonCmd =
        juce::String (R"({"cmd":"measure","source":"dynamic","type":"gr_timeline","carrier_start_hz":10000,"path":)")
        + juce::JSON::toString (exportPath) + "}";
    auto response = parser.handleCommand (jsonCmd);

    flushMessageManager (200);

    // 1. Response + export present.
    REQUIRE (response.contains ("\"ok\":true"));
    REQUIRE_FALSE (response.contains ("\"error\""));

    juce::File exportFile (exportPath);
    REQUIRE (exportFile.existsAsFile());
    auto exportedJson = juce::JSON::parse (exportFile.loadFileAsString());
    REQUIRE (exportedJson["type"].toString() == "gr_timeline");
    REQUIRE (exportedJson["gr"]["timeline"].size() > 0);

    // 2. The tau block must be valid — the whole point of the carrier_start_hz
    //    fix (at 20 Hz default the low-frequency wobble breaks the edge fit).
    REQUIRE ((bool) exportedJson["tau"]["valid"]);
    REQUIRE (static_cast<double> (exportedJson["tau"]["attack_sec"]) > 0.0);
    // Release: the default ADSR release (0.2 s) is too short for the input to
    // fall below threshold before the 2 s signal ends, so the release edge is
    // not always detected (release_sec may be 0). The attack tau above proves
    // the carrier_start_hz fix; a full release estimate needs a configurable
    // ADSR release (a separate, documented limitation).
    REQUIRE (static_cast<double> (exportedJson["tau"]["release_sec"]) >= 0.0);

    exportFile.deleteFile();
    juce::File (exportPath).withFileExtension (".wav").deleteFile();
}

//==============================================================================
// 12. dataset (docs/plan-batch-pipeline.md) — one command runs the default
// 4-type battery (frequency_response / harmonic / compression / gr_timeline)
// and aggregates everything into a dataset JSON package. Optional blocks:
// "scan" (parameter-sweep family, skipped with "scan":false on bad input) and
// "compression_family" (level x speed grid). Source mapping is fixed:
// freq/harmonic/compression -> signal, gr_timeline -> dynamic. All 8 cases
// are the RED-phase contract for the dataset command (implementation is a
// separate wave); they currently fail with "unknown cmd".
//==============================================================================

TEST_CASE ("CommandParser: dataset runs default 4 types and exports dataset JSON",
           "[commandparser][dataset][dataset-default]")
{
    ensureMessageManager();

    // ---- Arrange ----
    auto plugin = std::make_unique<TestPlugin>();
    plugin->setGain (1.0);
    plugin->prepareToPlay (48000.0, 256);

    MeasurementSession session;
    session.setPluginInstance (plugin.get());
    session.setSampleRate (48000.0);
    session.setBlockSize (256);

    CommandParser parser;
    parser.setPluginInstance (plugin.get());
    parser.setSession (&session);

    // Export under the temp directory; the crash-protection WAV mirror is
    // derived by swapping ".json" for ".wav".
    const juce::File tempDir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                   .getChildFile ("pluginlab_dataset_test");
    tempDir.createDirectory();
    const juce::File jsonPath = tempDir.getChildFile ("dataset_default.json");
    const juce::File wavPath  = jsonPath.withFileExtension (".wav");
    jsonPath.deleteFile();
    wavPath.deleteFile();

    // ---- Act ----
    // No "types" field -> the default 4-type battery.
    const juce::String jsonCmd =
        juce::String (R"({"cmd":"dataset","path":)")
        + juce::JSON::toString (jsonPath.getFullPathName()) + "}";
    auto response = parser.handleCommand (jsonCmd);

    flushMessageManager (200);

    // ---- Assert ----
    // 1. Response: ok, requested export path echoed, per-type success map
    //    with all 4 default keys true.
    auto respJson = juce::JSON::parse (response);
    auto* respObj = respJson.getDynamicObject();
    REQUIRE (respObj != nullptr);
    REQUIRE (respObj->getProperty ("ok") == juce::var (true));
    REQUIRE (respObj->getProperty ("export_path").toString() == jsonPath.getFullPathName());

    auto* types = respObj->getProperty ("types").getDynamicObject();
    REQUIRE (types != nullptr);
    REQUIRE (types->getProperty ("frequency_response") == juce::var (true));
    REQUIRE (types->getProperty ("harmonic") == juce::var (true));
    REQUIRE (types->getProperty ("compression") == juce::var (true));
    REQUIRE (types->getProperty ("gr_timeline") == juce::var (true));

    // 2. Export: parseable dataset JSON with the context block, the GR
    //    timeline block and the three per-type analysis blocks (S1 unit
    //    proxy — the plan extends the Dataset package with the per-type
    //    frequency_response / harmonic / compression blocks).
    REQUIRE (jsonPath.existsAsFile());
    auto exportedJson = juce::JSON::parse (jsonPath.loadFileAsString());
    auto* exportObj = exportedJson.getDynamicObject();
    REQUIRE (exportObj != nullptr);
    REQUIRE (exportObj->getProperty ("type").toString() == "dataset");
    REQUIRE (exportObj->hasProperty ("context"));
    REQUIRE (exportObj->hasProperty ("gr_timeline"));
    REQUIRE (exportObj->hasProperty ("frequency_response"));
    REQUIRE (exportObj->hasProperty ("harmonic"));
    REQUIRE (exportObj->hasProperty ("compression"));

    // Cleanup
    jsonPath.deleteFile();
    wavPath.deleteFile();
    tempDir.deleteRecursively();
}

TEST_CASE ("CommandParser: dataset gr_timeline block matches standalone measure export",
           "[commandparser][dataset][dataset-body-equiv]")
{
    ensureMessageManager();

    // ---- Arrange ----
    auto plugin = std::make_unique<TestPlugin>();
    plugin->setGain (1.0);
    plugin->prepareToPlay (48000.0, 256);

    MeasurementSession session;
    session.setPluginInstance (plugin.get());
    session.setSampleRate (48000.0);
    session.setBlockSize (256);

    CommandParser parser;
    parser.setPluginInstance (plugin.get());
    parser.setSession (&session);

    const juce::File tempDir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                   .getChildFile ("pluginlab_dataset_test");
    tempDir.createDirectory();
    const juce::File datasetPath   = tempDir.getChildFile ("dataset_body_equiv.json");
    const juce::File standalonePath = tempDir.getChildFile ("dataset_standalone_gr.json");
    datasetPath.deleteFile();
    datasetPath.withFileExtension (".wav").deleteFile();
    standalonePath.deleteFile();
    standalonePath.withFileExtension (".wav").deleteFile();

    // ---- Act ----
    // (i) Dataset battery: the gr_timeline block comes from the fixed
    //     dynamic-source mapping.
    auto respDataset = parser.handleCommand (
        juce::String (R"({"cmd":"dataset","path":)")
        + juce::JSON::toString (datasetPath.getFullPathName()) + "}");
    flushMessageManager (200);
    REQUIRE (respDataset.contains ("\"ok\":true"));

    // (ii) Standalone gr_timeline measurement with the same session config.
    auto respStandalone = parser.handleCommand (
        juce::String (R"({"cmd":"measure","source":"dynamic","type":"gr_timeline","path":)")
        + juce::JSON::toString (standalonePath.getFullPathName()) + "}");
    flushMessageManager (200);
    REQUIRE (respStandalone.contains ("\"ok\":true"));

    // ---- Assert ----
    // Both blocks come from the same GainReduction analyzer over the same
    // deterministic dynamic dry/wet pair -> data-equivalent GR bodies.
    auto datasetJson  = juce::JSON::parse (datasetPath.loadFileAsString());
    auto standaloneJson = juce::JSON::parse (standalonePath.loadFileAsString());
    REQUIRE (datasetJson.getDynamicObject() != nullptr);
    REQUIRE (standaloneJson.getDynamicObject() != nullptr);

    auto dsGR = datasetJson["gr_timeline"]["gr"];
    auto stGR = standaloneJson["gr"];
    REQUIRE (dsGR["sample_rate"].equals (stGR["sample_rate"]));
    REQUIRE (dsGR["num_points"].equals (stGR["num_points"]));
    REQUIRE (dsGR["timeline"].size() == stGR["timeline"].size());
    REQUIRE (dsGR["timeline"].size() > 0);
    for (int i = 0; i < dsGR["timeline"].size(); ++i)
    {
        INFO ("GR point " << i);
        REQUIRE (static_cast<double> (dsGR["timeline"][i]["t"]) ==
                 Catch::Approx (static_cast<double> (stGR["timeline"][i]["t"])).margin (1e-4));
        REQUIRE (static_cast<double> (dsGR["timeline"][i]["gr_db"]) ==
                 Catch::Approx (static_cast<double> (stGR["timeline"][i]["gr_db"])).margin (1e-3));
    }

    // Same TimeConstants estimate -> data-equivalent tau blocks.
    auto dsTau = datasetJson["gr_timeline"]["tau"];
    auto stTau = standaloneJson["tau"];
    REQUIRE (dsTau["valid"].equals (stTau["valid"]));
    REQUIRE (static_cast<double> (dsTau["attack_sec"]) ==
             Catch::Approx (static_cast<double> (stTau["attack_sec"])).margin (1e-6));
    REQUIRE (static_cast<double> (dsTau["release_sec"]) ==
             Catch::Approx (static_cast<double> (stTau["release_sec"])).margin (1e-6));

    // Cleanup
    datasetPath.deleteFile();
    datasetPath.withFileExtension (".wav").deleteFile();
    standalonePath.deleteFile();
    standalonePath.withFileExtension (".wav").deleteFile();
    tempDir.deleteRecursively();
}

TEST_CASE ("CommandParser: dataset with bad scan param_id skips scan and keeps types",
           "[commandparser][dataset][dataset-scan-bad]")
{
    ensureMessageManager();

    // ---- Arrange ----
    auto plugin = std::make_unique<TestPlugin>();
    plugin->setGain (1.0);
    plugin->prepareToPlay (48000.0, 256);

    MeasurementSession session;
    session.setPluginInstance (plugin.get());
    session.setSampleRate (48000.0);
    session.setBlockSize (256);

    CommandParser parser;
    parser.setPluginInstance (plugin.get());
    parser.setSession (&session);

    const juce::File tempDir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                   .getChildFile ("pluginlab_dataset_test");
    tempDir.createDirectory();
    const juce::File jsonPath = tempDir.getChildFile ("dataset_scan_bad.json");
    const juce::File wavPath  = jsonPath.withFileExtension (".wav");
    jsonPath.deleteFile();
    wavPath.deleteFile();

    // ---- Act ----
    // Unresolvable param_id -> the scan block is skipped ("scan":false, S3
    // deterministic partial failure); the 4-type battery still completes.
    const juce::String jsonCmd =
        juce::String (R"({"cmd":"dataset","path":)")
        + juce::JSON::toString (jsonPath.getFullPathName())
        + R"(,"scan":{"param_id":"nonexistent","values":[0.5]})" + "}";
    auto response = parser.handleCommand (jsonCmd);

    flushMessageManager (200);

    // ---- Assert ----
    auto respJson = juce::JSON::parse (response);
    auto* respObj = respJson.getDynamicObject();
    REQUIRE (respObj != nullptr);
    REQUIRE (respObj->getProperty ("ok") == juce::var (true));
    REQUIRE (respObj->getProperty ("scan") == juce::var (false));

    auto* types = respObj->getProperty ("types").getDynamicObject();
    REQUIRE (types != nullptr);
    REQUIRE (types->getProperty ("frequency_response") == juce::var (true));
    REQUIRE (types->getProperty ("harmonic") == juce::var (true));
    REQUIRE (types->getProperty ("compression") == juce::var (true));
    REQUIRE (types->getProperty ("gr_timeline") == juce::var (true));

    // The dataset export carries no scan block.
    REQUIRE (jsonPath.existsAsFile());
    auto exportedJson = juce::JSON::parse (jsonPath.loadFileAsString());
    REQUIRE_FALSE (exportedJson.getDynamicObject()->hasProperty ("scan"));

    // Cleanup
    jsonPath.deleteFile();
    wavPath.deleteFile();
    tempDir.deleteRecursively();
}

TEST_CASE ("CommandParser: dataset with valid scan embeds scan block",
           "[commandparser][dataset][dataset-scan-ok]")
{
    ensureMessageManager();

    // ---- Arrange ----
    auto plugin = std::make_unique<TestPlugin>();
    plugin->setGain (1.0);
    plugin->prepareToPlay (48000.0, 256);

    MeasurementSession session;
    session.setPluginInstance (plugin.get());
    session.setSampleRate (48000.0);
    session.setBlockSize (256);

    CommandParser parser;
    parser.setPluginInstance (plugin.get());
    parser.setSession (&session);

    const juce::File tempDir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                   .getChildFile ("pluginlab_dataset_test");
    tempDir.createDirectory();
    const juce::File jsonPath = tempDir.getChildFile ("dataset_scan_ok.json");
    const juce::File wavPath  = jsonPath.withFileExtension (".wav");
    jsonPath.deleteFile();
    wavPath.deleteFile();

    // ---- Act ----
    // TestPlugin hosts a "gain" hosted parameter (stable id "gain", display
    // "Gain"); the scan block mirrors the standalone scan command's field
    // naming (param_id + values).
    const juce::String jsonCmd =
        juce::String (R"({"cmd":"dataset","path":)")
        + juce::JSON::toString (jsonPath.getFullPathName())
        + R"(,"scan":{"param_id":"gain","values":[0.5]})" + "}";
    auto response = parser.handleCommand (jsonCmd);

    flushMessageManager (200);

    // ---- Assert ----
    auto respJson = juce::JSON::parse (response);
    auto* respObj = respJson.getDynamicObject();
    REQUIRE (respObj != nullptr);
    REQUIRE (respObj->getProperty ("ok") == juce::var (true));
    REQUIRE (respObj->getProperty ("scan") == juce::var (true));

    // Export: the embedded scan block mirrors the standalone scan schema —
    // one family entry per scan value.
    REQUIRE (jsonPath.existsAsFile());
    auto exportedJson = juce::JSON::parse (jsonPath.loadFileAsString());
    REQUIRE (exportedJson["scan"]["param_id"].toString() == "gain");
    REQUIRE (exportedJson["scan"]["family"].size() == 1);

    // Cleanup
    jsonPath.deleteFile();
    wavPath.deleteFile();
    tempDir.deleteRecursively();
}

TEST_CASE ("CommandParser: dataset with compression_family embeds grid",
           "[commandparser][dataset][dataset-cf]")
{
    ensureMessageManager();

    // ---- Arrange ----
    // Reference compressor with known dynamics (same configuration as the
    // CompressionFamily tests): threshold -30 dB, ratio 4, attack 5 ms,
    // release 50 ms.
    auto plugin = std::make_unique<TestCompressorPlugin>();
    plugin->setThresholdDB (-30.0);
    plugin->setRatio (4.0);
    plugin->setAttackSec (0.005);
    plugin->setReleaseSec (0.05);
    plugin->setMakeupGainDB (0.0);
    plugin->prepareToPlay (48000.0, 256);

    MeasurementSession session;
    session.setPluginInstance (plugin.get());
    session.setSampleRate (48000.0);
    session.setBlockSize (256);

    CommandParser parser;
    parser.setPluginInstance (plugin.get());
    parser.setSession (&session);

    const juce::File tempDir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                   .getChildFile ("pluginlab_dataset_test");
    tempDir.createDirectory();
    const juce::File jsonPath = tempDir.getChildFile ("dataset_cf.json");
    const juce::File wavPath  = jsonPath.withFileExtension (".wav");
    jsonPath.deleteFile();
    wavPath.deleteFile();

    // ---- Act ----
    // 2 input levels x 3 envelope speeds -> 6 grid cells.
    const juce::String jsonCmd =
        juce::String (R"({"cmd":"dataset","path":)")
        + juce::JSON::toString (jsonPath.getFullPathName())
        + R"(,"compression_family":{"levels_db":[-12,0],"speeds":[0.5,1,2]})" + "}";
    auto response = parser.handleCommand (jsonCmd);

    flushMessageManager (200);

    // ---- Assert ----
    auto respJson = juce::JSON::parse (response);
    auto* respObj = respJson.getDynamicObject();
    REQUIRE (respObj != nullptr);
    REQUIRE (respObj->getProperty ("ok") == juce::var (true));
    REQUIRE (respObj->getProperty ("compression_family") == juce::var (true));

    // Export: one family entry per grid cell (levels x speeds).
    REQUIRE (jsonPath.existsAsFile());
    auto exportedJson = juce::JSON::parse (jsonPath.loadFileAsString());
    REQUIRE (exportedJson["compression_family"]["family"].size() == 6);

    // Cleanup
    jsonPath.deleteFile();
    wavPath.deleteFile();
    tempDir.deleteRecursively();
}

TEST_CASE ("CommandParser: dataset compression_family uses defaults when fields omitted",
           "[commandparser][dataset][dataset-cf-defaults]")
{
    ensureMessageManager();

    // ---- Arrange ----
    // Same reference compressor as the explicit-grid test.
    auto plugin = std::make_unique<TestCompressorPlugin>();
    plugin->setThresholdDB (-30.0);
    plugin->setRatio (4.0);
    plugin->setAttackSec (0.005);
    plugin->setReleaseSec (0.05);
    plugin->setMakeupGainDB (0.0);
    plugin->prepareToPlay (48000.0, 256);

    MeasurementSession session;
    session.setPluginInstance (plugin.get());
    session.setSampleRate (48000.0);
    session.setBlockSize (256);

    CommandParser parser;
    parser.setPluginInstance (plugin.get());
    parser.setSession (&session);

    const juce::File tempDir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                   .getChildFile ("pluginlab_dataset_test");
    tempDir.createDirectory();
    const juce::File jsonPath = tempDir.getChildFile ("dataset_cf_defaults.json");
    const juce::File wavPath  = jsonPath.withFileExtension (".wav");
    jsonPath.deleteFile();
    wavPath.deleteFile();

    // ---- Act ----
    // No levels_db/speeds -> documented defaults [-12.0, 0.0] x [0.5, 1.0, 2.0]
    // (plan-batch-pipeline.md §二, compression_family 缺省语义) -> 6 grid cells.
    const juce::String jsonCmd =
        juce::String (R"({"cmd":"dataset","path":)")
        + juce::JSON::toString (jsonPath.getFullPathName())
        + R"(,"compression_family":{})" + "}";
    auto response = parser.handleCommand (jsonCmd);

    flushMessageManager (200);

    // ---- Assert ----
    auto respJson = juce::JSON::parse (response);
    auto* respObj = respJson.getDynamicObject();
    REQUIRE (respObj != nullptr);
    REQUIRE (respObj->getProperty ("ok") == juce::var (true));
    REQUIRE (respObj->getProperty ("compression_family") == juce::var (true));

    // Export: 2 default levels x 3 default speeds -> 6 family entries.
    REQUIRE (jsonPath.existsAsFile());
    auto exportedJson = juce::JSON::parse (jsonPath.loadFileAsString());
    REQUIRE (exportedJson["compression_family"]["family"].size() == 6);

    // Cleanup
    jsonPath.deleteFile();
    wavPath.deleteFile();
    tempDir.deleteRecursively();
}

TEST_CASE ("CommandParser: dataset rejects unknown type",
           "[commandparser][dataset][dataset-unknown-type]")
{
    ensureMessageManager();

    // ---- Arrange ----
    auto plugin = std::make_unique<TestPlugin>();
    plugin->prepareToPlay (48000.0, 256);

    MeasurementSession session;
    session.setPluginInstance (plugin.get());
    session.setSampleRate (48000.0);
    session.setBlockSize (256);

    CommandParser parser;
    parser.setPluginInstance (plugin.get());
    parser.setSession (&session);

    // ---- Act / Assert ----
    // Unknown measure type -> the whole request fails before any block runs,
    // with the same message the measure command uses.
    auto response = parser.handleCommand (R"({"cmd":"dataset","types":["bogus"]})");
    REQUIRE (response.contains ("\"ok\":false"));
    REQUIRE (response.contains ("\"error\""));
    REQUIRE (response.contains ("unknown measure type"));
}

TEST_CASE ("CommandParser: dataset fails without session or plugin",
           "[commandparser][dataset][dataset-no-session]")
{
    ensureMessageManager();

    // ---- Arrange ----
    CommandParser parser;   // no session, no plugin instance

    // ---- Act ----
    auto response = parser.handleCommand (R"({"cmd":"dataset","path":"Z:\nonexistent\dataset.json"})");

    // ---- Assert: same guard as the measure command (S2). ----
    REQUIRE (response.contains ("\"ok\":false"));
    REQUIRE (response.contains ("\"error\""));
    REQUIRE (response.contains ("no session or plugin"));
}

TEST_CASE ("CommandParser: dataset with no successful blocks returns error",
           "[commandparser][dataset][dataset-all-fail]")
{
    ensureMessageManager();

    // ---- Arrange ----
    auto plugin = std::make_unique<TestPlugin>();
    plugin->prepareToPlay (48000.0, 256);

    MeasurementSession session;
    session.setPluginInstance (plugin.get());
    session.setSampleRate (48000.0);
    session.setBlockSize (256);

    CommandParser parser;
    parser.setPluginInstance (plugin.get());
    parser.setSession (&session);

    // ---- Act ----
    // An empty battery means zero successful blocks -> the whole dataset
    // fails (S2/S3).
    auto response = parser.handleCommand (R"({"cmd":"dataset","types":[]})");

    // ---- Assert ----
    REQUIRE (response.contains ("\"ok\":false"));
    REQUIRE (response.contains ("\"error\""));
    // The dataset command must be dispatched: its own all-fail error, never
    // the unknown-cmd fallback (otherwise this case would pass spuriously).
    REQUIRE_FALSE (response.contains ("unknown cmd"));
}
