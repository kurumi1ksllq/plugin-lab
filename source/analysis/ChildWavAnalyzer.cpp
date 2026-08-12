#include "ChildWavAnalyzer.h"
#include "WavCaptureReader.h"
#include "FreqResponse.h"
#include "HarmonicAnalysis.h"
#include "CompressionCurve.h"
#include "CompressionFamily.h"
#include "GainReduction.h"
#include "TimeConstants.h"
#include "Export.h"
#include "../utils/MathUtils.h"

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

juce::String analyzeChildCompression (const juce::File& wavPath,
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

    // The child's generator config mirrors the host's compression branch
    // (MeasurementSession.cpp Type::compressionCurve, :172-179): ToneBurst,
    // 1000 Hz, the constructor-default 9 amplitude levels {0.01..0.9}, 50 ms
    // burst + 150 ms gap (prepare() defaults), master amplitude 1.0. The
    // analysis needs exactly these levels (the dB list's size drives
    // CompressionCurve's burst segmentation; the values mirror
    // MeasurementSession::getInputLevelsDB, :307-316, which derives them
    // from the same amplitudes via MathUtils::amplitudeToDB) — keep the
    // list in lockstep with MeasurementSession.cpp AND PluginHostChild.cpp
    // handleMeasure (each site carries a cross-referencing comment so the
    // two cannot silently drift).
    static const std::vector<double> kLevels =
        { 0.01, 0.02, 0.05, 0.1, 0.2, 0.3, 0.5, 0.7, 0.9 };
    std::vector<double> inputLevelsDB;
    inputLevelsDB.reserve (kLevels.size());
    for (double level : kLevels)
        inputLevelsDB.push_back (MathUtils::amplitudeToDB (level));

    CompressionCurve analyzer;
    const auto result = analyzer.analyze (dry, wet, sampleRate, inputLevelsDB);

    // Context built from child-reported metadata + measure-request params
    // (ADR-D-6) — same shape as analyzeChildHarmonic. Non-freq types force
    // the sweep excitation on the host side (CommandParser.cpp:662-665), so
    // the context records "sweep" and the default suppresses it from the
    // export.
    Export::Context ctx;
    ctx.pluginName     = pluginName;
    ctx.classId        = classId;
    ctx.latencySamples = latencySamples;
    ctx.sampleRate     = sampleRate;
    ctx.blockSize      = blockSize;
    ctx.excitation     = "sweep";
    ctx.source.type    = "signal";
    ctx.paramSnapshot  = "{}";   // no host plugin instance → empty snapshot

    return Export::compressionCurveToJSON (result, ctx);
}

juce::String analyzeChildGrTimeline (const juce::File& wavPath,
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

    // Analysis chain mirrors CommandParser.cpp runAndAnalyze gr_timeline
    // branch (dynamic source): GainReduction with a 1 ms RMS window
    // (kGRWindowSec — a few-ms attack edge needs the fine resolution),
    // then edge detection + tau estimation on a POSITIVE-dB copy
    // (detectMarkers and TimeConstants expect positive dB = reduction;
    // GainReduction reports the wet/dry ratio, negative for a compressor).
    // The exported timeline keeps the NON-negated GainReduction convention
    // (negative dB = reduction).
    const auto gr = GainReduction::analyze (dry, wet, sampleRate,
                                            latencySamples, 0.001);

    auto grPositive = gr;
    for (auto& p : grPositive.timeline)
        p.grDB = -p.grDB;
    const auto markers = CompressionFamily::detectMarkers (grPositive);
    const auto tau = TimeConstants::estimate (grPositive, markers, sampleRate);

    // Context built from child-reported metadata + measure-request params
    // (ADR-D-6) — same shape as analyzeChildCompression. Non-freq types
    // force the sweep excitation on the host side (CommandParser.cpp:662-665),
    // so the context records "sweep" and the default suppresses it from the
    // export.
    Export::Context ctx;
    ctx.pluginName     = pluginName;
    ctx.classId        = classId;
    ctx.latencySamples = latencySamples;
    ctx.sampleRate     = sampleRate;
    ctx.blockSize      = blockSize;
    ctx.excitation     = "sweep";
    ctx.source.type    = "signal";
    ctx.paramSnapshot  = "{}";   // no host plugin instance → empty snapshot

    return Export::grTimelineToJSON (gr, tau, ctx);
}

} // namespace ChildWavAnalyzer
