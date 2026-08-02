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

        auto t = obj->getProperty ("type").toString();
        if (t == Protocol::MeasureType::freq)
            session->setMeasurementType (MeasurementSession::Type::frequencyResponse);
        else if (t == Protocol::MeasureType::harmonic)
            session->setMeasurementType (MeasurementSession::Type::harmonicAnalysis);
        else if (t == Protocol::MeasureType::compression)
            session->setMeasurementType (MeasurementSession::Type::compressionCurve);
        else
            return Protocol::makeResponse (false, R"("error":"unknown measure type")");

        session->setPluginInstance (plugin);

        // Determine export path (capture before dispatching)
        auto path = obj->getProperty ("path").toString();
        if (path.isEmpty())
            path = juce::File::getCurrentWorkingDirectory()
                       .getChildFile ("pluginlab_freq_response.json")
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

            // Dispatch the analysis to the analyzer matching the session type.
            MeasurementResults results;
            results.type = session->getType();

            juce::String exportJson;

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
