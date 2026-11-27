/**
 * ChildHostParityTests (block D, T3): end-to-end parity between the
 * out-of-process measurement path (PluginHostChild + WAV transit +
 * ChildWavAnalyzer, D2a+D2b) and the in-process reference path (plugin
 * loaded directly in the test process + the same analysis entry).
 *
 * Acceptance from docs/plan-block-d-out-of-process.md D2b:
 * "Pro-Q 4 经子进程测量与宿主直测一致（<0.5dB）" — verified here with the real
 * magic.CURVE VST3 (CHILD_PARITY_PLUGIN). Both sides run the SAME generator
 * through the SAME frozen SweepRunner pipeline, write their dry/wet to a
 * 24-bit WAV, and the SAME ChildWavAnalyzer entry turns each into export
 * JSON. The two raw magnitude curves are compared point-by-point in
 * 100 Hz – 10 kHz (mirroring FreqResponseTests.cpp:198) with |ΔdB| < 0.5.
 *
 *  If the real plugin is absent from the machine (CHILD_PARITY_PLUGIN does
 *  not exist), the tests SKIP with the reason logged — they never substitute
 *  a fake plugin for the real one.
 *
 *  T6 (issue #15): the same parity for the gr_timeline dynamic measurement —
 *  child-process enveloped-sweep run vs the in-process reference, compared on
 *  the GR timeline (driven region < 0.5 dB) and the tau estimates (0.7x-1.4x).
 */
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "../source/host/ChildProcessCoordinator.h"
#include "../source/host/PluginManager.h"
#include "../source/host/ChildMeasureContract.h"
#include "../source/capture/MeasurementSession.h"
#include "../source/ipc/CommandParser.h"
#include "TestPlugin.h"
#include "../source/analysis/ChildWavAnalyzer.h"
#include "../source/capture/SweepRunner.h"
#include "../source/signal/SineSweep.h"
#include "../source/signal/Impulse.h"
#include "../source/signal/EnvelopeSignal.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <thread>
#include <vector>

#ifndef PLUGIN_HOST_CHILD_EXE
#error "PLUGIN_HOST_CHILD_EXE must be defined by tests/CMakeLists.txt"
#endif

#ifndef CHILD_PARITY_PLUGIN
#error "CHILD_PARITY_PLUGIN must be defined by tests/CMakeLists.txt"
#endif

#ifndef PLUGINLAB_SOURCE_DIR
#error "PLUGINLAB_SOURCE_DIR must be defined by tests/CMakeLists.txt"
#endif

namespace
{
    //==============================================================================
    juce::File realChildExe()
    {
        return juce::File (juce::String (PLUGIN_HOST_CHILD_EXE));
    }

    juce::File realPluginFile()
    {
        return juce::File (juce::String (CHILD_PARITY_PLUGIN));
    }

    juce::File tempFile (const juce::String& prefix, const juce::String& suffix)
    {
        return juce::File::getSpecialLocation (juce::File::SpecialLocationType::tempDirectory)
                   .getNonexistentChildFile (prefix, suffix);
    }

    /** Poll `flag` until it turns true or timeoutMs elapses. */
    bool waitFor (const std::atomic<bool>& flag, int timeoutMs)
    {
        const auto deadline = std::chrono::steady_clock::now()
                            + std::chrono::milliseconds (timeoutMs);
        while (! flag.load() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for (std::chrono::milliseconds (10));
        return flag.load();
    }

    /** Make sure a JUCE MessageManager exists and THIS thread is the message
     *  thread (JUCE single-threaded mode): createPluginInstanceAsync posts
     *  creation to the message thread, which we pump with runDispatchLoopUntil
     *  while waiting (mirror of PluginHostChild.cpp handleLoad). */
    void ensureMessageManager()
    {
        if (juce::MessageManager::getInstanceWithoutCreating() == nullptr)
            juce::MessageManager::getInstance();
    }

    //==============================================================================
    // Hand-written 24-bit interleaved WAV writer ([dry ch0..N-1, wet ch0..N-1])
    // — same layout as CaptureBuffer::flush / WavExporter (see
    // WavCaptureReaderTests.cpp for the sibling fixture).

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

    bool writeTestWav (const juce::File& path,
                       const juce::AudioBuffer<float>& dry,
                       const juce::AudioBuffer<float>& wet,
                       double sampleRate)
    {
        const int numChannels = dry.getNumChannels();
        const int totalChannels = 2 * numChannels;
        const int numSamples = dry.getNumSamples();
        const uint32_t dataSize = static_cast<uint32_t> (numSamples)
                                * static_cast<uint32_t> (totalChannels) * 3u;
        const uint32_t bytesPerFrame = static_cast<uint32_t> (totalChannels) * 3u;

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
        writeU16LE (header + 34, static_cast<uint16_t> (24));
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
                const int32_t v = static_cast<int32_t> (
                    juce::jlimit (-1.0f, 1.0f, sample) * 8388607.0f);
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
    // Child-protocol helpers (contract: docs/plan-block-d-out-of-process.md
    // "子进程 IPC 协议契约"; progress lines refresh liveness and are consumed
    // silently).

    /** Pop lines until one contains `needle` (progress lines pass through
     *  silently). Returns the matching line or empty on timeout. */
    juce::String popUntil (PluginHostChildCoordinator& coord, const juce::String& needle,
                           int timeoutMs)
    {
        const auto deadline = juce::Time::getMillisecondCounter()
                            + static_cast<juce::uint32> (timeoutMs);
        while (juce::Time::getMillisecondCounter() < deadline)
        {
            const auto line = coord.popLine (200);
            if (line.isEmpty())
                continue;
            if (line.contains (needle))
                return line;
        }
        return {};
    }

    /** start → load → measure (type configurable, default frequency_response
     *  keeps the P1/P2 call sites unchanged); returns the measure result line
     *  (contains "samples") or empty on error/timeout. */
    juce::String measureViaChild (PluginHostChildCoordinator& coord,
                                  const juce::String& pluginPath,
                                  const juce::String& excitation,
                                  const juce::File& exportPath,
                                  const juce::File& wavPath,
                                  const juce::String& measureType = "frequency_response")
    {
        // start handshake
        if (! coord.sendLine (R"({"cmd":"start"})"))
            return {};
        if (popUntil (coord, "\"pid\":", 5000).isEmpty())
            return {};

        // load
        {
            juce::DynamicObject req;
            req.setProperty ("cmd", "load");
            req.setProperty ("path", pluginPath);
            // JSON::toString(v) defaults to MULTILINE output — the child's
            // getline would receive a truncated fragment. allOnOneLine=true.
            const auto reqVar = juce::var (new juce::DynamicObject (req));
            if (! coord.sendLine (juce::JSON::toString (reqVar, true)))
                return {};
            const auto loadLine = popUntil (coord, "\"name\"", 40000);
            if (loadLine.isEmpty() || ! loadLine.contains ("\"ok\":true"))
                return {};
        }

        // measure
        {
            juce::DynamicObject req;
            req.setProperty ("cmd", "measure");
            req.setProperty ("type", measureType);
            req.setProperty ("excitation", excitation);
            req.setProperty ("sample_rate", 48000);
            req.setProperty ("block_size", 512);
            req.setProperty ("export_path", exportPath.getFullPathName());
            req.setProperty ("wav_path", wavPath.getFullPathName());
            const auto reqVar = juce::var (new juce::DynamicObject (req));
            if (! coord.sendLine (juce::JSON::toString (reqVar, true)))
                return {};
            return popUntil (coord, "\"samples\"", 60000);
        }
    }

    //==============================================================================
    // In-process reference path (the "host direct measurement"): load the real
    // VST3 in this test process and run the same frozen SweepRunner pipeline.

    std::unique_ptr<juce::AudioPluginInstance> loadPluginInProcess (
        const juce::File& path, double sampleRate, int blockSize, juce::String& pluginName)
    {
        juce::AudioPluginFormatManager formatManager;
        formatManager.addFormat (std::make_unique<juce::VST3PluginFormat>());
        auto* format = formatManager.getFormat (0);
        if (format == nullptr)
            return {};

        // Same description-from-file route as PluginHostChild.cpp handleLoad.
        juce::OwnedArray<juce::PluginDescription> found;
        format->findAllTypesForFile (found, path.getFullPathName());
        if (found.isEmpty())
            return {};

        // createPluginInstanceAsync posts to the message thread — pump the
        // dispatch loop while waiting (mirror of handleLoad; /EHa on this TU
        // turns the catch(...) into SEH interception too).
        struct LoadState
        {
            std::unique_ptr<juce::AudioPluginInstance> instance;
            juce::String error;
            juce::WaitableEvent done;
            std::atomic<bool> alive { true };
        };
        auto state = std::make_shared<LoadState>();
        auto onCreated = [state] (std::unique_ptr<juce::AudioPluginInstance> instance,
                                  const juce::String& errorMessage) mutable
        {
            if (! state->alive.load())
                return;
            state->instance = std::move (instance);
            state->error = errorMessage;
            state->done.signal();
        };

        try
        {
            format->createPluginInstanceAsync (*found.getFirst(), sampleRate, blockSize,
                                               std::move (onCreated));
            const auto deadline = juce::Time::getMillisecondCounter() + 30000u;
            while (! state->done.wait (20))
            {
                if (juce::Time::getMillisecondCounter() >= deadline)
                {
                    state->alive = false;
                    return {};
                }
                juce::MessageManager::getInstance()->runDispatchLoopUntil (20);
            }
        }
        catch (...)
        {
            return {};
        }

        if (! state->instance)
            return {};
        pluginName = found.getFirst()->name;
        return std::move (state->instance);
    }

    /** Run the same generator configuration the child uses (sweep: 20 Hz –
     *  20 kHz, 5 s, 0.5 amp; MLS: 16383-sample, 0.5 amp) through the frozen
     *  SweepRunner; returns copies of the recorded dry/wet. */
    void runInProcessSweep (juce::AudioPluginInstance* plugin, bool useMLS,
                            juce::AudioBuffer<float>& dry, juce::AudioBuffer<float>& wet)
    {
        std::unique_ptr<SignalGenerator> gen;
        if (useMLS)
        {
            auto mls = std::make_unique<Impulse>();
            mls->useMLS (true);
            mls->setMLSLength (16383);
            mls->setAmplitude (0.5);
            gen = std::move (mls);
        }
        else
        {
            auto sweep = std::make_unique<SineSweep>();
            sweep->setFrequencyRange (20.0, 20000.0);
            sweep->setDuration (5.0);
            sweep->setAmplitude (0.5);
            gen = std::move (sweep);
        }

        SweepRunner runner;
        runner.prepare (48000.0, 512);
        runner.setPlugin (plugin);
        runner.setGenerator (gen.get());
        REQUIRE (runner.run());

        dry = runner.getResult().getDryBuffer();
        wet = runner.getResult().getWetBuffer();
    }

    /** Build the EXACT dynamic generator the child uses for gr_timeline
     *  (PluginHostChild.cpp handleMeasure gr_timeline branch): SineSweep
     *  [10000,20000] Hz, 2 s, amp 0.5, wrapped in EnvelopeSignal ADSR
     *  {0.02,0.1,0.8,0.2} s, speed 1.0. Keep in lockstep with that branch
     *  (and the CommandParser dynamic-source defaults) so both sides drive
     *  the plugin with bit-identical excitation. */
    std::unique_ptr<SignalGenerator> makeGrTimelineGenerator()
    {
        auto sweep = std::make_unique<SineSweep>();
        sweep->setFrequencyRange (10000.0, 20000.0);
        sweep->setDuration (2.0);
        sweep->setAmplitude (0.5);

        auto env = std::make_unique<EnvelopeSignal> (std::move (sweep));
        env->setEnvelope (EnvelopeSignal::Envelope::adsr);
        env->setADSR (0.02, 0.1, 0.8, 0.2);
        env->setSpeed (1.0);
        return env;
    }

    /** In-process gr_timeline reference: the same dynamic generator as the
     *  child through the same frozen SweepRunner (48 kHz / 512, no tail pad —
     *  identical to the child's handleMeasure configuration). */
    void runInProcessGrTimeline (juce::AudioPluginInstance* plugin,
                                 juce::AudioBuffer<float>& dry,
                                 juce::AudioBuffer<float>& wet)
    {
        auto gen = makeGrTimelineGenerator();

        SweepRunner runner;
        runner.prepare (48000.0, 512);
        runner.setPlugin (plugin);
        runner.setGenerator (gen.get());
        REQUIRE (runner.run());

        dry = runner.getResult().getDryBuffer();
        wet = runner.getResult().getWetBuffer();
    }

    //==============================================================================
    // Comparison helpers.

    juce::String jsonField (const juce::String& line, const juce::String& key)
    {
        const auto doc = juce::JSON::parse (line);
        if (! doc.isObject())
            return {};
        return doc[juce::Identifier (key)].toString();
    }

    int jsonIntField (const juce::String& line, const juce::String& key)
    {
        const auto doc = juce::JSON::parse (line);
        if (! doc.isObject())
            return -1;
        return static_cast<int> (doc[juce::Identifier (key)]);
    }

    struct RawPoint
    {
        double freq = 0.0;
        double magDB = 0.0;
    };

    std::vector<RawPoint> parseRaw (const juce::String& json)
    {
        std::vector<RawPoint> points;
        const auto doc = juce::JSON::parse (json);
        if (! doc.isObject())
            return points;
        const auto raw = doc["raw"];
        if (! raw.isArray())
            return points;
        points.reserve (static_cast<size_t> (raw.size()));
        for (int i = 0; i < raw.size(); ++i)
            points.push_back ({ static_cast<double> (raw[i]["f"]),
                                static_cast<double> (raw[i]["mag"]) });
        return points;
    }

    /** Max |Δmag| over points with 100 ≤ f ≤ 10000 Hz (both curves share the
     *  same FFT bin grid). Returns -1 when the point counts differ. */
    double maxMagDiffInBand (const std::vector<RawPoint>& a,
                             const std::vector<RawPoint>& b,
                             int& inBandCount)
    {
        inBandCount = 0;
        if (a.size() != b.size())
            return -1.0;
        double maxDiff = 0.0;
        for (size_t i = 0; i < a.size(); ++i)
        {
            if (a[i].freq < 100.0 || a[i].freq > 10000.0)
                continue;
            ++inBandCount;
            maxDiff = std::max (maxDiff, std::abs (a[i].magDB - b[i].magDB));
        }
        return maxDiff;
    }

    //==============================================================================
    // gr_timeline JSON parsing (Export::grTimelineToJSON schema): "gr" body
    // (sample_rate, num_points, timeline[{t, gr_db}]) + "tau" body
    // (attack_sec, release_sec, valid).

    struct GrTimelinePoint
    {
        double t = 0.0;
        double grDB = 0.0;
    };

    struct GrJson
    {
        juce::String type;
        int numPoints = -1;
        std::vector<GrTimelinePoint> timeline;
    };

    struct TauJson
    {
        double attackSec = 0.0;
        double releaseSec = 0.0;
        bool valid = false;
    };

    GrJson parseGrTimelineJson (const juce::String& line)
    {
        GrJson out;
        const auto doc = juce::JSON::parse (line);
        if (! doc.isObject())
            return out;
        out.type = doc["type"].toString();
        const auto gr = doc["gr"];
        if (! gr.isObject())
            return out;
        out.numPoints = static_cast<int> (gr["num_points"]);
        const auto tl = gr["timeline"];
        if (! tl.isArray())
            return out;
        out.timeline.reserve (static_cast<size_t> (tl.size()));
        for (int i = 0; i < tl.size(); ++i)
            out.timeline.push_back ({ static_cast<double> (tl[i]["t"]),
                                      static_cast<double> (tl[i]["gr_db"]) });
        return out;
    }

    TauJson parseTauJson (const juce::String& line)
    {
        TauJson out;
        const auto doc = juce::JSON::parse (line);
        if (! doc.isObject())
            return out;
        const auto tau = doc["tau"];
        if (! tau.isObject())
            return out;
        out.attackSec  = static_cast<double> (tau["attack_sec"]);
        out.releaseSec = static_cast<double> (tau["release_sec"]);
        out.valid      = static_cast<bool> (tau["valid"]);
        return out;
    }

    /** Max |Δgr_db| over the DRIVEN region (either curve |gr_db| >
     *  drivenThresholdDB). Both curves share the same non-overlapping RMS
     *  window grid, so the comparison is index-by-index. Windows whose dry
     *  RMS is below the silence threshold (envelope attack ramp, tail pads)
     *  are forced to 0 dB by GainReduction — the |gr_db| filter excludes
     *  them naturally. */
    double maxGrDiffDriven (const std::vector<GrTimelinePoint>& a,
                            const std::vector<GrTimelinePoint>& b,
                            int& drivenCount,
                            double drivenThresholdDB)
    {
        drivenCount = 0;
        const size_t n = std::min (a.size(), b.size());
        double maxDiff = 0.0;
        for (size_t i = 0; i < n; ++i)
        {
            if (std::abs (a[i].grDB) <= drivenThresholdDB
                && std::abs (b[i].grDB) <= drivenThresholdDB)
                continue;
            ++drivenCount;
            maxDiff = std::max (maxDiff, std::abs (a[i].grDB - b[i].grDB));
        }
        return maxDiff;
    }
}  // namespace

//==============================================================================
// P1 — sweep excitation: child-process measurement vs in-process reference.
//==============================================================================

TEST_CASE ("ChildHostParity: child-process sweep matches in-process measurement (<0.5 dB)",
           "[childparity][parity]")
{
    // Preconditions — real plugin + real child exe (no fake-plugin fallback).
    // A .vst3 is a DIRECTORY, so exists() (not existsAsFile()) is the check.
    const auto pluginFile = realPluginFile();
    if (! pluginFile.exists())
        SKIP ("CHILD_PARITY_PLUGIN not present: " + pluginFile.getFullPathName());
    if (! realChildExe().existsAsFile())
        SKIP ("PLUGIN_HOST_CHILD_EXE not present: " + realChildExe().getFullPathName());

    constexpr double kSr = 48000.0;
    constexpr int kBlockSize = 512;
    constexpr double kMaxDbDiff = 0.5;

    ensureMessageManager();

    // ── Child path: spawn + start + load + measure(sweep) ──
    PluginHostChildCoordinator coord (realChildExe().getFullPathName());
    std::atomic<bool> crashed { false };
    coord.setOnCrash ([&] (const juce::String&) { crashed.store (true); });
    REQUIRE (coord.start());

    const auto exportPath = tempFile ("pluginlab_parity_", ".json");
    const auto wavPath = tempFile ("pluginlab_parity_", ".wav");

    const auto resultLine = measureViaChild (coord, pluginFile.getFullPathName(),
                                             "sweep", exportPath, wavPath);
    REQUIRE_FALSE (crashed.load());
    REQUIRE (resultLine.isNotEmpty());          // {"ok":true,...,"samples":...}
    REQUIRE (resultLine.contains ("\"ok\":true"));
    REQUIRE (wavPath.existsAsFile());
    REQUIRE (wavPath.getSize() > 44);

    const auto childName = jsonField (resultLine, "name");
    const auto childClassId = jsonField (resultLine, "class_id");
    const int childLatency = jsonIntField (resultLine, "latency_samples");
    INFO ("child plugin: name=" << childName << " class_id=" << childClassId
          << " latency_samples=" << childLatency
          << " wav=" << wavPath.getFullPathName() << " (" << wavPath.getSize() << " bytes)");
    REQUIRE (childName.isNotEmpty());

    // ── Host path: in-process load + identical sweep + same analysis entry ──
    juce::String hostName;
    auto plugin = loadPluginInProcess (pluginFile, kSr, kBlockSize, hostName);
    REQUIRE (plugin != nullptr);
    REQUIRE (hostName.isNotEmpty());
    INFO ("host plugin: name=" << hostName);

    const int numChannels = plugin->getTotalNumInputChannels();
    REQUIRE (numChannels > 0);

    juce::AudioBuffer<float> hostDry, hostWet;
    runInProcessSweep (plugin.get(), false, hostDry, hostWet);
    REQUIRE (hostDry.getNumSamples() > 0);

    const auto hostWav = tempFile ("pluginlab_parity_host_", ".wav");
    REQUIRE (writeTestWav (hostWav, hostDry, hostWet, kSr));

    const auto transitJson = ChildWavAnalyzer::analyzeChildFrequencyResponse (
        wavPath, numChannels, kSr, kBlockSize, "sweep", 0,
        childName, childClassId, childLatency);
    const auto directJson = ChildWavAnalyzer::analyzeChildFrequencyResponse (
        hostWav, numChannels, kSr, kBlockSize, "sweep", 0,
        hostName, childClassId, plugin->getLatencySamples());
    REQUIRE (transitJson.isNotEmpty());
    REQUIRE (directJson.isNotEmpty());

    // ── Compare raw magnitude curves in-band ──
    int inBandCount = 0;
    const double maxDiff = maxMagDiffInBand (parseRaw (transitJson), parseRaw (directJson),
                                             inBandCount);
    REQUIRE (maxDiff >= 0.0);                   // both curves share the bin grid
    REQUIRE (inBandCount > 100);
    WARN ("in-band points=" << inBandCount << " max |ΔdB|=" << maxDiff);   // acceptance evidence
    REQUIRE (maxDiff < kMaxDbDiff);

    coord.stop();
    exportPath.deleteFile();
    wavPath.deleteFile();
    hostWav.deleteFile();
}

//==============================================================================
// P2 — MLS excitation: same comparison through the MLS branch.
//==============================================================================

TEST_CASE ("ChildHostParity: child-process MLS matches in-process measurement (<0.5 dB)",
           "[childparity][parity][mls]")
{
    // A .vst3 is a DIRECTORY, so exists() (not existsAsFile()) is the check.
    const auto pluginFile = realPluginFile();
    if (! pluginFile.exists())
        SKIP ("CHILD_PARITY_PLUGIN not present: " + pluginFile.getFullPathName());
    if (! realChildExe().existsAsFile())
        SKIP ("PLUGIN_HOST_CHILD_EXE not present: " + realChildExe().getFullPathName());

    constexpr double kSr = 48000.0;
    constexpr int kBlockSize = 512;
    constexpr int kMlsLength = 16383;
    constexpr double kMaxDbDiff = 0.5;

    ensureMessageManager();

    // ── Child path: measure(mls) ──
    PluginHostChildCoordinator coord (realChildExe().getFullPathName());
    std::atomic<bool> crashed { false };
    coord.setOnCrash ([&] (const juce::String&) { crashed.store (true); });
    REQUIRE (coord.start());

    const auto exportPath = tempFile ("pluginlab_parity_mls_", ".json");
    const auto wavPath = tempFile ("pluginlab_parity_mls_", ".wav");

    const auto resultLine = measureViaChild (coord, pluginFile.getFullPathName(),
                                             "mls", exportPath, wavPath);
    REQUIRE_FALSE (crashed.load());
    REQUIRE (resultLine.isNotEmpty());
    REQUIRE (resultLine.contains ("\"ok\":true"));
    REQUIRE (wavPath.existsAsFile());
    REQUIRE (wavPath.getSize() > 44);

    const auto childName = jsonField (resultLine, "name");
    const auto childClassId = jsonField (resultLine, "class_id");
    const int childLatency = jsonIntField (resultLine, "latency_samples");
    INFO ("child plugin: name=" << childName << " class_id=" << childClassId
          << " latency_samples=" << childLatency
          << " wav=" << wavPath.getFullPathName() << " (" << wavPath.getSize() << " bytes)");
    REQUIRE (childName.isNotEmpty());

    // ── Host path ──
    juce::String hostName;
    auto plugin = loadPluginInProcess (pluginFile, kSr, kBlockSize, hostName);
    REQUIRE (plugin != nullptr);
    INFO ("host plugin: name=" << hostName);

    const int numChannels = plugin->getTotalNumInputChannels();
    REQUIRE (numChannels > 0);

    juce::AudioBuffer<float> hostDry, hostWet;
    runInProcessSweep (plugin.get(), true, hostDry, hostWet);
    REQUIRE (hostDry.getNumSamples() > 0);

    const auto hostWav = tempFile ("pluginlab_parity_host_mls_", ".wav");
    REQUIRE (writeTestWav (hostWav, hostDry, hostWet, kSr));

    const auto transitJson = ChildWavAnalyzer::analyzeChildFrequencyResponse (
        wavPath, numChannels, kSr, kBlockSize, "mls", kMlsLength,
        childName, childClassId, childLatency);
    const auto directJson = ChildWavAnalyzer::analyzeChildFrequencyResponse (
        hostWav, numChannels, kSr, kBlockSize, "mls", kMlsLength,
        hostName, childClassId, plugin->getLatencySamples());
    REQUIRE (transitJson.isNotEmpty());
    REQUIRE (directJson.isNotEmpty());

    int inBandCount = 0;
    const double maxDiff = maxMagDiffInBand (parseRaw (transitJson), parseRaw (directJson),
                                             inBandCount);
    REQUIRE (maxDiff >= 0.0);
    REQUIRE (inBandCount > 100);
    WARN ("in-band points=" << inBandCount << " max |ΔdB|=" << maxDiff);   // acceptance evidence
    REQUIRE (maxDiff < kMaxDbDiff);

    coord.stop();
    exportPath.deleteFile();
    wavPath.deleteFile();
    hostWav.deleteFile();
}

//==============================================================================
// T6 — gr_timeline (issue #15): child-process dynamic measurement vs the
// in-process dynamic reference. Both sides run the SAME enveloped sweep
// through the same frozen SweepRunner, transit their dry/wet through a
// 24-bit WAV, and the SAME ChildWavAnalyzer::analyzeChildGrTimeline turns
// each WAV into export JSON. The two GR timelines are compared point-by-point
// over the driven region (|gr_db| > 0.5), and the tau estimates must agree
// within 0.7x-1.4x.
//==============================================================================

TEST_CASE ("ChildHostParity: child-process gr_timeline matches in-process measurement",
           "[childparity][gr]")
{
    // Preconditions — real plugin + real child exe (no fake-plugin fallback).
    // A .vst3 is a DIRECTORY, so exists() (not existsAsFile()) is the check.
    const auto pluginFile = realPluginFile();
    if (! pluginFile.exists())
        SKIP ("CHILD_PARITY_PLUGIN not present: " + pluginFile.getFullPathName());
    if (! realChildExe().existsAsFile())
        SKIP ("PLUGIN_HOST_CHILD_EXE not present: " + realChildExe().getFullPathName());

    constexpr double kSr = 48000.0;
    constexpr int kBlockSize = 512;
    // Driven region: points where compression is actually active (|gr_db| >
    // this). Windows on the silent envelope tail are forced to 0 dB by
    // GainReduction and fall out of the filter naturally.
    constexpr double kDrivenThresholdDB = 0.5;
    constexpr double kMaxGrDiffDB = 0.5;   // acceptance: max |Δgr_db| < 0.5 dB
    constexpr double kMinTauRatio = 0.7;   // acceptance: larger/smaller tau ∈ [0.7, 1.4]
    constexpr double kMaxTauRatio = 1.4;

    ensureMessageManager();

    // ── Child path: spawn + start + load + measure(gr_timeline) ──
    PluginHostChildCoordinator coord (realChildExe().getFullPathName());
    std::atomic<bool> crashed { false };
    coord.setOnCrash ([&] (const juce::String&) { crashed.store (true); });
    REQUIRE (coord.start());

    const auto exportPath = tempFile ("pluginlab_parity_gr_", ".json");
    const auto wavPath = tempFile ("pluginlab_parity_gr_", ".wav");

    // excitation "sweep": the child's gr_timeline branch generates its own
    // enveloped sweep and only validates the excitation field (sweep|mls).
    const auto resultLine = measureViaChild (coord, pluginFile.getFullPathName(),
                                             "sweep", exportPath, wavPath, "gr_timeline");
    REQUIRE_FALSE (crashed.load());
    REQUIRE (resultLine.isNotEmpty());          // {"ok":true,...,"samples":...}
    REQUIRE (resultLine.contains ("\"ok\":true"));
    REQUIRE (wavPath.existsAsFile());
    REQUIRE (wavPath.getSize() > 44);

    const auto childName = jsonField (resultLine, "name");
    const auto childClassId = jsonField (resultLine, "class_id");
    const int childLatency = jsonIntField (resultLine, "latency_samples");
    INFO ("child plugin: name=" << childName << " class_id=" << childClassId
          << " latency_samples=" << childLatency
          << " wav=" << wavPath.getFullPathName() << " (" << wavPath.getSize() << " bytes)");
    REQUIRE (childName.isNotEmpty());

    // ── Host path: in-process load + identical dynamic generator + same
    //    analysis entry on a transit WAV written from the recorded buffers ──
    juce::String hostName;
    auto plugin = loadPluginInProcess (pluginFile, kSr, kBlockSize, hostName);
    REQUIRE (plugin != nullptr);
    REQUIRE (hostName.isNotEmpty());
    INFO ("host plugin: name=" << hostName);

    const int numChannels = plugin->getTotalNumInputChannels();
    REQUIRE (numChannels > 0);

    juce::AudioBuffer<float> hostDry, hostWet;
    runInProcessGrTimeline (plugin.get(), hostDry, hostWet);
    REQUIRE (hostDry.getNumSamples() > 0);

    const auto hostWav = tempFile ("pluginlab_parity_host_gr_", ".wav");
    REQUIRE (writeTestWav (hostWav, hostDry, hostWet, kSr));

    // Same analysis entry, same WAV layout, same metadata route — the only
    // remaining difference is the transit path (child process vs in-process).
    const auto transitJson = ChildWavAnalyzer::analyzeChildGrTimeline (
        wavPath, numChannels, kSr, kBlockSize,
        childName, childClassId, childLatency);
    const auto directJson = ChildWavAnalyzer::analyzeChildGrTimeline (
        hostWav, numChannels, kSr, kBlockSize,
        hostName, childClassId, plugin->getLatencySamples());
    REQUIRE (transitJson.isNotEmpty());
    REQUIRE (directJson.isNotEmpty());

    // ── Compare GR timelines + tau estimates ──
    const auto transit = parseGrTimelineJson (transitJson);
    const auto direct = parseGrTimelineJson (directJson);
    const auto transitTau = parseTauJson (transitJson);
    const auto directTau = parseTauJson (directJson);

    REQUIRE (transit.type == "gr_timeline");
    REQUIRE (direct.type == "gr_timeline");
    REQUIRE_FALSE (transit.timeline.empty());
    REQUIRE_FALSE (direct.timeline.empty());
    REQUIRE (transit.numPoints == direct.numPoints);   // identical window grid

    int drivenCount = 0;
    const double maxGrDiff = maxGrDiffDriven (transit.timeline, direct.timeline,
                                              drivenCount, kDrivenThresholdDB);
    INFO ("tau transit: attack=" << transitTau.attackSec
          << " release=" << transitTau.releaseSec
          << " valid=" << (transitTau.valid ? "true" : "false")
          << " | tau direct: attack=" << directTau.attackSec
          << " release=" << directTau.releaseSec
          << " valid=" << (directTau.valid ? "true" : "false"));
    WARN ("num_points=" << transit.numPoints << " driven points=" << drivenCount
          << " max |Δgr_db|=" << maxGrDiff);   // acceptance evidence
    REQUIRE (drivenCount > 0);
    REQUIRE (maxGrDiff < kMaxGrDiffDB);

    REQUIRE (transitTau.valid);
    REQUIRE (directTau.valid);
    REQUIRE (transitTau.attackSec > 0.0);
    REQUIRE (transitTau.releaseSec > 0.0);
    REQUIRE (directTau.attackSec > 0.0);
    REQUIRE (directTau.releaseSec > 0.0);

    // Log-domain tau fits are sensitive to measurement noise. The ratio of
    // the larger to the smaller estimate covers both orderings; both
    // directions staying inside [0.7, 1.4] means either is within 0.7x-1.4x
    // of the other. (Calibration note: if the GR curves still match < 0.5 dB
    // and both taus are valid but these bounds are too tight for this plugin,
    // widen to 0.5-2.0 with a comment — never weaken the GR-curve gate.)
    const double attackRatio = std::max (transitTau.attackSec, directTau.attackSec)
                             / std::min (transitTau.attackSec, directTau.attackSec);
    const double releaseRatio = std::max (transitTau.releaseSec, directTau.releaseSec)
                              / std::min (transitTau.releaseSec, directTau.releaseSec);
    WARN ("tau attack ratio=" << attackRatio << " release ratio=" << releaseRatio);
    REQUIRE (attackRatio >= kMinTauRatio);
    REQUIRE (attackRatio <= kMaxTauRatio);
    REQUIRE (releaseRatio >= kMinTauRatio);
    REQUIRE (releaseRatio <= kMaxTauRatio);

    coord.stop();
    exportPath.deleteFile();
    wavPath.deleteFile();
    hostWav.deleteFile();
}


//==============================================================================
// T7 (issue #116) — host/child loadPlugin contract parity.
//
// The child's load command (source/child/PluginHostChild.cpp handleLoad) is a
// deliberate mirror of PluginManager::loadPlugin, aligned by cross-referencing
// comments only ("Mirror PluginManager::loadPlugin (PluginManager::loadPlugin)"),
// so either side can drift silently. These cases pin the parts that must stay
// identical:
//
//   * the 30 s creation budget,
//   * the late-callback guard (alive=false → the callback writes nothing),
//   * the child's ~500 ms progress heartbeat inside the host's 3 s watchdog,
//   * the "plugin not found" / "load timeout" error vocabulary,
//   * the 48 kHz / 512 defaults.
//
// What is deliberately NOT shared, and therefore asserted in its own shape
// instead of as "equal": the threading model (the child's main thread IS its
// message thread, so it pumps runDispatchLoopUntil; the host waits on a
// dedicated thread) and the failure SEMANTICS (the host answers nullptr +
// preventive blacklist + CRASH_LOG; the child answers
// {"ok":false,"error":"load timeout"} and never blacklists).
//
// The child's constants live in an anonymous namespace inside a separate
// executable, so no link-time symbol ties them to PluginManager's — the
// contract is read from the source text. That is the only way a unit test can
// compare the two sides without changing production code (the alternative,
// extracting a shared helper, was rejected for the threading/semantics split
// above — see the issue).

namespace
{
    //==============================================================================
    juce::File sourceRoot()
    {
        return juce::File (juce::String (PLUGINLAB_SOURCE_DIR));
    }

    /** Whitespace-stripped source text: value drift (30000 → 20000) still
        fails, pure re-alignment/reformatting does not. */
    juce::String squashedSource (const juce::String& relativePath)
    {
        auto text = sourceRoot().getChildFile (relativePath).loadFileAsString();
        return text.removeCharacters (" \t\r\n");
    }

    /** Unmodified source text — for assertions on string LITERALS (squashing
        would also eat the spaces inside "load timeout"). */
    juce::String sourceText (const juce::String& relativePath)
    {
        return sourceRoot().getChildFile (relativePath).loadFileAsString();
    }

    /** Throwaway cache directory: PluginManager::loadPlugin persists the
        blacklist on timeout, and the test must never touch the real
        %APPDATA%/PluginLab cache. */
    struct TempCacheDir
    {
        juce::File dir;

        TempCacheDir()
            : dir (juce::File::getSpecialLocation (juce::File::SpecialLocationType::tempDirectory)
                       .getNonexistentChildFile ("pluginlab_parity_contract_", "_dir"))
        {
            dir.createDirectory();
        }

        ~TempCacheDir() { dir.deleteRecursively(); }

        juce::File cacheFile() const { return dir.getChildFile ("pluginlist.xml"); }
    };

    juce::PluginDescription fakeDescription (const juce::String& name,
                                             const juce::String& fileOrIdentifier,
                                             int uid)
    {
        juce::PluginDescription d;
        d.name = name;
        d.descriptiveName = name;
        d.fileOrIdentifier = fileOrIdentifier;
        d.pluginFormatName = "VST3";
        d.uniqueId = uid;
        return d;
    }
}  // namespace

//==============================================================================
// T7a — creation budget + late-callback guard.
//==============================================================================

TEST_CASE ("ChildHostParity: load timeout budget is 30 s on both paths and late callbacks are discarded",
           "[childparity][contract][load]")
{
    // ---- Contract: same creation budget on both sides ----
    REQUIRE (PluginManager::kLoadTimeoutMs == 30000);
    REQUIRE (squashedSource ("host/PluginManager.h").contains ("kLoadTimeoutMs=30000"));
    REQUIRE (squashedSource ("child/PluginHostChild.cpp").contains ("kLoadTimeoutMs=30000"));

    // ---- Contract: the late-callback guard is the same in both TUs ----
    // alive=false on the timeout path plus the callback's early return: the
    // creation callback can still arrive after the timeout and must write
    // neither the instance nor the error into the state.
    const auto hostText = squashedSource ("host/PluginManager.cpp");
    const auto childText = squashedSource ("child/PluginHostChild.cpp");
    REQUIRE (hostText.contains ("state->alive=false"));
    REQUIRE (hostText.contains ("if(!state->alive.load())return;"));
    REQUIRE (childText.contains ("state->alive=false"));
    REQUIRE (childText.contains ("if(!state->alive.load())return;"));

    // ---- Contract: only the child answers a timeout with a protocol error;
    //      the host reports nullptr + CRASH_LOG + blacklist (see the
    //      behavioural half below) ----
    REQUIRE (sourceText ("child/PluginHostChild.cpp").contains (R"("error":"load timeout")"));

    // ---- Behaviour (host side): timeout → nullptr + blacklist, and the
    //      stale callback is dropped without touching the state ----
    TempCacheDir tmp;
    PluginManager mgr;
    mgr.setCacheFile (tmp.cacheFile());   // saveCache on timeout must not touch the real cache
    mgr.setLoadTimeoutMs (50);

    PluginManager::PluginCreationCallback stale;
    mgr.setAsyncCreateOverride ([&] (const juce::PluginDescription&, double, int,
                                     PluginManager::PluginCreationCallback cb)
    {
        if (! stale)                      // keep only the first (timed-out) callback
            stale = std::move (cb);
    });

    auto instance = mgr.loadPlugin (fakeDescription ("Fake", "C:\\fake\\fake.vst3", 9101),
                                    48000.0, 512);
    REQUIRE (instance == nullptr);
    REQUIRE (mgr.getKnownPlugins().getBlacklistedFiles().contains ("C:\\fake\\fake.vst3"));

    // The late callback arrives after loadPlugin already returned (timed out):
    // the alive=false guard drops it — no crash, nothing written.
    REQUIRE (stale != nullptr);
    REQUIRE_NOTHROW (stale (std::make_unique<TestPlugin>(), juce::String()));
}

//==============================================================================
// T7b — progress heartbeat interval vs the host watchdog budget.
//==============================================================================

TEST_CASE ("ChildHostParity: child progress heartbeat stays inside the host watchdog budget",
           "[childparity][contract][heartbeat]")
{
    // The host's watchdog treats silence as death (default 3 s) and the child
    // keeps itself alive with progress lines while a blocking load/measure
    // runs. Both values are pinned: the child's interval is TU-local and the
    // watchdog's budget is a constructor default argument, so neither is
    // reachable as a link-time symbol from the test.
    constexpr int kExpectedProgressIntervalMs = 500;
    constexpr int kCoordinatorHeartbeatMs = 3000;

    REQUIRE (squashedSource ("child/PluginHostChild.cpp")
                 .contains (juce::String ("lastProgressMs>=") + juce::String (kExpectedProgressIntervalMs)));
    REQUIRE (squashedSource ("host/ChildProcessCoordinator.h")
                 .contains (juce::String ("heartbeatTimeoutMs=") + juce::String (kCoordinatorHeartbeatMs)));

    // Two consecutive missed progress lines still fit inside the budget — the
    // two constants above are pinned to the real values, so this is a
    // documented relationship, not an assertion about the code.
}

//==============================================================================
// T7c — "plugin not found" wording, both paths, same literal.
//==============================================================================

TEST_CASE ("ChildHostParity: 'plugin not found' wording is identical on both paths",
           "[childparity][contract][errors]")
{
    const juce::String kNotFound ("plugin not found");

    // ---- Host path: CommandParser resolves against knownPlugins (empty here)
    //      and then against the blacklist (empty too) ----
    ensureMessageManager();
    PluginManager mgr;
    CommandParser parser;
    parser.setPluginManager (&mgr);

    const auto hostResponse = parser.handleCommand (
        R"({"cmd":"loadPlugin","path":"C:\\fake\\NoSuchPlugin.vst3"})");
    REQUIRE (hostResponse.contains ("\"ok\":false"));
    REQUIRE (hostResponse.contains (kNotFound));

    // ---- Child path: the real child scans the file and finds no types ----
    // Same SKIP policy as the other real-process cases in this file: a missing
    // child exe must not red the whole binary.
    if (! realChildExe().existsAsFile())
        SKIP ("PLUGIN_HOST_CHILD_EXE not present: " + realChildExe().getFullPathName());
    PluginHostChildCoordinator coord (realChildExe().getFullPathName());
    std::atomic<bool> crashed { false };
    coord.setOnCrash ([&] (const juce::String&) { crashed.store (true); });
    REQUIRE (coord.start());

    REQUIRE (coord.sendLine (R"({"cmd":"load","path":"C:\\fake\\NoSuchPlugin.vst3"})"));
    const auto childResponse = popUntil (coord, "\"error\"", 10000);
    coord.stop();

    REQUIRE_FALSE (crashed.load());
    REQUIRE (childResponse.contains ("\"ok\":false"));
    REQUIRE (childResponse.contains (kNotFound));
}

//==============================================================================
// T7d — 48 kHz / 512 defaults on both paths.
//==============================================================================

TEST_CASE ("ChildHostParity: sampleRate/blockSize defaults are 48000/512 on both paths",
           "[childparity][contract][defaults]")
{
    // Host-side default: the child measure contract is what the host forwards
    // when the caller omits fields. (MeasurementSession's own defaults are the
    // in-process measurement path — out of scope for a loadPlugin parity test.)
    const ChildMeasureContract::ChildMeasureRequest request;
    REQUIRE (request.sampleRate == 48000.0);
    REQUIRE (request.blockSize == 512);

    // Child-side defaults: applied when a load/measure request omits the
    // fields (DynamicObject::getProperty has no default overload in JUCE 9).
    const auto childText = squashedSource ("child/PluginHostChild.cpp");
    REQUIRE (childText.contains ("kDefaultSampleRate=48000.0"));
    REQUIRE (childText.contains ("kDefaultBlockSize=512"));
}

//==============================================================================
// T7e — the child really applies those defaults when the request omits them
// (real plugin; SKIPs when absent, like the other parity cases).
//==============================================================================

TEST_CASE ("ChildHostParity: child load applies the 48k/512 defaults when the request omits them",
           "[childparity][contract][defaults]")
{
    // A .vst3 is a DIRECTORY, so exists() (not existsAsFile()) is the check.
    const auto pluginFile = realPluginFile();
    if (! pluginFile.exists())
        SKIP ("CHILD_PARITY_PLUGIN not present: " + pluginFile.getFullPathName());
    if (! realChildExe().existsAsFile())
        SKIP ("PLUGIN_HOST_CHILD_EXE not present: " + realChildExe().getFullPathName());

    ensureMessageManager();

    PluginHostChildCoordinator coord (realChildExe().getFullPathName());
    std::atomic<bool> crashed { false };
    coord.setOnCrash ([&] (const juce::String&) { crashed.store (true); });
    REQUIRE (coord.start());

    REQUIRE (coord.sendLine (R"({"cmd":"start"})"));
    REQUIRE (popUntil (coord, "\"pid\":", 5000).isNotEmpty());

    // Only "path" — no sample_rate / block_size: the child must fall back to
    // 48000/512 and still create the instance.
    juce::DynamicObject req;
    req.setProperty ("cmd", "load");
    req.setProperty ("path", pluginFile.getFullPathName());
    const auto reqVar = juce::var (new juce::DynamicObject (req));
    REQUIRE (coord.sendLine (juce::JSON::toString (reqVar, true)));

    const auto loadLine = popUntil (coord, "\"name\"", 40000);
    coord.stop();

    REQUIRE_FALSE (crashed.load());
    REQUIRE (loadLine.contains ("\"ok\":true"));
    REQUIRE (jsonField (loadLine, "name").isNotEmpty());
}
