#include "CommandParser.h"
#include "Protocol.h"
#include "../analysis/Export.h"
#include "../analysis/HarmonicAnalysis.h"
#include "../analysis/CompressionCurve.h"

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
            if (d.fileOrIdentifier == path || d.name == path)
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
            data += R"({"index":)" + juce::String (i)
                  + R"(,"name":")" + n.quoted()
                  + R"(","value":)" + juce::String (v, 4) + R"(})";
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
        if      (sourceStr == Protocol::Source::signal)  source = MeasurementSession::Source::signal;
        else if (sourceStr == Protocol::Source::file)    source = MeasurementSession::Source::file;
        else if (sourceStr == Protocol::Source::noise)   source = MeasurementSession::Source::noise;
        else if (sourceStr == Protocol::Source::dynamic) source = MeasurementSession::Source::dynamic;
        else
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
        else
            return Protocol::makeResponse (false, R"("error":"unknown measure type")");

        // --- source-specific configuration ---
        if (source == MeasurementSession::Source::file)
        {
            // For the file source "path" names the INPUT audio file (the
            // export path is disambiguated via "export_path" below).
            auto inputPath = obj->getProperty ("path").toString();
            if (inputPath.isEmpty())
                return Protocol::makeResponse (false, R"("error":"path required")");
            juce::File inputFile (inputPath);
            if (! inputFile.existsAsFile())
                return Protocol::makeResponse (false, R"("error":"file not found")");
            session->setFilePath (inputFile);
        }
        else if (source == MeasurementSession::Source::noise)
        {
            NoiseGenerator::Type noiseType = NoiseGenerator::Type::white;
            auto noiseTypeStr = obj->getProperty ("noise_type").toString();
            if (noiseTypeStr == "pink")
                noiseType = NoiseGenerator::Type::pink;
            else if (! noiseTypeStr.isEmpty() && noiseTypeStr != "white")
                return Protocol::makeResponse (false, R"("error":"unknown noise type")");

            double duration = 2.0;
            if (obj->hasProperty ("duration"))
                duration = static_cast<double> (obj->getProperty ("duration"));

            uint32_t seed = 0x2E42A5;
            if (obj->hasProperty ("seed"))
                seed = static_cast<uint32_t> (static_cast<int64_t> (obj->getProperty ("seed")));

            session->setNoiseConfig (noiseType, duration, seed);
        }
        else if (source == MeasurementSession::Source::dynamic)
        {
            if (obj->hasProperty ("carrier_freq"))
                session->setDynamicCarrierFreq (static_cast<double> (obj->getProperty ("carrier_freq")));
        }

        session->setSource (source);
        session->setPluginInstance (plugin);

        // Determine export path:
        //   - signal/noise/dynamic → "path" (existing protocol)
        //   - file → "export_path" ("path" names the input audio file)
        auto path = (source == MeasurementSession::Source::file)
                        ? obj->getProperty ("export_path").toString()
                        : obj->getProperty ("path").toString();
        if (path.isEmpty())
            path = juce::File::getCurrentWorkingDirectory()
                       .getChildFile (source == MeasurementSession::Source::signal
                                          ? "pluginlab_freq_response.json"
                                          : "pluginlab_raw_capture.json")
                       .getFullPathName();

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

            Export::Context ctx;
            ctx.pluginName = plugin->getName();
            {
                juce::PluginDescription desc;
                plugin->fillInPluginDescription (desc);
                ctx.classId = desc.fileOrIdentifier;
            }
            ctx.latencySamples = plugin->getLatencySamples();
            ctx.sampleRate     = session->getSampleRate();
            ctx.blockSize       = session->getBlockSize();
            ctx.paramSnapshot   = session->getParameterSnapshot();

            // Attach the input-source metadata to the export.
            ctx.source.type = sourceStr;
            if (source == MeasurementSession::Source::file)
            {
                ctx.source.filePath         = session->getSourceFilePath();
                ctx.source.sourceSampleRate = session->getSourceSampleRate();
                ctx.source.resampleRatio    = session->getResampleRatio();
                ctx.source.durationSec      = session->getSourceDurationSec();
            }
            else if (source == MeasurementSession::Source::noise)
            {
                ctx.source.noiseType   = (session->getNoiseType() == NoiseGenerator::Type::pink)
                                             ? juce::String ("pink") : juce::String ("white");
                ctx.source.seed        = session->getNoiseSeed();
                ctx.source.durationSec = session->getNoiseDuration();
            }

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
                }
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

    // --- stop ---
    if (cmd == "stop")
    {
        if (session != nullptr)
            session->cancel();
        return Protocol::makeResponse (true);
    }

    return Protocol::makeResponse (false, R"("error":"unknown cmd")");
}
