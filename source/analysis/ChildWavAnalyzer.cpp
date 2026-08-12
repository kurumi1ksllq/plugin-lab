#include "ChildWavAnalyzer.h"
#include "WavCaptureReader.h"
#include "FreqResponse.h"
#include "HarmonicAnalysis.h"
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

juce::String analyzeChildHarmonic (const juce::File& wavPath,
                                   int numChannels,
                                   double sampleRate,
                                   int blockSize,
                                   const juce::String& pluginName,
                                   const juce::String& classId,
                                   int latencySamples)
{
    juce::AudioBuffer<float> dry;
    juce::AudioBuffer<float> wet;
    if (! WavCaptureReader::readDryWet (wavPath, numChannels, dry, wet))
        return {};

    // The child's generator config mirrors the host's harmonic branch
    // (MeasurementSession.cpp Type::harmonicAnalysis, :156-169): MultiTone,
    // 8 octave fundamentals 100..12800 Hz, 3 s, amplitude 0.4. The analysis
    // must look for exactly the fundamentals the child generated — keep this
    // list in lockstep with MeasurementSession.cpp AND PluginHostChild.cpp
    // handleMeasure (both hardcode the same list; a comment cross-references
    // each site so the two cannot silently drift).
    static const std::vector<double> kFundamentals =
        { 100.0, 200.0, 400.0, 800.0, 1600.0, 3200.0, 6400.0, 12800.0 };

    HarmonicAnalysis analyzer;
    const auto result = analyzer.analyze (wet, sampleRate, kFundamentals);

    // Context built from child-reported metadata + measure-request params
    // (ADR-D-6) — same shape as analyzeChildFrequencyResponse. Non-freq
    // types force the sweep excitation on the host side
    // (CommandParser.cpp:662-665), so the context records "sweep" and the
    // default suppresses it from the export.
    Export::Context ctx;
    ctx.pluginName     = pluginName;
    ctx.classId        = classId;
    ctx.latencySamples = latencySamples;
    ctx.sampleRate     = sampleRate;
    ctx.blockSize      = blockSize;
    ctx.excitation     = "sweep";
    ctx.source.type    = "signal";
    ctx.paramSnapshot  = "{}";   // no host plugin instance → empty snapshot

    return Export::harmonicAnalysisToJSON (result, ctx);
}

} // namespace ChildWavAnalyzer
