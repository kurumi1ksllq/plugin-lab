#include "CommandParser.h"
#include "Protocol.h"
#include "../analysis/Export.h"

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

        if (statusCallback)
            juce::MessageManager::callAsync ([this] { statusCallback ("Measuring..."); });

        bool ok = session->run();

        if (ok)
        {
            auto& r = session->getResult();

            // Run frequency response analysis
            FreqResponse fr;
            fr.setLatencySamples (plugin->getLatencySamples());
            auto frResult = fr.analyze (r.getDryBuffer(),
                                        r.getWetBuffer(),
                                        r.getSampleRate());

            // Build export context
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

            // Determine export path
            auto path = obj->getProperty ("path").toString();
            if (path.isEmpty())
                path = juce::File::getCurrentWorkingDirectory()
                           .getChildFile ("pluginlab_freq_response.json")
                           .getFullPathName();

            // Export to file
            auto exportJson = Export::freqResponseToJSON (frResult, ctx);
            juce::File exportFile (path);
            Export::writeToFile (exportJson, exportFile);

            // Fire callback on the message thread (the IPC thread never touches UI)
            if (measurementCompleteCallback)
            {
                auto resultCopy = frResult;
                juce::MessageManager::callAsync ([this, resultCopy]
                {
                    measurementCompleteCallback (resultCopy);
                });
            }

            juce::String d = R"("samples":)" + juce::String (r.getNumRecordedSamples())
                           + R"(,"rate":)"    + juce::String (r.getSampleRate())
                           + R"(,"export_path":")" + path.quoted() + R"(")";
            return Protocol::makeResponse (true, d);
        }
        return Protocol::makeResponse (false, R"("error":"measurement failed")");
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
