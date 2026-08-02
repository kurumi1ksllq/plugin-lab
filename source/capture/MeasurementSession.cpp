#include "MeasurementSession.h"
#include "../signal/SineSweep.h"
#include "../signal/MultiTone.h"
#include "../signal/ToneBurst.h"
#include "../utils/MathUtils.h"

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
            // Explicit frequency list shared with HarmonicAnalysis: the
            // analysis must know exactly which fundamentals to look for.
            static const std::vector<double> kFundamentals =
                { 100.0, 200.0, 400.0, 800.0, 1600.0, 3200.0, 6400.0, 12800.0 };

            auto multi = std::make_unique<MultiTone>();
            multi->setDuration (3.0);
            multi->setAmplitude (0.4);
            multi->setFrequencies (kFundamentals);
            fundamentalFreqs = kFundamentals;
            gen = std::move (multi);
            break;
        }

        case Type::compressionCurve:
        {
            auto bursts = std::make_unique<ToneBurst>();
            bursts->setFrequency (1000.0);
            lastLevels = bursts->getLevels();
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

std::vector<double> MeasurementSession::getInputLevelsDB() const
{
    std::vector<double> levelsDB;
    levelsDB.reserve (lastLevels.size());

    for (double amp : lastLevels)
        levelsDB.push_back (MathUtils::amplitudeToDB (amp));

    return levelsDB;
}
