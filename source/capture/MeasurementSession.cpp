#include "MeasurementSession.h"
#include "../signal/SineSweep.h"
#include "../signal/MultiTone.h"
#include "../signal/ToneBurst.h"

void MeasurementSession::setPluginInstance (juce::AudioPluginInstance* p)
{
    plugin = p;
}

void MeasurementSession::setPluginDescription (const juce::PluginDescription& desc)
{
    pluginDesc = desc;
}

void MeasurementSession::setMeasurementType (Type t)
{
    type = t;
}

void MeasurementSession::captureParameterSnapshot()
{
    if (plugin == nullptr)
    {
        paramSnapshot = "{}";
        return;
    }

    juce::String json = "{\n";
    auto& params = plugin->getParameters();

    for (int i = 0; i < params.size(); ++i)
    {
        auto* param = params[i];
        auto value = param->getValue();

        json += "  \"" + param->getName (64) + "\": "
                + juce::String (value, 4);
        if (i < params.size() - 1) json += ",";
        json += "\n";
    }

    json += "}";
    paramSnapshot = json;
}

bool MeasurementSession::run()
{
    if (plugin == nullptr)
        return false;

    // Prepare the runner
    runner.prepare (sampleRate, blockSize);
    runner.setPlugin (plugin);

    // Create the appropriate signal generator
    std::unique_ptr<SignalGenerator> gen;

    switch (type)
    {
        case Type::frequencyResponse:
        {
            auto sweep = std::make_unique<SineSweep>();
            sweep->setFrequencyRange (20.0, 20000.0);
            sweep->setDuration (5.0);
            sweep->setAmplitude (0.5);
            gen = std::move (sweep);
            break;
        }

        case Type::harmonicAnalysis:
        {
            auto multi = std::make_unique<MultiTone>();
            multi->setDuration (3.0);
            multi->setAmplitude (0.4);
            gen = std::move (multi);
            break;
        }

        case Type::compressionCurve:
        {
            auto bursts = std::make_unique<ToneBurst>();
            bursts->setFrequency (1000.0);
            gen = std::move (bursts);
            break;
        }
    }

    if (gen == nullptr)
        return false;

    runner.setGenerator (gen.get());

    // Wire progress callback
    runner.setProgressCallback ([this] (float p)
    {
        lastProgress = p;
        if (progressCallback)
            progressCallback (p);
    });

    // Capture the parameter snapshot before running
    captureParameterSnapshot();

    // Run
    return runner.run();
}
