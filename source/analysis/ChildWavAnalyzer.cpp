#include "ChildWavAnalyzer.h"
#include "WavCaptureReader.h"
#include "FreqResponse.h"
#include "Export.h"

namespace ChildWavAnalyzer
{

juce::String analyzeChildFrequencyResponse (const juce::File& wavPath,
                                            int numChannels,
                                            double sampleRate,
                                            int blockSize,
                                            const juce::String& excitation,
                                            int mlsLength,
                                            const juce::String& pluginName,
                                            const juce::String& classId,
                                            int latencySamples)
{
    juce::AudioBuffer<float> dry;
    juce::AudioBuffer<float> wet;
    if (! WavCaptureReader::readDryWet (wavPath, numChannels, dry, wet))
        return {};

    // Latency must be set BEFORE analyze() so the linear phase ramp from the
    // plugin delay is compensated (FreqResponse.h:40).
    FreqResponse fr;
    fr.setLatencySamples (latencySamples);

    FreqResponse::Result result;
    if (excitation == "mls")
        result = fr.analyzeMLS (dry, wet, sampleRate, mlsLength);
    else
        result = fr.analyze (dry, wet, sampleRate);

    // Context built from child-reported metadata + measure-request params
    // (ADR-D-6); the in-process buildExportContext path is untouched.
    Export::Context ctx;
    ctx.pluginName     = pluginName;
    ctx.classId        = classId;
    ctx.latencySamples = latencySamples;
    ctx.sampleRate     = sampleRate;
    ctx.blockSize      = blockSize;
    ctx.excitation     = excitation;
    ctx.source.type    = "signal";
    ctx.paramSnapshot  = "{}";   // no host plugin instance → empty snapshot

    return Export::freqResponseToJSON (result, ctx);
}

} // namespace ChildWavAnalyzer
