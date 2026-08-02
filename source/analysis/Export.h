#pragma once

#include <JuceHeader.h>
#include "FreqResponse.h"
#include "HarmonicAnalysis.h"
#include "CompressionCurve.h"

/**
 * Exports measurement results to structured JSON files
 * that can be consumed by Sisyphus for DSP implementation.
 */
namespace Export
{

    //==============================================================================
    /** Measurement context attached to an export so that the consumer can
     *  reproduce the exact measurement setup. */
    struct Context
    {
        /** Input-source metadata (Protocol::Source values). */
        struct SourceInfo
        {
            juce::String type = "signal";
            juce::String filePath;
            double sourceSampleRate = 0.0;
            double resampleRatio = 0.0;
            double durationSec = 0.0;
            juce::String noiseType;
            uint32_t seed = 0;
        };

        juce::String pluginName;
        juce::String classId;
        int latencySamples = 0;
        double sampleRate = 0.0;
        int blockSize = 0;
        juce::String paramSnapshot;  // JSON object string produced by MeasurementSession
        SourceInfo source;
    };

    //==============================================================================
    /** Export frequency response to JSON string. */
    juce::String freqResponseToJSON (const FreqResponse::Result& result,
                                      const juce::String& pluginName = "Unknown");

    /** Export frequency response with full measurement context to JSON string. */
    juce::String freqResponseToJSON (const FreqResponse::Result& result,
                                      const Context& context);

    /** Export harmonic analysis to JSON string. */
    juce::String harmonicAnalysisToJSON (const HarmonicAnalysis::Result& result,
                                          const juce::String& pluginName = "Unknown");

    /** Export harmonic analysis with full measurement context to JSON string. */
    juce::String harmonicAnalysisToJSON (const HarmonicAnalysis::Result& result,
                                          const Context& context);

    /** Export compression curve to JSON string. */
    juce::String compressionCurveToJSON (const CompressionCurve::Result& result,
                                          const juce::String& pluginName = "Unknown");

    /** Export compression curve with full measurement context to JSON string. */
    juce::String compressionCurveToJSON (const CompressionCurve::Result& result,
                                          const Context& context);

    /** Export a raw capture (non-signal sources): record metadata only, no
     *  analysis (analysis of raw captures is phase 4). */
    juce::String rawCaptureToJSON (int64_t samples, double rate, int blockSize,
                                   const Context& context);

    /** Write a JSON string to a file. Returns true on success. */
    bool writeToFile (const juce::String& json,
                      const juce::File& file);

}  // namespace Export
