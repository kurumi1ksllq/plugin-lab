#include "MeasurementSession.h"
#include "../signal/SineSweep.h"
#include "../signal/MultiTone.h"
#include "../signal/ToneBurst.h"
#include "../signal/FilePlayback.h"
#include "../signal/NoiseGenerator.h"
#include "../signal/EnvelopeSignal.h"
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

void MeasurementSession::setNoiseConfig (NoiseGenerator::Type t, double durationSec, uint32_t seed)
{
    noiseType = t;
    noiseDuration = durationSec;
    noiseSeed = seed;
}

void MeasurementSession::setDynamicADSR (double attackSec, double decaySec,
                                         double sustain, double releaseSec)
{
    dynamicADSR[0] = attackSec;
    dynamicADSR[1] = decaySec;
    dynamicADSR[2] = sustain;
    dynamicADSR[3] = releaseSec;
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

    // Create the signal generator for the configured input source.
    // The source decides the signal; Type stays an analysis field (only
    // used on the Source::signal path).
    std::unique_ptr<SignalGenerator> gen;

    switch (source)
    {
        case Source::signal:
        {
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
            break;
        }

        case Source::file:
        {
            // The file is opened inside FilePlayback::prepare() (called by the
            // runner); a missing file would fall back to 10 s of silence, so
            // reject it up front instead.
            if (! filePath.existsAsFile())
                return false;

            auto playback = std::make_unique<FilePlayback>();
            playback->setFile (filePath);
            gen = std::move (playback);
            break;
        }

        case Source::noise:
        {
            auto noise = std::make_unique<NoiseGenerator>();
            noise->setType (noiseType);
            noise->setDuration (noiseDuration);
            noise->setAmplitude (0.3);
            noise->setSeed (noiseSeed);
            gen = std::move (noise);
            break;
        }

        case Source::dynamic:
        {
            // 2 s sine sweep (start frequency configurable) shaped by the
            // (configurable) ADSR envelope. The carrier amplitude, envelope
            // speed and edge times drive the dynamic level used by the
            // compression-family sweep (T4.3); the defaults reproduce the
            // original dynamic-source signal exactly.
            auto sweep = std::make_unique<SineSweep>();
            sweep->setFrequencyRange (dynamicCarrierStartHz, 20000.0);
            sweep->setDuration (2.0);
            sweep->setAmplitude (dynamicAmplitude);

            auto env = std::make_unique<EnvelopeSignal> (std::move (sweep));
            env->setEnvelope (EnvelopeSignal::Envelope::adsr);
            env->setADSR (dynamicADSR[0], dynamicADSR[1], dynamicADSR[2], dynamicADSR[3]);
            env->setSpeed (dynamicSpeed);
            gen = std::move (env);
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
    if (! runner.run())
        return false;

    // Capture source metadata from the generator for the export (file only).
    if (auto* playback = dynamic_cast<FilePlayback*> (gen.get()))
    {
        sourceFilePath    = playback->getSourcePath();
        sourceSampleRate  = playback->getSourceSampleRate();
        resampleRatio     = playback->getResampleRatio();
        sourceDurationSec = playback->getDurationSec();
    }

    return true;
}

std::vector<double> MeasurementSession::getInputLevelsDB() const
{
    std::vector<double> levelsDB;
    levelsDB.reserve (lastLevels.size());

    for (double amp : lastLevels)
        levelsDB.push_back (MathUtils::amplitudeToDB (amp));

    return levelsDB;
}
