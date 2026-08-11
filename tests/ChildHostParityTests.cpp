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
 * If the real plugin is absent from the machine (CHILD_PARITY_PLUGIN does
 * not exist), the tests SKIP with the reason logged — they never substitute
 * a fake plugin for the real one.
 */
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "../source/host/ChildProcessCoordinator.h"
#include "../source/analysis/ChildWavAnalyzer.h"
#include "../source/capture/SweepRunner.h"
#include "../source/signal/SineSweep.h"
#include "../source/signal/Impulse.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>
#include <vector>

#ifndef PLUGIN_HOST_CHILD_EXE
#error "PLUGIN_HOST_CHILD_EXE must be defined by tests/CMakeLists.txt"
#endif

#ifndef CHILD_PARITY_PLUGIN
#error "CHILD_PARITY_PLUGIN must be defined by tests/CMakeLists.txt"
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

    /** start → load → measure; returns the measure result line (contains
     *  "samples") or empty on error/timeout. */
    juce::String measureViaChild (PluginHostChildCoordinator& coord,
                                  const juce::String& pluginPath,
                                  const juce::String& excitation,
                                  const juce::File& exportPath,
                                  const juce::File& wavPath)
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
            req.setProperty ("type", "frequency_response");
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
