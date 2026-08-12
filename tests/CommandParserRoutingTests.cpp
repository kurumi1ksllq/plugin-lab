/**
 * CommandParser D6 routing tests (block D ticket D6 — see
 * docs/plan-block-d-out-of-process.md ADR-D-5/6/7 + 子进程 IPC 协议契约表).
 *
 * A blacklisted plugin is a known host-killer (Pianoteq family): it must
 * NEVER be measured in the host process. The measure command routes to the
 * child-measure callback (ChildMeasureContract) when the loaded plugin's
 * fileOrIdentifier is blacklisted, or fails with an explicit error when no
 * callback is configured (risk-1 ruling: the blacklist isolation fallback is
 * never disabled). Whitelisted plugins keep the host-direct path unchanged.
 *
 * The plugin instance used here is a real TestPlugin whose
 * fillInPluginDescription reports fileOrIdentifier "TestPlugin" — the same
 * resolution buildExportContext uses. Blacklisting "TestPlugin" on a real
 * PluginManager makes isBlacklistedPath (exact-or-prefix, sameBundlePath)
 * match, mirroring the PluginManagerTests blacklist fixture precedent.
 */
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "TestPlugin.h"
#include "../source/ipc/CommandParser.h"
#include "../source/ipc/Protocol.h"
#include "../source/capture/MeasurementSession.h"
#include "../source/host/PluginManager.h"
#include "../source/host/ChildMeasureContract.h"
#include "../source/signal/SineSweep.h"
#include "../source/signal/Impulse.h"
#include "../source/signal/MultiTone.h"

#include <atomic>
#include <functional>
#include <cstring>
#include <cmath>
#include <vector>

//==============================================================================
// Helpers (mirror CommandParserTests.cpp)
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
// Hand-written 24-bit interleaved WAV writer for the child-analysis fixtures
// (mirrors WavCaptureReaderTests.cpp — the same [dry ch0..N-1, wet ch0..N-1]
// 2*numChannels layout the child's CaptureBuffer mirror produces, ADR-D-5).

void writeU16LE (uint8_t* dest, uint16_t value)
{
    dest[0] = static_cast<uint8_t> (value & 0xFF);
    dest[1] = static_cast<uint8_t> ((value >> 8) & 0xFF);
}

void writeU32LE (uint8_t* dest, uint32_t value)
{
    dest[0] = static_cast<uint8_t> (value & 0xFF);
    dest[1] = static_cast<uint8_t> ((value >> 8) & 0xFF);
    dest[2] = static_cast<uint8_t> ((value >> 16) & 0xFF);
    dest[3] = static_cast<uint8_t> ((value >> 24) & 0xFF);
}

int32_t quantize24 (float sample)
{
    return static_cast<int32_t> (juce::jlimit (-1.0f, 1.0f, sample) * 8388607.0f);
}

bool writeTestWav (const juce::File& path,
                   const juce::AudioBuffer<float>& dry,
                   const juce::AudioBuffer<float>& wet,
                   double sampleRate,
                   int bitsPerSample)
{
    const int numChannels = dry.getNumChannels();
    const int totalChannels = 2 * numChannels;
    const int numSamples = dry.getNumSamples();
    const int bytesPerSample = bitsPerSample / 8;
    const uint32_t dataSize = static_cast<uint32_t> (numSamples)
                            * static_cast<uint32_t> (totalChannels)
                            * static_cast<uint32_t> (bytesPerSample);
    const uint32_t bytesPerFrame = static_cast<uint32_t> (totalChannels)
                                 * static_cast<uint32_t> (bytesPerSample);

    path.deleteFile();
    juce::FileOutputStream stream (path, 0);
    if (stream.failedToOpen())
        return false;

    uint8_t header[44] = {};
    std::memcpy (header, "RIFF", 4);
    writeU32LE (header + 4, dataSize + 36);
    std::memcpy (header + 8, "WAVE", 4);
    std::memcpy (header + 12, "fmt ", 4);
    writeU32LE (header + 16, static_cast<uint32_t> (16));
    writeU16LE (header + 20, static_cast<uint16_t> (1));   // PCM
    writeU16LE (header + 22, static_cast<uint16_t> (totalChannels));
    writeU32LE (header + 24, static_cast<uint32_t> (sampleRate));
    writeU32LE (header + 28, static_cast<uint32_t> (sampleRate * static_cast<double> (bytesPerFrame)));
    writeU16LE (header + 32, static_cast<uint16_t> (bytesPerFrame));
    writeU16LE (header + 34, static_cast<uint16_t> (bitsPerSample));
    std::memcpy (header + 36, "data", 4);
    writeU32LE (header + 40, dataSize);

    if (! stream.write (header, sizeof (header)))
        return false;

    std::vector<uint8_t> pcm (static_cast<size_t> (dataSize));
    size_t offset = 0;
    for (int s = 0; s < numSamples; ++s)
    {
        for (int c = 0; c < totalChannels; ++c)
        {
            const float sample = (c < numChannels)
                ? dry.getSample (c, s)
                : wet.getSample (c - numChannels, s);
            const int32_t v = quantize24 (sample);
            pcm[offset++] = static_cast<uint8_t> (v & 0xFF);
            pcm[offset++] = static_cast<uint8_t> ((v >> 8) & 0xFF);
            pcm[offset++] = static_cast<uint8_t> ((v >> 16) & 0xFF);
        }
    }

    const bool ok = stream.write (pcm.data(), pcm.size());
    stream.flush();
    return ok;
}

//==============================================================================
// Deterministic dry/wet fixtures for the child-analysis path: a pure delay
// (all-pass → 0 dB response after latency compensation) as the plugin
// transfer function, mirroring the WavCaptureReaderTests fixtures.

juce::AudioBuffer<float> generateSweep (double sr, double durationSec, double amplitude)
{
    SineSweep sw;
    sw.setFrequencyRange (20.0, 20000.0);
    sw.setDuration (durationSec);
    sw.setAmplitude (amplitude);
    sw.prepare (sr, 512);

    const int totalSamples = static_cast<int> (sr * durationSec);
    juce::AudioBuffer<float> buf (1, totalSamples);
    buf.clear();
    sw.generate (buf, 0, totalSamples);
    return buf;
}

juce::AudioBuffer<float> generateMLS (double sr, int mlsLength, double amplitude)
{
    Impulse imp;
    imp.useMLS (true);
    imp.setMLSLength (mlsLength);
    imp.setAmplitude (amplitude);
    imp.prepare (sr, 512);

    juce::AudioBuffer<float> buf (1, mlsLength * 2);
    buf.clear();
    imp.generate (buf, 0, mlsLength);         // period 1 (plugin warm-up)
    imp.reset();
    imp.generate (buf, mlsLength, mlsLength); // period 2 (steady state)
    return buf;
}

juce::AudioBuffer<float> delayCopy (const juce::AudioBuffer<float>& src, int delaySamples)
{
    const int totalSamples = src.getNumSamples();
    juce::AudioBuffer<float> dst (1, totalSamples);
    dst.clear();
    const float* srcData = src.getReadPointer (0);
    float* dstData = dst.getWritePointer (0);

    for (int i = delaySamples; i < totalSamples; ++i)
        dstData[i] = srcData[i - delaySamples];

    return dst;
}

/** MultiTone fixture mirroring the host harmonic generator config
 *  (MeasurementSession.cpp Type::harmonicAnalysis branch): 8 octave
 *  fundamentals 100..12800 Hz, `durationSec`, `amplitude` — must stay in
 *  lockstep with ChildWavAnalyzer::analyzeChildHarmonic and the child's
 *  handleMeasure branch. */
juce::AudioBuffer<float> generateMultiTone (double sr, double durationSec, double amplitude)
{
    MultiTone mt;
    mt.setDuration (durationSec);
    mt.setAmplitude (amplitude);
    mt.setFrequencies ({ 100.0, 200.0, 400.0, 800.0, 1600.0, 3200.0, 6400.0, 12800.0 });
    mt.prepare (sr, 512);

    const int totalSamples = static_cast<int> (sr * durationSec);
    juce::AudioBuffer<float> buf (1, totalSamples);
    buf.clear();
    mt.generate (buf, 0, totalSamples);
    return buf;
}

juce::File tempWav (const juce::String& prefix)
{
    return juce::File::getSpecialLocation (juce::File::SpecialLocationType::tempDirectory)
               .getNonexistentChildFile (prefix, ".wav");
}

//==============================================================================
// Fixture: TestPlugin + MeasurementSession + real PluginManager
//==============================================================================

namespace
{
    /** Shared arrangement for the D6 routing tests. `blacklistEntry` is
     *  added to the real PluginManager blacklist ("" = no entry → the
     *  plugin is whitelisted); `withCallback` wires the child-measure
     *  callback; `withPlugin` sets the TestPlugin instance on the parser
     *  (false = the blacklisted-plugin scenario where the host never loaded
     *  it — B+ decision — so plugin stays nullptr). The callback records
     *  invocations + the request and returns the fixture's outcomeOverride. */
    struct RoutingFixture
    {
        TestPlugin plugin;
        MeasurementSession session;
        PluginManager manager;
        CommandParser parser;

        std::atomic<int> childCalls { 0 };
        std::atomic<bool> hostCompleteFired { false };
        ChildMeasureContract::ChildMeasureRequest lastRequest;
        ChildMeasureContract::ChildMeasureOutcome outcomeOverride;

        RoutingFixture (const juce::String& blacklistEntry, bool withCallback,
                        bool withPlugin = true)
        {
            ensureMessageManager();

            plugin.prepareToPlay (44100.0, 256);

            session.setPluginInstance (&plugin);
            session.setSampleRate (44100.0);
            session.setBlockSize (256);
            session.setMeasurementType (MeasurementSession::Type::frequencyResponse);

            if (blacklistEntry.isNotEmpty())
                manager.addToBlacklistLocked (blacklistEntry);

            if (withPlugin)
                parser.setPluginInstance (&plugin);
            parser.setSession (&session);
            parser.setPluginManager (&manager);

            // Host-side completion — must never fire on the child path.
            parser.setMeasurementCompleteCallback ([this] (const MeasurementResults&) {
                hostCompleteFired.store (true);
            });

            if (withCallback)
            {
                parser.setChildMeasureCallback ([this] (const ChildMeasureContract::ChildMeasureRequest& req) {
                    childCalls.fetch_add (1);
                    lastRequest = req;
                    return outcomeOverride;
                });
            }
        }

        /** Success outcome pointing at real, writable files: `wavPath` is a
         *  written 24-bit dry/wet fixture the child-analysis path reads
         *  (ADR-D-5 layout), `exportPath` is where that path writes the
         *  frequency_response JSON (ADR-D-6). A 1-channel plugin fixture
         *  delayed by 97 samples — the analysis must succeed for the
         *  response to be ok:true. */
        static ChildMeasureContract::ChildMeasureOutcome okOutcome (const juce::String& wavPath,
                                                                    const juce::String& exportPath)
        {
            ChildMeasureContract::ChildMeasureOutcome outcome;
            outcome.ok = true;
            outcome.result.samples        = 88200;
            outcome.result.rate           = 44100.0;
            outcome.result.exportPath     = exportPath;
            outcome.result.wavPath        = wavPath;
            outcome.result.name           = "ChildPlugin";
            outcome.result.classId        = "child.class.id";
            outcome.result.channels       = 1;
            outcome.result.latencySamples = 97;
            return outcome;
        }

        /** Builds a measure command JSON with a safely-escaped export path. */
        static juce::String measureCommand (const juce::String& type,
                                            const juce::String& extraFields,
                                            const juce::String& exportPath)
        {
            return juce::String (R"({"cmd":"measure","type":")") + type + "\""
                 + (extraFields.isNotEmpty() ? "," + extraFields : juce::String())
                 + R"(,"path":)" + juce::JSON::toString (exportPath) + "}";
        }
    };
}  // namespace

//==============================================================================
// R1 — blacklisted → child callback invoked exactly once, request mapped
//==============================================================================

TEST_CASE ("CommandParser: blacklisted plugin routes measure to child callback with mapped request",
           "[commandparser][routing][d6]")
{
    // ---- Arrange ----
    const juce::String exportPath =
        juce::File::getCurrentWorkingDirectory()
            .getChildFile ("test_child_route.json")
            .getFullPathName();

    // Real dry/wet WAV fixture the child-analysis path reads (ADR-D-5
    // layout): an MLS sequence delayed by 97 samples (all-pass), 1 plugin
    // channel → 2 interleaved channels. MLS matches the "mls" excitation
    // below (and the session's default freqMLSLength = 16383).
    const auto wavPath = tempWav ("pluginlab_route_r1_").getFullPathName();
    const auto childJson = juce::File (wavPath).withFileExtension (".json").getFullPathName();
    {
        const auto dry = generateMLS (44100.0, 16383, 0.5);
        const auto wet = delayCopy (dry, 97);
        REQUIRE (writeTestWav (juce::File (wavPath), dry, wet, 44100.0, 24));
    }

    // "TestPlugin" is the fileOrIdentifier TestPlugin::fillInPluginDescription
    // reports — the same resolution the routing and buildExportContext use.
    RoutingFixture f ("TestPlugin", true);
    f.outcomeOverride = RoutingFixture::okOutcome (wavPath, childJson);

    juce::File (exportPath).deleteFile();
    juce::File (exportPath).withFileExtension (".wav").deleteFile();
    juce::File (childJson).deleteFile();

    // ---- Act ----
    const auto response = f.parser.handleCommand (
        RoutingFixture::measureCommand ("frequency_response", R"("excitation":"mls")", exportPath));
    flushMessageManager (200);

    // ---- Assert: callback fired exactly once, request fields mapped ----
    REQUIRE (f.childCalls.load() == 1);
    REQUIRE_FALSE (f.hostCompleteFired.load());   // host never measured

    REQUIRE (f.lastRequest.type == "frequency_response");
    REQUIRE (f.lastRequest.excitation == "mls");  // mls honoured (freq type)
    REQUIRE (f.lastRequest.sampleRate == Catch::Approx (44100.0));
    REQUIRE (f.lastRequest.blockSize == 256);
    REQUIRE (f.lastRequest.exportPath == exportPath);

    // Documented wavPathFor rule: a trailing ".json" becomes ".wav"
    // (independent expression of the rule — not the production code path).
    REQUIRE (f.lastRequest.wavPath == exportPath.dropLastCharacters (5) + ".wav");

    // ---- Assert: response mirrors the child result ----
    const auto json = juce::JSON::parse (response);
    auto* obj = json.getDynamicObject();
    REQUIRE (obj != nullptr);
    REQUIRE (static_cast<bool> (obj->getProperty ("ok")));
    REQUIRE (obj->getProperty ("samples") == juce::var (static_cast<int64_t> (88200)));
    REQUIRE (static_cast<double> (obj->getProperty ("rate")) == Catch::Approx (44100.0));
    REQUIRE (obj->getProperty ("export_path").toString() == childJson);
    REQUIRE (obj->getProperty ("wav_path").toString() == wavPath);
    REQUIRE (obj->getProperty ("name").toString() == "ChildPlugin");
    REQUIRE (obj->getProperty ("class_id").toString() == "child.class.id");
    REQUIRE (obj->getProperty ("channels") == juce::var (1));
    REQUIRE (obj->getProperty ("latency_samples") == juce::var (97));

    // ---- Assert: host path did not run — the command's export path stays
    // untouched, but the child-analysis path wrote the frequency JSON ----
    REQUIRE_FALSE (juce::File (exportPath).existsAsFile());
    REQUIRE_FALSE (juce::File (exportPath).withFileExtension (".wav").existsAsFile());
    REQUIRE (juce::File (childJson).existsAsFile());          // gap-2 wiring
    REQUIRE (juce::File (childJson).getSize() > 0);

    // ---- Cleanup ----
    juce::File (exportPath).deleteFile();
    juce::File (exportPath).withFileExtension (".wav").deleteFile();
    juce::File (childJson).deleteFile();
    juce::File (wavPath).deleteFile();
}

//==============================================================================
// R2 — whitelisted → host-direct path, child callback never called
//==============================================================================

TEST_CASE ("CommandParser: whitelisted plugin keeps host-direct path, child callback never called",
           "[commandparser][routing][d6]")
{
    // ---- Arrange ----
    const juce::String exportPath =
        juce::File::getCurrentWorkingDirectory()
            .getChildFile ("test_whitelist_route.json")
            .getFullPathName();

    // Blacklist holds an unrelated entry: TestPlugin's fileOrIdentifier
    // ("TestPlugin") is not blacklisted → whitelisted.
    RoutingFixture f ("C:\\fake\\unrelated.vst3", true);

    juce::File (exportPath).deleteFile();
    juce::File (exportPath).withFileExtension (".wav").deleteFile();

    // ---- Act ----
    const auto response = f.parser.handleCommand (
        RoutingFixture::measureCommand ("frequency_response", {}, exportPath));
    flushMessageManager (200);

    // ---- Assert ----
    REQUIRE (f.childCalls.load() == 0);                    // never routed to child
    REQUIRE (response.contains ("\"ok\":true"));           // host measurement ran
    REQUIRE (response.contains ("\"samples\":"));
    REQUIRE (response.contains ("\"export_path\":"));
    REQUIRE (f.hostCompleteFired.load());                  // host completion fired
    REQUIRE (juce::File (exportPath).existsAsFile());      // export written by host path

    // ---- Cleanup ----
    juce::File (exportPath).deleteFile();
    juce::File (exportPath).withFileExtension (".wav").deleteFile();
}

//==============================================================================
// R3 — blacklisted + no callback → explicit error, never host-direct
//==============================================================================

TEST_CASE ("CommandParser: blacklisted plugin without child callback fails explicitly, never host-direct",
           "[commandparser][routing][d6]")
{
    // ---- Arrange ----
    const juce::String exportPath =
        juce::File::getCurrentWorkingDirectory()
            .getChildFile ("test_no_callback_route.json")
            .getFullPathName();

    RoutingFixture f ("TestPlugin", false);   // no child callback configured

    juce::File (exportPath).deleteFile();
    juce::File (exportPath).withFileExtension (".wav").deleteFile();

    // ---- Act ----
    const auto response = f.parser.handleCommand (
        RoutingFixture::measureCommand ("frequency_response", {}, exportPath));
    flushMessageManager (200);

    // ---- Assert: explicit error, isolation never disabled ----
    REQUIRE (response.contains ("\"ok\":false"));
    REQUIRE (response.contains ("child measurement not configured"));
    REQUIRE_FALSE (f.hostCompleteFired.load());            // host path not run
    REQUIRE_FALSE (juce::File (exportPath).existsAsFile());          // no export
    REQUIRE_FALSE (juce::File (exportPath).withFileExtension (".wav").existsAsFile());

    // ---- Cleanup ----
    juce::File (exportPath).deleteFile();
    juce::File (exportPath).withFileExtension (".wav").deleteFile();
}

//==============================================================================
// R4 — callback !ok → error passthrough (ADR-D-7; gr_timeline is still a
//      rejected child type, deferred to a separate issue)
//==============================================================================

TEST_CASE ("CommandParser: child callback failure passes error through (ADR-D-7 passthrough)",
           "[commandparser][routing][d6]")
{
    // ---- Arrange ----
    const juce::String exportPath =
        juce::File::getCurrentWorkingDirectory()
            .getChildFile ("test_child_error_route.json")
            .getFullPathName();

    RoutingFixture f ("TestPlugin", true);
    // Default outcomeOverride has ok=false; the orchestrator (ADR-D-7) rejects
    // gr_timeline — CommandParser must pass the error through and never fall
    // back to host-direct measurement.
    f.outcomeOverride.error = "child measurement not implemented for type 'gr_timeline'";

    juce::File (exportPath).deleteFile();
    juce::File (exportPath).withFileExtension (".wav").deleteFile();

    // ---- Act ----
    // gr_timeline requires a non-signal source to pass CommandParser's
    // pre-validation (CommandParser.cpp:635-638) — dynamic reaches the child
    // path, where the orchestrator still rejects it (ADR-D-7).
    const auto response = f.parser.handleCommand (
        RoutingFixture::measureCommand ("gr_timeline", R"("source":"dynamic")", exportPath));
    flushMessageManager (200);

    // ---- Assert: error passthrough, request still delivered once ----
    REQUIRE (response.contains ("\"ok\":false"));
    REQUIRE (response.contains ("child measurement not implemented for type 'gr_timeline'"));
    REQUIRE (f.childCalls.load() == 1);
    REQUIRE (f.lastRequest.type == "gr_timeline");
    REQUIRE_FALSE (f.hostCompleteFired.load());
    REQUIRE_FALSE (juce::File (exportPath).existsAsFile());

    // ---- Cleanup ----
    juce::File (exportPath).deleteFile();
    juce::File (exportPath).withFileExtension (".wav").deleteFile();
}

//==============================================================================
// R5 — child response shape aligns with host-direct measurement
//==============================================================================

TEST_CASE ("CommandParser: child response shape aligns with host-direct measurement",
           "[commandparser][routing][d6]")
{
    // ---- Arrange ----
    const juce::String hostExport =
        juce::File::getCurrentWorkingDirectory()
            .getChildFile ("test_shape_host.json")
            .getFullPathName();
    const juce::String childExport =
        juce::File::getCurrentWorkingDirectory()
            .getChildFile ("test_shape_child.json")
            .getFullPathName();

    // Host-direct baseline: whitelisted (no blacklist entry).
    RoutingFixture hostFixture ("", true);
    // Child path: blacklisted + fake callback reporting a real WAV — the
    // child-analysis path must succeed for the response to be ok:true
    // (ADR-D-6 metadata shape on top of the shared host fields).
    const auto wavPath = tempWav ("pluginlab_route_shape_").getFullPathName();
    const auto childResultJson = juce::File (wavPath).withFileExtension (".json").getFullPathName();
    {
        const auto dry = generateSweep (44100.0, 2.0, 0.5);
        const auto wet = delayCopy (dry, 97);
        REQUIRE (writeTestWav (juce::File (wavPath), dry, wet, 44100.0, 24));
    }
    RoutingFixture childFixture ("TestPlugin", true);
    childFixture.outcomeOverride = RoutingFixture::okOutcome (wavPath, childResultJson);

    juce::File (hostExport).deleteFile();
    juce::File (hostExport).withFileExtension (".wav").deleteFile();
    juce::File (childExport).deleteFile();
    juce::File (childExport).withFileExtension (".wav").deleteFile();
    juce::File (childResultJson).deleteFile();

    // ---- Act: the same measure command through both paths ----
    const auto hostResponse = hostFixture.parser.handleCommand (
        RoutingFixture::measureCommand ("frequency_response", {}, hostExport));
    flushMessageManager (200);

    const auto childResponse = childFixture.parser.handleCommand (
        RoutingFixture::measureCommand ("frequency_response", {}, childExport));
    flushMessageManager (200);

    // ---- Assert: field alignment ----
    const auto hostJson = juce::JSON::parse (hostResponse);
    const auto childJson = juce::JSON::parse (childResponse);
    auto* ho = hostJson.getDynamicObject();
    auto* co = childJson.getDynamicObject();
    REQUIRE (ho != nullptr);
    REQUIRE (co != nullptr);
    REQUIRE (static_cast<bool> (ho->getProperty ("ok")));
    REQUIRE (static_cast<bool> (co->getProperty ("ok")));

    // The four fields the host path returns must be present on both responses
    // (samples/rate/export_path/wav_path — CommandParser.cpp:639-643).
    static const char* sharedFields[] = { "samples", "rate", "export_path", "wav_path" };
    for (const char* field : sharedFields)
    {
        REQUIRE (ho->hasProperty (field));
        REQUIRE (co->hasProperty (field));
    }

    // Child path additionally reports plugin identity + channel metadata
    // (ADR-D-6; host path cannot, it has no plugin instance).
    static const char* childOnlyFields[] = { "name", "class_id", "channels", "latency_samples" };
    for (const char* field : childOnlyFields)
        REQUIRE (co->hasProperty (field));

    // The host path must have run for the baseline (export file written);
    // the child analysis path wrote its own result JSON at the outcome path
    // (not the command's export path).
    REQUIRE (juce::File (hostExport).existsAsFile());
    REQUIRE (juce::File (childResultJson).existsAsFile());
    REQUIRE_FALSE (juce::File (childExport).existsAsFile());

    // ---- Cleanup ----
    juce::File (hostExport).deleteFile();
    juce::File (hostExport).withFileExtension (".wav").deleteFile();
    juce::File (childExport).deleteFile();
    juce::File (childExport).withFileExtension (".wav").deleteFile();
    juce::File (childResultJson).deleteFile();
    juce::File (wavPath).deleteFile();
}

//==============================================================================
// R6a — gap 1: no host plugin instance + child measure path → routes
//==============================================================================

TEST_CASE ("CommandParser: no host plugin + child measure path routes blacklisted plugin to child",
           "[commandparser][routing][d6]")
{
    // ---- Arrange ----
    // Blacklisted path — the B+ decision means the host never loaded it, so
    // the fixture leaves plugin == nullptr on the parser (withPlugin=false).
    // The host load path would have called setChildMeasurePath with the
    // plugin's fileOrIdentifier.
    const juce::String childPath = "C:\\fake\\blacklisted.vst3";

    // Real MLS fixture matching the "mls" excitation below.
    const auto wavPath = tempWav ("pluginlab_route_noplugin_").getFullPathName();
    const auto childJson = juce::File (wavPath).withFileExtension (".json").getFullPathName();
    {
        const auto dry = generateMLS (44100.0, 16383, 0.5);
        const auto wet = delayCopy (dry, 97);
        REQUIRE (writeTestWav (juce::File (wavPath), dry, wet, 44100.0, 24));
    }

    RoutingFixture f (childPath, true, false);
    f.outcomeOverride = RoutingFixture::okOutcome (wavPath, childJson);
    f.parser.setChildMeasurePath (childPath);

    juce::File (childJson).deleteFile();

    // ---- Act ----
    const auto response = f.parser.handleCommand (
        RoutingFixture::measureCommand ("frequency_response", R"("excitation":"mls")", childJson));
    flushMessageManager (200);

    // ---- Assert: routed to the child despite no host plugin instance ----
    REQUIRE (f.childCalls.load() == 1);
    REQUIRE_FALSE (f.hostCompleteFired.load());   // host never measured
    REQUIRE (f.lastRequest.type == "frequency_response");
    REQUIRE (f.lastRequest.excitation == "mls");
    REQUIRE (f.lastRequest.sampleRate == Catch::Approx (44100.0));
    REQUIRE (f.lastRequest.blockSize == 256);

    // The analysis path ran end to end: ok response + export file written.
    REQUIRE (response.contains ("\"ok\":true"));
    REQUIRE (juce::File (childJson).existsAsFile());

    // ---- Cleanup ----
    juce::File (childJson).deleteFile();
    juce::File (wavPath).deleteFile();
}

//==============================================================================
// R6b — gap 1: no host plugin, no (or non-blacklisted) child path → refused
//==============================================================================

TEST_CASE ("CommandParser: no host plugin without child measure path fails with no session or plugin",
           "[commandparser][routing][d6]")
{
    // ---- Arrange ----
    const juce::String exportPath =
        juce::File::getCurrentWorkingDirectory()
            .getChildFile ("test_no_plugin_route.json")
            .getFullPathName();

    RoutingFixture f ("", false, false);            // no plugin, no callback
    f.parser.setChildMeasurePath ("");              // and no child target

    // ---- Act / Assert: empty path → "no session or plugin", never routed ----
    const auto response = f.parser.handleCommand (
        RoutingFixture::measureCommand ("frequency_response", {}, exportPath));
    REQUIRE (response.contains ("\"ok\":false"));
    REQUIRE (response.contains ("no session or plugin"));
    REQUIRE (f.childCalls.load() == 0);
    REQUIRE_FALSE (f.hostCompleteFired.load());

    // ---- A non-empty but NON-blacklisted target path is refused too: a
    // cleared blacklist (UI "clear blacklist and rescan") must not silently
    // route, and must never fall back to host-direct measurement. ----
    RoutingFixture f2 ("", false, false);
    f2.parser.setChildMeasurePath ("C:\\fake\\whitelisted.vst3");
    const auto response2 = f2.parser.handleCommand (
        RoutingFixture::measureCommand ("frequency_response", {}, exportPath));
    REQUIRE (response2.contains ("\"ok\":false"));
    REQUIRE (response2.contains ("no session or plugin"));
    REQUIRE (f2.childCalls.load() == 0);
    REQUIRE_FALSE (f2.hostCompleteFired.load());

    // ---- Cleanup ----
    juce::File (exportPath).deleteFile();
    juce::File (exportPath).withFileExtension (".wav").deleteFile();
}

//==============================================================================
// R7 — gap 2: child measurement writes the analyzed export JSON (ADR-D-6)
//==============================================================================

TEST_CASE ("CommandParser: child measurement writes the analyzed frequency-response export (ADR-D-6)",
           "[commandparser][routing][d6]")
{
    // ---- Arrange ----
    // Sweep fixture (default excitation): a 2 s sweep delayed by 97 samples
    // (all-pass → 0 dB / 0° after latency compensation).
    const auto wavPath = tempWav ("pluginlab_route_analyze_").getFullPathName();
    const auto childJson = juce::File (wavPath).withFileExtension (".json").getFullPathName();
    {
        const auto dry = generateSweep (44100.0, 2.0, 0.5);
        const auto wet = delayCopy (dry, 97);
        REQUIRE (writeTestWav (juce::File (wavPath), dry, wet, 44100.0, 24));
    }

    RoutingFixture f ("TestPlugin", true);
    f.outcomeOverride = RoutingFixture::okOutcome (wavPath, childJson);

    juce::File (childJson).deleteFile();

    // ---- Act ----
    const auto response = f.parser.handleCommand (
        RoutingFixture::measureCommand ("frequency_response", {}, childJson));
    flushMessageManager (200);

    // ---- Assert: ok + the export file exists and holds the analysis ----
    REQUIRE (response.contains ("\"ok\":true"));
    REQUIRE (f.childCalls.load() == 1);
    REQUIRE (juce::File (childJson).existsAsFile());
    REQUIRE (juce::File (childJson).getSize() > 0);

    const auto doc = juce::JSON::parse (juce::File (childJson).loadFileAsString());
    REQUIRE (doc.isObject());
    REQUIRE (doc["type"].toString() == "frequency_response");
    // Context built from the child-reported metadata (ADR-D-6).
    REQUIRE (doc["plugin"].toString() == "ChildPlugin");
    REQUIRE (doc["class_id"].toString() == "child.class.id");
    REQUIRE (doc["latency_samples"] == juce::var (97));
    REQUIRE (doc["sample_rate"] == juce::var (44100.0));
    REQUIRE (doc["raw"].isArray());
    REQUIRE (doc["raw"].size() > 0);

    // ---- Cleanup ----
    juce::File (childJson).deleteFile();
    juce::File (wavPath).deleteFile();
}

//==============================================================================
// R8 — gap 2: unreadable child WAV → explicit failure, never ok with a
// missing export file
//==============================================================================

TEST_CASE ("CommandParser: child measurement with an unreadable WAV fails explicitly",
           "[commandparser][routing][d6]")
{
    // ---- Arrange ----
    // The callback reports a WAV path that does not exist (or is not a
    // valid 24-bit dry/wet mirror) — the analysis entry must fail.
    const auto missingWav = tempWav ("pluginlab_route_missing_").getFullPathName();
    const auto childJson = juce::File (missingWav).withFileExtension (".json").getFullPathName();

    RoutingFixture f ("TestPlugin", true);
    f.outcomeOverride = RoutingFixture::okOutcome (missingWav, childJson);

    juce::File (childJson).deleteFile();

    // ---- Act ----
    const auto response = f.parser.handleCommand (
        RoutingFixture::measureCommand ("frequency_response", {}, childJson));
    flushMessageManager (200);

    // ---- Assert: explicit failure, no export file, callback still reached ----
    REQUIRE (response.contains ("\"ok\":false"));
    REQUIRE (response.contains ("child measurement failed"));
    REQUIRE (f.childCalls.load() == 1);
    REQUIRE_FALSE (f.hostCompleteFired.load());
    REQUIRE_FALSE (juce::File (childJson).existsAsFile());

    // ---- Cleanup ----
    juce::File (childJson).deleteFile();
}

//==============================================================================
// R9 — T1: measure {type: harmonic} on a blacklisted plugin routes to the
//      child AND the host analyzes the child's WAV into a harmonic_analysis
//      export (dispatch to ChildWavAnalyzer::analyzeChildHarmonic).
//==============================================================================

TEST_CASE ("CommandParser: harmonic measure routes to child and writes the harmonic analysis export (T1)",
           "[commandparser][routing][d6][childharmonic]")
{
    // ---- Arrange ----
    // MultiTone fixture (identity dry/wet — the octave collisions alone
    // produce harmonics, so the harmonic analysis yields 7 tones): mirrors
    // the host harmonic generator config.
    const auto wavPath = tempWav ("pluginlab_route_har_").getFullPathName();
    const auto childJson = juce::File (wavPath).withFileExtension (".json").getFullPathName();
    {
        const auto dry = generateMultiTone (44100.0, 2.0, 0.4);
        juce::AudioBuffer<float> wet (dry);
        REQUIRE (writeTestWav (juce::File (wavPath), dry, wet, 44100.0, 24));
    }

    RoutingFixture f ("TestPlugin", true);
    f.outcomeOverride = RoutingFixture::okOutcome (wavPath, childJson);

    juce::File (childJson).deleteFile();

    // ---- Act ----
    const auto response = f.parser.handleCommand (
        RoutingFixture::measureCommand ("harmonic", {}, childJson));
    flushMessageManager (200);

    // ---- Assert: routed to child, request type forwarded ----
    REQUIRE (response.contains ("\"ok\":true"));
    REQUIRE (f.childCalls.load() == 1);
    REQUIRE_FALSE (f.hostCompleteFired.load());
    REQUIRE (f.lastRequest.type == "harmonic");
    // Non-freq types force sweep excitation (CommandParser.cpp:662-665).
    REQUIRE (f.lastRequest.excitation == "sweep");

    // ---- Assert: the harmonic analysis entry ran end to end ----
    REQUIRE (juce::File (childJson).existsAsFile());
    REQUIRE (juce::File (childJson).getSize() > 0);

    const auto doc = juce::JSON::parse (juce::File (childJson).loadFileAsString());
    REQUIRE (doc.isObject());
    REQUIRE (doc["type"].toString() == "harmonic_analysis");
    REQUIRE (doc["plugin"].toString() == "ChildPlugin");
    REQUIRE (doc["class_id"].toString() == "child.class.id");
    const auto tones = doc["tones"].getArray();
    REQUIRE (tones != nullptr);
    REQUIRE (tones->size() == 7);   // 8-fundamental config, 12800 Hz dropped

    // ---- Cleanup ----
    juce::File (childJson).deleteFile();
    juce::File (wavPath).deleteFile();
}
