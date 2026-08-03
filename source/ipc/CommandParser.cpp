#include "CommandParser.h"
#include "Protocol.h"
#include "../analysis/Export.h"
#include "../analysis/HarmonicAnalysis.h"
#include "../analysis/CompressionCurve.h"
#include "../analysis/GainReduction.h"
#include "../analysis/TimeConstants.h"
#include "../analysis/CompressionFamily.h"

// Maps the protocol source string to the session source enum. Returns false
// when the value is not a known protocol source; callers respond with the
// "unknown source" error.
static bool parseSource (const juce::String& sourceStr, MeasurementSession::Source& source)
{
    if      (sourceStr == Protocol::Source::signal)  source = MeasurementSession::Source::signal;
    else if (sourceStr == Protocol::Source::file)    source = MeasurementSession::Source::file;
    else if (sourceStr == Protocol::Source::noise)   source = MeasurementSession::Source::noise;
    else if (sourceStr == Protocol::Source::dynamic) source = MeasurementSession::Source::dynamic;
    else return false;

    return true;
}

// Applies the source-specific session configuration: the input file path
// (with existence check) for the file source, the noise generator settings
// for the noise source, and the carrier frequency / sweep-start for the
// dynamic source. Returns an error message when the configuration is
// invalid (empty string on success); callers respond with it. Shared by
// the measure and scan commands.
static juce::String configureSessionSource (MeasurementSession& session, const juce::DynamicObject& obj,
                                            MeasurementSession::Source source)
{
    if (source == MeasurementSession::Source::file)
    {
        // For the file source "path" names the INPUT audio file (the
        // export path is disambiguated via "export_path" below).
        auto inputPath = obj.getProperty ("path").toString();
        if (inputPath.isEmpty())
            return R"("error":"path required")";
        juce::File inputFile (inputPath);
        if (! inputFile.existsAsFile())
            return R"("error":"file not found")";
        session.setFilePath (inputFile);
    }
    else if (source == MeasurementSession::Source::noise)
    {
        NoiseGenerator::Type noiseType = NoiseGenerator::Type::white;
        auto noiseTypeStr = obj.getProperty ("noise_type").toString();
        if (noiseTypeStr == "pink")
            noiseType = NoiseGenerator::Type::pink;
        else if (! noiseTypeStr.isEmpty() && noiseTypeStr != "white")
            return R"("error":"unknown noise type")";

        double duration = 2.0;
        if (obj.hasProperty ("duration"))
            duration = static_cast<double> (obj.getProperty ("duration"));

        uint32_t seed = 0x2E42A5;
        if (obj.hasProperty ("seed"))
            seed = static_cast<uint32_t> (static_cast<int64_t> (obj.getProperty ("seed")));

        session.setNoiseConfig (noiseType, duration, seed);
    }
    else if (source == MeasurementSession::Source::dynamic)
    {
        if (obj.hasProperty ("carrier_freq"))
            session.setDynamicCarrierFreq (static_cast<double> (obj.getProperty ("carrier_freq")));

        // The carrier sweep must start high enough that the detector is
        // not polluted by low-frequency carrier wobble (otherwise the GR
        // attack edge is corrupted and tau comes back invalid). Default
        // to 10 kHz, matching CompressionFamily's internal configuration.
        double carrierStartHz = 10000.0;
        if (obj.hasProperty ("carrier_start_hz"))
            carrierStartHz = static_cast<double> (obj.getProperty ("carrier_start_hz"));
        session.setDynamicCarrierStartHz (carrierStartHz);
    }

    return {};
}

// Resolves the export path for a command: "export_path" for the file
// source (where "path" names the input audio file), "path" otherwise.
// Falls back to <current directory>/<defaultFileName> when omitted. The
// default file name differs per command (measure/scan) and per source.
static juce::String resolveExportPath (const juce::DynamicObject& obj, MeasurementSession::Source source,
                                       const juce::String& defaultFileName)
{
    auto path = (source == MeasurementSession::Source::file)
                    ? obj.getProperty ("export_path").toString()
                    : obj.getProperty ("path").toString();
    if (path.isEmpty())
        path = juce::File::getCurrentWorkingDirectory()
                   .getChildFile (defaultFileName)
                   .getFullPathName();
    return path;
}

// Builds the export context for a measurement/scan: plugin identity and
// latency, session sample rate / block size / parameter snapshot, plus the
// input-source metadata (file source: path/sample-rate/resample/duration;
// noise source: type/seed/duration). Shared verbatim by the measure and
// scan commands.
static Export::Context buildExportContext (juce::AudioPluginInstance* plugin, MeasurementSession& session,
                                           MeasurementSession::Source source, const juce::String& sourceStr)
{
    Export::Context ctx;
    ctx.pluginName = plugin->getName();
    {
        juce::PluginDescription desc;
        plugin->fillInPluginDescription (desc);
        ctx.classId = desc.fileOrIdentifier;
    }
    ctx.latencySamples = plugin->getLatencySamples();
    ctx.sampleRate     = session.getSampleRate();
    ctx.blockSize      = session.getBlockSize();
    ctx.paramSnapshot  = session.getParameterSnapshot();

    // Attach the input-source metadata to the export.
    ctx.source.type = sourceStr;
    if (source == MeasurementSession::Source::file)
    {
        ctx.source.filePath         = session.getSourceFilePath();
        ctx.source.sourceSampleRate = session.getSourceSampleRate();
        ctx.source.resampleRatio    = session.getResampleRatio();
        ctx.source.durationSec      = session.getSourceDurationSec();
    }
    else if (source == MeasurementSession::Source::noise)
    {
        ctx.source.noiseType   = (session.getNoiseType() == NoiseGenerator::Type::pink)
                                     ? juce::String ("pink") : juce::String ("white");
        ctx.source.seed        = session.getNoiseSeed();
        ctx.source.durationSec = session.getNoiseDuration();
    }

    return ctx;
}

juce::String CommandParser::handleCommand (const juce::String& jsonCommand)
{
    auto json = juce::JSON::parse (jsonCommand);
    auto* obj = json.getDynamicObject();

    if (obj == nullptr)
        return Protocol::makeResponse (false, R"("error":"invalid JSON")");

    auto cmd = obj->getProperty ("cmd").toString();

    // --- loadPlugin ---
    if (cmd == "loadPlugin")
    {
        auto path = obj->getProperty ("path").toString();
        if (path.isEmpty())
            return Protocol::makeResponse (false, R"("error":"path required")");

        if (pluginManager == nullptr)
            return Protocol::makeResponse (false, R"("error":"no plugin manager")");

        auto& knownPlugins = pluginManager->getKnownPlugins();
        auto types = knownPlugins.getTypes();

        for (auto& d : types)
        {
            // fileOrIdentifier matches exactly (it is a canonical path/ID);
            // the display name matches case-insensitively so callers can
            // address a plugin by a loosely-cased name.
            if (d.fileOrIdentifier == path || d.name.equalsIgnoreCase (path))
            {
                if (loadPluginCallback)
                {
                    auto descCopy = d;
                    juce::MessageManager::callAsync ([this, descCopy] { loadPluginCallback (descCopy); });
                }
                return Protocol::makeResponse (true, R"("name":")" + d.name.quoted() + R"(")");
            }
        }
        return Protocol::makeResponse (false, R"("error":"plugin not found")");
    }

    // --- setParam ---
    if (cmd == "setParam")
    {
        if (plugin == nullptr)
            return Protocol::makeResponse (false, R"("error":"no plugin loaded")");

        auto name = obj->getProperty ("name").toString();
        double value = obj->getProperty ("value");

        auto& params = plugin->getParameters();
        for (int i = 0; i < params.size(); ++i)
        {
            auto n = params[i]->getName (128);
            if (n == name || n.contains (name))
            {
                params[i]->setValueNotifyingHost (static_cast<float> (value));
                return Protocol::makeResponse (true, R"("param":")" + name.quoted()
                                                 + R"(","value":)" + juce::String (value, 4));
            }
        }
        return Protocol::makeResponse (false, R"("error":"parameter not found")");
    }

    // --- getParams ---
    if (cmd == "getParams")
    {
        if (plugin == nullptr)
            return Protocol::makeResponse (false, R"("error":"no plugin loaded")");

        juce::String data = R"("params":[)";
        auto& params = plugin->getParameters();
        for (int i = 0; i < params.size(); ++i)
        {
            auto p = params[i];
            auto n = p->getName (128);
            auto v = p->getValue();

            // Stable parameter ID, resolved the same way the scan command
            // does (see the "scan" case below): hosted parameters expose a
            // stable ID that survives display-name changes; anything else
            // gets an empty id.
            juce::String paramId;
            if (auto* hosted = dynamic_cast<juce::HostedAudioProcessorParameter*> (p))
                paramId = hosted->getParameterID();

            data += R"({"index":)" + juce::String (i)
                  + R"(,"name":")" + n.quoted()
                  + R"(","value":)" + juce::String (v, 4)
                  + R"(,"param_id":")" + paramId + R"("})";
            if (i < params.size() - 1) data += ",";
        }
        data += "]";
        return Protocol::makeResponse (true, data);
    }

    // --- measure ---
    if (cmd == "measure")
    {
        if (session == nullptr || plugin == nullptr)
            return Protocol::makeResponse (false, R"("error":"no session or plugin")");

        // --- input source (default: signal) ---
        auto sourceStr = obj->getProperty ("source").toString();
        if (sourceStr.isEmpty())
            sourceStr = Protocol::Source::signal;

        MeasurementSession::Source source = MeasurementSession::Source::signal;
        if (! parseSource (sourceStr, source))
            return Protocol::makeResponse (false, R"("error":"unknown source")");

        // --- measurement type (analysis field; for raw sources it is only
        //     metadata — defaults to frequency_response when omitted) ---
        auto t = obj->getProperty ("type").toString();
        if (t.isEmpty())
            t = Protocol::MeasureType::freq;
        if (t == Protocol::MeasureType::freq)
            session->setMeasurementType (MeasurementSession::Type::frequencyResponse);
        else if (t == Protocol::MeasureType::harmonic)
            session->setMeasurementType (MeasurementSession::Type::harmonicAnalysis);
        else if (t == Protocol::MeasureType::compression)
            session->setMeasurementType (MeasurementSession::Type::compressionCurve);
        else if (t == Protocol::MeasureType::grTimeline)
            session->setMeasurementType (MeasurementSession::Type::grTimeline);
        else
            return Protocol::makeResponse (false, R"("error":"unknown measure type")");

        // The GR timeline needs a recorded dry/wet pair with dynamics:
        // built-in signal sources have no GR timeline generator.
        if (t == Protocol::MeasureType::grTimeline
            && source == MeasurementSession::Source::signal)
            return Protocol::makeResponse (false,
                R"("error":"gr_timeline requires a non-signal source")");

        // --- source-specific configuration ---
        auto sourceError = configureSessionSource (*session, *obj, source);
        if (sourceError.isNotEmpty())
            return Protocol::makeResponse (false, sourceError);

        session->setSource (source);
        session->setPluginInstance (plugin);

        // Determine export path:
        //   - signal/noise/dynamic → "path" (existing protocol)
        //   - file → "export_path" ("path" names the input audio file)
        auto path = resolveExportPath (*obj, source,
                                       source == MeasurementSession::Source::signal
                                           ? "pluginlab_freq_response.json"
                                           : "pluginlab_raw_capture.json");

        if (statusCallback)
            juce::MessageManager::callAsync ([this] { statusCallback ("Measuring..."); });

        // Body of the measurement: run, analyse, export, build response.
        // Extracted as a reusable lambda so both the sync (message-thread)
        // and async (IPC-thread → message-thread dispatch) paths share it.
        auto runMeasurement = [&]() -> juce::String
        {
            if (! session->run())
                return Protocol::makeResponse (false, R"("error":"measurement failed")");

            auto& result = session->getResult();

            Export::Context ctx = buildExportContext (plugin, *session, source, sourceStr);

            MeasurementResults results;
            results.type   = session->getType();
            results.source = sourceStr;

            juce::String exportJson;

            if (source == MeasurementSession::Source::signal)
            {
                // Dispatch the analysis to the analyzer matching the session type.
                switch (session->getType())
                {
                    case MeasurementSession::Type::frequencyResponse:
                    {
                        FreqResponse fr;
                        fr.setLatencySamples (plugin->getLatencySamples());
                        results.freq = fr.analyze (result.getDryBuffer(),
                                                   result.getWetBuffer(),
                                                   result.getSampleRate());
                        exportJson = Export::freqResponseToJSON (results.freq, ctx);
                        break;
                    }

                    case MeasurementSession::Type::harmonicAnalysis:
                    {
                        HarmonicAnalysis ha;
                        results.harmonic = ha.analyze (result.getWetBuffer(),
                                                       result.getSampleRate(),
                                                       session->getFundamentalFreqs());
                        exportJson = Export::harmonicAnalysisToJSON (results.harmonic, ctx);
                        break;
                    }

                    case MeasurementSession::Type::compressionCurve:
                    {
                        CompressionCurve cc;
                        results.compression = cc.analyze (result.getDryBuffer(),
                                                          result.getWetBuffer(),
                                                          result.getSampleRate(),
                                                          session->getInputLevelsDB());
                        exportJson = Export::compressionCurveToJSON (results.compression, ctx);
                        break;
                    }

                    // Unreachable — the parser rejects gr_timeline for
                    // Source::signal (defensive, keeps the enum switch
                    // exhaustive).
                    case MeasurementSession::Type::grTimeline:
                        return Protocol::makeResponse (false,
                            R"("error":"gr_timeline requires a non-signal source")");
                }
            }
            else if (t == Protocol::MeasureType::grTimeline)
            {
                // GR timeline analysis (T4.4): gain reduction over the
                // recorded dry/wet pair, then attack/release time constants.
                //
                // - GainReduction reports the wet/dry ratio (negative dB for
                //   a compressor); the exported timeline keeps that convention.
                // - TimeConstants needs controlled envelope edges: the dynamic
                //   source has them (auto-detected via CompressionFamily); the
                //   file/noise sources are real-world material with no
                //   controlled edges, so their markers stay empty and the tau
                //   estimate is invalid by design (GR timeline still exported).
                // - A 1 ms RMS window (matching CompressionFamily's internal
                //   configuration) keeps enough timeline points to resolve a
                //   few-ms attack edge; the default 5 ms window blurs it away.
                results.gr = GainReduction::analyze (result.getDryBuffer(),
                                                     result.getWetBuffer(),
                                                     result.getSampleRate(),
                                                     plugin->getLatencySamples(),
                                                     0.001);

                TimeConstants::EventMarkers markers;
                if (source == MeasurementSession::Source::dynamic)
                {
                    // detectMarkers and TimeConstants expect positive dB =
                    // reduction; GainReduction reports the wet/dry ratio
                    // (negative dB for a compressor). Negate a copy for the
                    // edge detection AND the tau estimate (CompressionFamily
                    // pattern — the exported timeline is NOT negated).
                    auto grPositive = results.gr;
                    for (auto& p : grPositive.timeline)
                        p.grDB = -p.grDB;
                    markers = CompressionFamily::detectMarkers (grPositive);
                    results.tau = TimeConstants::estimate (grPositive, markers,
                                                           result.getSampleRate());
                }
                else
                {
                    // file/noise sources have no controlled envelope edges:
                    // markers stay empty and the tau estimate is invalid by
                    // design (GR timeline still exported).
                    results.tau = TimeConstants::estimate (results.gr, markers,
                                                           result.getSampleRate());
                }

                exportJson = Export::grTimelineToJSON (results.gr, results.tau, ctx);
            }
            else
            {
                // Raw capture: record-only export, no analysis (phase 4).
                results.rawSamples    = result.getNumRecordedSamples();
                results.rawSampleRate = result.getSampleRate();
                exportJson = Export::rawCaptureToJSON (result.getNumRecordedSamples(),
                                                       result.getSampleRate(),
                                                       session->getBlockSize(),
                                                       ctx);
            }

            juce::File exportFile (path);
            Export::writeToFile (exportJson, exportFile);

            // measurementCompleteCallback is fired synchronously on the
            // measurement thread — unit tests assert this timing.
            if (measurementCompleteCallback)
                measurementCompleteCallback (results);

            juce::String d = R"("samples":)" + juce::String (result.getNumRecordedSamples())
                           + R"(,"rate":)"    + juce::String (result.getSampleRate())
                           + R"(,"export_path":")" + path.quoted() + R"(")";
            return Protocol::makeResponse (true, d);
        };

        // Dispatch strategy:
        //   - already on the message thread (unit tests, or when called from
        //     within a message callback):  execute synchronously.
        //   - any other thread (real IPC PipeServer thread):  dispatch via
        //     callAsync + WaitableEvent so processBlock runs on the message
        //     thread (required by Pro-Q 4 and similar VST3 plugins).
        if (juce::MessageManager::getInstance()->isThisTheMessageThread())
        {
            return runMeasurement();
        }

        juce::WaitableEvent done;
        juce::String response;

        juce::MessageManager::callAsync ([&]
        {
            response = runMeasurement();
            done.signal();
        });

        done.wait();
        return response;
    }

    // --- scan ---
    if (cmd == "scan")
    {
        if (session == nullptr || plugin == nullptr)
            return Protocol::makeResponse (false, R"("error":"no session or plugin")");

        // --- measurement type (analysis field; default frequency_response) ---
        auto t = obj->getProperty ("type").toString();
        if (t.isEmpty())
            t = Protocol::MeasureType::freq;

        MeasurementSession::Type scanType;
        if (t == Protocol::MeasureType::freq)
            scanType = MeasurementSession::Type::frequencyResponse;
        else if (t == Protocol::MeasureType::harmonic)
            scanType = MeasurementSession::Type::harmonicAnalysis;
        else if (t == Protocol::MeasureType::compression)
            scanType = MeasurementSession::Type::compressionCurve;
        else
            return Protocol::makeResponse (false, R"("error":"unknown measure type")");

        // --- scanned parameter (located by stable ID) ---
        auto paramId = obj->getProperty ("param_id").toString();
        if (paramId.isEmpty())
            return Protocol::makeResponse (false, R"("error":"param_id required")");

        juce::AudioProcessorParameter* scanParam = nullptr;
        {
            auto& params = plugin->getParameters();
            for (auto* candidate : params)
            {
                auto* hosted = dynamic_cast<juce::HostedAudioProcessorParameter*> (candidate);
                if (hosted != nullptr && hosted->getParameterID() == paramId)
                {
                    scanParam = candidate;
                    break;
                }
            }
        }
        if (scanParam == nullptr)
            return Protocol::makeResponse (false, R"("error":"parameter not found")");

        // --- values (normalized 0..1; must be non-empty) ---
        auto valuesVar = obj->getProperty ("values");
        if (! valuesVar.isArray() || valuesVar.size() == 0)
            return Protocol::makeResponse (false, R"("error":"values array required")");

        std::vector<float> values;
        values.reserve (static_cast<size_t> (valuesVar.size()));
        for (int i = 0; i < valuesVar.size(); ++i)
        {
            const double v = static_cast<double> (valuesVar[i]);
            if (v < 0.0 || v > 1.0)
                return Protocol::makeResponse (false, R"#("error":"values out of range (0..1)")#");
            values.push_back (static_cast<float> (v));
        }

        // --- input source (default: signal; other sources capture raw per
        //     round — analysis still runs via ScanEngine) ---
        auto sourceStr = obj->getProperty ("source").toString();
        if (sourceStr.isEmpty())
            sourceStr = Protocol::Source::signal;

        MeasurementSession::Source source = MeasurementSession::Source::signal;
        if (! parseSource (sourceStr, source))
            return Protocol::makeResponse (false, R"("error":"unknown source")");

        // --- source-specific configuration (mirrors the measure command) ---
        auto sourceError = configureSessionSource (*session, *obj, source);
        if (sourceError.isNotEmpty())
            return Protocol::makeResponse (false, sourceError);

        session->setSource (source);
        session->setPluginInstance (plugin);

        // --- export path (default pluginlab_scan.json) ---
        auto path = resolveExportPath (*obj, source, "pluginlab_scan.json");

        if (statusCallback)
            juce::MessageManager::callAsync ([this] { statusCallback ("Scanning..."); });

        // Body of the scan: run the engine, export, build the response.
        // Extracted as a reusable lambda so both the sync (message-thread)
        // and async (IPC-thread → message-thread dispatch) paths share it.
        auto runScan = [&]() -> juce::String
        {
            ScanEngine engine;
            engine.setPluginInstance (plugin);
            engine.setSession (session);

            // Per-round progress → status callback (e.g. "scan round 2/5").
            auto scanResult = engine.run (paramId, values, scanType,
                [this] (int round, int totalRounds)
                {
                    juce::MessageManager::callAsync ([this, round, totalRounds]
                    {
                        if (statusCallback)
                            statusCallback ("scan round " + juce::String (round)
                                            + "/" + juce::String (totalRounds));
                    });
                });

            // No rounds completed (round 1 failed or the scan was aborted
            // before the first round): report the error.
            if (scanResult.family.empty())
                return Protocol::makeResponse (false, R"("error":"scan failed")");

            Export::Context ctx = buildExportContext (plugin, *session, source, sourceStr);

            auto exportJson = Export::scanToJSON (scanResult, scanType, ctx);
            juce::File exportFile (path);
            Export::writeToFile (exportJson, exportFile);

            // scanCompleteCallback fires synchronously on the measurement
            // thread — unit tests assert this timing (measure pattern).
            if (scanCompleteCallback)
                scanCompleteCallback (scanResult);

            juce::String d = R"("runs":)" + juce::String (static_cast<int> (scanResult.family.size()))
                           + R"(,"export_path":")" + path.quoted() + R"(")";
            return Protocol::makeResponse (true, d);
        };

        // Dispatch strategy mirrors the measure command: synchronous on the
        // message thread, callAsync + WaitableEvent from any other thread.
        if (juce::MessageManager::getInstance()->isThisTheMessageThread())
            return runScan();

        juce::WaitableEvent done;
        juce::String response;
        juce::MessageManager::callAsync ([&] { response = runScan(); done.signal(); });
        done.wait();
        return response;
    }

    // --- stop ---
    if (cmd == "stop")
    {
        if (session != nullptr)
            session->cancel();
        return Protocol::makeResponse (true);
    }

    return Protocol::makeResponse (false, R"("error":"unknown cmd")");
}
