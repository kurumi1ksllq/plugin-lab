#include "ChildProtocol.h"
#include "../capture/SweepRunner.h"
#include "../signal/Impulse.h"
#include "../signal/SineSweep.h"
#include "../signal/MultiTone.h"
#include "../utils/CrashLog.h"

#include <iostream>

#ifdef JUCE_WINDOWS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

// This TU touches plugin-DLL code paths (VST3 create/initialise) and is
// compiled with /EHa (root CMakeLists.txt set_source_files_properties) so the
// catch (...) in handleLoad also intercepts SEH faults from a misbehaving
// plugin — same mechanism as PluginManager.cpp / SweepRunner.cpp.

namespace
{
    // Error vocabulary mirrors source/ipc/CommandParser.cpp (path required /
    // plugin not found / invalid JSON) plus child-specific additions
    // (load timeout / not implemented).
    constexpr int    kLoadTimeoutMs    = 30000;
    constexpr double kDefaultSampleRate = 48000.0;
    constexpr int    kDefaultBlockSize = 512;
    constexpr double kFlushIntervalSec = 5.0;  // WAV crash mirror interval (ADR-D-5)
}

/** Out-of-process VST3 host child (Block D). Reads one JSON request line from
    stdin, dispatches on "cmd", writes one JSON response line to stdout. */
class PluginHostChild
{
public:
    PluginHostChild()
    {
        formatManager.addFormat (std::make_unique<juce::VST3PluginFormat>());
    }

    /** One request line in, one response line out. */
    juce::String handleCommand (const juce::String& jsonCommand)
    {
        auto json = juce::JSON::parse (jsonCommand);
        auto* obj = json.getDynamicObject();

        if (obj == nullptr)
            return ChildProtocol::makeResponse (false, R"("error":"invalid JSON")");

        auto cmd = obj->getProperty ("cmd").toString();

        if (cmd == ChildProtocol::Command::start)
            return handleStart();
        if (cmd == ChildProtocol::Command::load)
            return handleLoad (*obj);
        if (cmd == ChildProtocol::Command::heartbeat)
            return ChildProtocol::makeResponse (true);
        if (cmd == ChildProtocol::Command::measure)
            return handleMeasure (*obj);
        if (cmd == ChildProtocol::Command::snapshotParams)
            return handleSnapshotParams();
        if (cmd == ChildProtocol::Command::restoreParams)
            return handleRestoreParams (*obj);
        if (cmd == ChildProtocol::Command::stop)
        {
            stopRequested = true;
            return ChildProtocol::makeResponse (true);
        }

        return ChildProtocol::makeResponse (false, R"("error":"unknown cmd")");
    }

    /** True once a stop command has been acknowledged — main() exits cleanly. */
    bool wantsStop() const noexcept { return stopRequested; }

private:
    juce::String handleStart()
    {
        return ChildProtocol::makeResponse (true,
            R"("pid":)" + juce::String (static_cast<juce::int64> (::GetCurrentProcessId()))
            + R"(,"version":1)");
    }

    juce::String handleLoad (const juce::DynamicObject& obj)
    {
        auto path = obj.getProperty ("path").toString();
        if (path.isEmpty())
            return ChildProtocol::makeResponse (false, R"("error":"path required")");

        auto* format = formatManager.getFormat (0);
        if (format == nullptr)
            return ChildProtocol::makeResponse (false, R"("error":"no VST3 format")");

        try
        {
            // Description-from-file: the VST3 host validates desc.uniqueId /
            // desc.deprecatedUid against the module at creation time, so a
            // hand-built minimal PluginDescription (uid = 0) cannot be loaded —
            // scan the file for the real description instead (same route as
            // tools/VST3Scanner.cpp).
            juce::OwnedArray<juce::PluginDescription> found;
            format->findAllTypesForFile (found, path);
            if (found.isEmpty())
                return ChildProtocol::makeResponse (false, R"("error":"plugin not found")");

            // DynamicObject::getProperty has no default-value overload in
            // JUCE 9 — apply the contract defaults (48 kHz / 512) when absent.
            const auto sampleRateVar = obj.getProperty ("sample_rate");
            const double sampleRate = sampleRateVar.isVoid() ? kDefaultSampleRate
                                                             : static_cast<double> (sampleRateVar);
            const auto blockSizeVar = obj.getProperty ("block_size");
            const int blockSize = blockSizeVar.isVoid() ? kDefaultBlockSize
                                                        : static_cast<int> (blockSizeVar);

            // Mirror PluginManager::loadPlugin (PluginManager.cpp:438-520):
            // explicit createPluginInstanceAsync + WaitableEvent timeout.
            // createPluginInstanceAsync postMessage()s the creation to the
            // message thread (AudioPluginFormat::handleMessage) — the child's
            // main thread IS the message thread, so pump the dispatch loop
            // while waiting. Late-callback protection: LoadState is held by
            // shared_ptr; on timeout alive=false and the still-arriving
            // callback is discarded without touching this object.
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
                    return;                          // late callback (load timed out) → ignore
                state->instance = std::move (instance);
                state->error = errorMessage;
                state->done.signal();
            };

            CRASH_LOG_INFO ("Child plugin load start", path);
            format->createPluginInstanceAsync (*found.getFirst(), sampleRate, blockSize, std::move (onCreated));

            // The host's heartbeat watchdog (default 3 s) treats silence as
            // death and TerminateProcesses us. Plugin creation can legitimately
            // take much longer than that (load timeout is 30 s), so emit a
            // progress line every ~500 ms while waiting — progress lines are
            // the protocol's liveness signal (contract table: {"ok":true,
            // "progress":0.10} 心跳/进度行).
            const auto deadline = juce::Time::getMillisecondCounter() + static_cast<juce::uint32> (kLoadTimeoutMs);
            auto lastProgressMs = juce::Time::getMillisecondCounter();
            while (! state->done.wait (20))
            {
                if (juce::Time::getMillisecondCounter() >= deadline)
                {
                    state->alive = false;
                    CRASH_LOG_WARN ("Child plugin load timeout",
                        path + " - creation did not finish in "
                        + juce::String (kLoadTimeoutMs) + "ms");
                    return ChildProtocol::makeResponse (false, R"("error":"load timeout")");
                }

                const auto nowMs = juce::Time::getMillisecondCounter();
                if (nowMs - lastProgressMs >= 500)
                {
                    lastProgressMs = nowMs;
                    std::cout << ChildProtocol::makeProgress (0.0f).toStdString() << std::flush;
                }

                juce::MessageManager::getInstance()->runDispatchLoopUntil (20);
            }

            if (! state->instance)
            {
                auto errorText = "load failed" + (state->error.isNotEmpty() ? ": " + state->error : juce::String());
                CRASH_LOG_WARN ("Child plugin load failed", path + " - " + errorText);
                return ChildProtocol::makeResponse (false,
                    R"("error":")" + ChildProtocol::escapeJsonString (errorText) + "\"");
            }

            pluginInstance = std::move (state->instance);
            CRASH_LOG_INFO ("Child plugin load ok", path);
            return ChildProtocol::makeResponse (true,
                R"("name":")" + ChildProtocol::escapeJsonString (found.getFirst()->name) + "\"");
        }
        catch (...)
        {
            // /EHa: also intercepts SEH faults from the plugin DLL during
            // scanning/creation. A real process termination (ExitProcess/abort)
            // still kills only this child — that is the point of the child
            // process design (D3 handles detection + restart on the host side).
            CRASH_LOG_ERR ("Child plugin load", path + " - exception/SEH during create");
            return ChildProtocol::makeResponse (false, R"("error":"load failed")");
        }
    }

    //==============================================================================
    /** Measure: {type, source, excitation?, sample_rate?, block_size?,
        export_path, wav_path?} → {"ok", samples, rate, export_path, wav_path,
        name, class_id, latency_samples}. D2 scope: frequency_response (sweep
        or MLS excitation) + harmonic (MultiTone, T1); compression returns
        "not implemented" per the contract (deferred). */
    juce::String handleMeasure (const juce::DynamicObject& obj)
    {
        auto type = obj.getProperty ("type").toString();

        if (type == "compression")
            return ChildProtocol::makeResponse (false, R"("error":"not implemented")");
        if (type != "frequency_response" && type != "harmonic")
            return ChildProtocol::makeResponse (false, R"("error":"unknown measure type")");
        const bool useMultiTone = (type == "harmonic");

        auto exportPath = obj.getProperty ("export_path").toString();
        if (exportPath.isEmpty())
            return ChildProtocol::makeResponse (false, R"("error":"path required")");

        if (pluginInstance == nullptr)
        {
            CRASH_LOG_WARN ("Child measure", "no plugin loaded");
            return ChildProtocol::makeResponse (false, R"("error":"measurement failed")");
        }

        try
        {
            // Defaults mirror the contract: sweep excitation, 48 kHz, 512.
            // (DynamicObject::getProperty has no default-value overload in
            // JUCE 9 — same pattern as handleLoad.) Unknown non-empty
            // excitation is rejected — mirrors the host path's "unknown
            // excitation '...' (expected sweep|mls)" (CommandParser.cpp:588);
            // only an absent/empty value defaults to sweep.
            const auto excitationVar = obj.getProperty ("excitation");
            const juce::String excitation = excitationVar.isVoid() ? juce::String ("sweep")
                                                                   : excitationVar.toString();
            if (excitation != "sweep" && excitation != "mls")
                return ChildProtocol::makeResponse (false, R"("error":"unknown excitation")");
            const bool useMLS = (excitation == "mls");
            const auto sampleRateVar = obj.getProperty ("sample_rate");
            const double sampleRate = sampleRateVar.isVoid() ? kDefaultSampleRate
                                                             : static_cast<double> (sampleRateVar);
            const auto blockSizeVar = obj.getProperty ("block_size");
            const int blockSize = blockSizeVar.isVoid() ? kDefaultBlockSize
                                                        : static_cast<int> (blockSizeVar);
            const auto wavPath = obj.getProperty ("wav_path").toString();

            // Generator selection mirrors MeasurementSession::run
            // (MeasurementSession.cpp:135-186): sweep → 20 Hz-20 kHz log sweep
            // 5 s, MLS → 16383-sample MLS impulse (both amplitude 0.5);
            // harmonic → MultiTone, 8 octave fundamentals 100..12800 Hz,
            // 3 s, amplitude 0.4. The host's analysis must look for exactly
            // the fundamentals generated here — keep the harmonic constants
            // in lockstep with MeasurementSession.cpp AND
            // ChildWavAnalyzer::analyzeChildHarmonic (each site carries a
            // cross-referencing comment).
            std::unique_ptr<SignalGenerator> gen;
            if (useMultiTone)
            {
                auto multi = std::make_unique<MultiTone>();
                multi->setDuration (3.0);
                multi->setAmplitude (0.4);
                multi->setFrequencies (
                    { 100.0, 200.0, 400.0, 800.0, 1600.0, 3200.0, 6400.0, 12800.0 });
                gen = std::move (multi);
            }
            else if (useMLS)
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

            // Frozen SweepRunner pipeline (same as the host):
            // prepare → setPlugin → setGenerator → run().
            SweepRunner runner;
            runner.prepare (sampleRate, blockSize);
            runner.setPlugin (pluginInstance.get());
            runner.setGenerator (gen.get());

            // Risk 3 (watchdog): run() blocks this thread — no stdin reads, so
            // no heartbeat lines. Every progress callback flushes a progress
            // line to stdout instead; progress lines refresh the host's
            // 3 s heartbeat watchdog (same pattern as the D1 load progress
            // lines), so a 5 s sweep is not killed mid-run.
            runner.setProgressCallback ([] (float progress)
            {
                std::cout << ChildProtocol::makeProgress (progress).toStdString() << std::flush;
            });

            // ADR-D-5: WAV crash mirror — incremental 24-bit interleaved
            // [dry ch0..N-1, wet ch0..N-1] flush to wav_path (interval 5 s).
            // Call after prepare(): the flush interval is derived from the
            // result's sample rate. Empty wav_path disables flushing.
            runner.getResult().setFlushConfig (juce::File (wavPath), kFlushIntervalSec);

            if (! runner.run())
            {
                CRASH_LOG_ERR ("Child measure", "sweep run failed");
                return ChildProtocol::makeResponse (false, R"("error":"measurement failed")");
            }

            auto& result = runner.getResult();
            juce::PluginDescription desc;
            pluginInstance->fillInPluginDescription (desc);

            // channels = plugin input channel count: the host's
            // WavCaptureReader splits the [dry ch0..N-1, wet ch0..N-1] WAV
            // layout (ADR-D-5) and needs N — it has no plugin instance for
            // blacklisted plugins, so the child reports it. Fall back to the
            // recorded dry buffer channel count if the plugin reports 0
            // (e.g. plugins whose channel layout is only known post-prepare).
            int numChannels = pluginInstance->getTotalNumInputChannels();
            if (numChannels <= 0)
                numChannels = result.getDryBuffer().getNumChannels();

            // ADR-D-6: the host has no plugin instance for blacklisted
            // plugins — the child reports the metadata it can read directly.
            // samples/rate shape mirrors CommandParser.cpp:639-643.
            juce::String data = R"("samples":)" + juce::String (result.getNumRecordedSamples())
                + R"(,"rate":)"    + juce::String (result.getSampleRate())
                + R"(,"export_path":")" + ChildProtocol::escapeJsonString (exportPath) + "\""
                + R"(,"wav_path":")" + ChildProtocol::escapeJsonString (wavPath) + "\""
                + R"(,"name":")" + ChildProtocol::escapeJsonString (pluginInstance->getName()) + "\""
                + R"(,"class_id":")" + ChildProtocol::escapeJsonString (desc.fileOrIdentifier) + "\""
                + R"(,"channels":)" + juce::String (numChannels)
                + R"(,"latency_samples":)" + juce::String (pluginInstance->getLatencySamples());
            return ChildProtocol::makeResponse (true, data);
        }
        catch (...)
        {
            // /EHa: also intercepts SEH faults from the plugin DLL during
            // processBlock (SweepRunner guards its own calls — this is the
            // outer net). A real process termination (ExitProcess/abort)
            // still kills only this child; the host detects and restarts.
            CRASH_LOG_ERR ("Child measure", "exception/SEH during measurement");
            return ChildProtocol::makeResponse (false, R"("error":"measurement failed")");
        }
    }

    //==============================================================================
    /** snapshot_params: full parameter state, stable-id keyed (ADR-D-1) →
        {"ok":true,"params":[{"id":...,"value":...},...]}. D3 crash recovery
        needs the state to travel host↔child across a restart; the child holds
        the only plugin instance (the host has none for blacklisted plugins),
        so the child takes the snapshot. Values are normalized 0..1. */
    juce::String handleSnapshotParams()
    {
        if (pluginInstance == nullptr)
            return ChildProtocol::makeResponse (false, R"("error":"no plugin loaded")");

        // Stable-id keyed, mirroring ParameterTimeline's findParam
        // (ParameterTimeline.cpp:6-20) and MeasurementSession's R2 pair
        // collection (MeasurementSession.cpp:53-81): hosted parameters only,
        // empty id → skip (R9 semantics — non-hosted params have no stable id
        // to key a restore on).
        juce::String paramsJson;
        int count = 0;
        for (auto* param : pluginInstance->getParameters())
        {
            auto* hosted = dynamic_cast<juce::HostedAudioProcessorParameter*> (param);
            if (hosted == nullptr || hosted->getParameterID().isEmpty())
                continue;

            if (count > 0)
                paramsJson += ",";
            paramsJson += R"({"id":")" + ChildProtocol::escapeJsonString (hosted->getParameterID()) + "\""
                        + R"(,"value":)" + juce::String (param->getValue(), 6) + "}";
            ++count;
        }
        return ChildProtocol::makeResponse (true, R"("params":[)" + paramsJson + "]");
    }

    //==============================================================================
    /** restore_params: {"params":[{id,value},...]} → set every known parameter
        through the stable-id lookup; unknown ids are skipped (their absence is
        not an error — a snapshot may contain params a newer/older plugin
        build no longer exposes). Values are normalized 0..1 (clamped
        defensively). → {"ok":true} */
    juce::String handleRestoreParams (const juce::DynamicObject& obj)
    {
        auto params = obj.getProperty ("params");
        if (! params.isArray())
            return ChildProtocol::makeResponse (false, R"("error":"params required")");

        if (pluginInstance == nullptr)
            return ChildProtocol::makeResponse (false, R"("error":"no plugin loaded")");

        for (int i = 0; i < params.size(); ++i)
        {
            const auto item = params[i];
            if (! item.isObject())
                continue;
            const auto id = item["id"].toString();
            const auto valueVar = item["value"];
            if (id.isEmpty() || valueVar.isVoid())
                continue;
            const float value = juce::jlimit (0.0f, 1.0f,
                                              static_cast<float> (static_cast<double> (valueVar)));

            // Same 6-line stable-id lookup as ParameterTimeline::findParam
            // (ParameterTimeline.cpp:6-20) — replicated here rather than
            // shared, following the codebase's cross-module precedent
            // (capture/ has no dependency on ipc/, the child links neither).
            for (auto* candidate : pluginInstance->getParameters())
            {
                auto* hosted = dynamic_cast<juce::HostedAudioProcessorParameter*> (candidate);
                if (hosted != nullptr && hosted->getParameterID() == id)
                {
                    candidate->setValueNotifyingHost (value);
                    break;
                }
            }
        }
        return ChildProtocol::makeResponse (true);
    }

    juce::AudioPluginFormatManager formatManager;
    std::unique_ptr<juce::AudioPluginInstance> pluginInstance;
    bool stopRequested = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginHostChild)
};

//==============================================================================
int main()
{
    juce::ScopedJuceInitialiser_GUI initialiser;

    PluginHostChild child;

    std::string line;
    while (std::getline (std::cin, line))
    {
        // trim() also strips the \r of Windows line endings before JSON parse
        auto request = juce::String (line).trim();
        if (request.isEmpty())
            continue;

        auto response = child.handleCommand (request);
        std::cout << response.toStdString() << std::flush;

        if (child.wantsStop())
            break;
    }

    return 0;
}
