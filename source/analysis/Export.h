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
    /** Export frequency response to JSON string. */
    juce::String freqResponseToJSON (const FreqResponse::Result& result,
                                      const juce::String& pluginName = "Unknown");

    /** Export harmonic analysis to JSON string. */
    juce::String harmonicAnalysisToJSON (const HarmonicAnalysis::Result& result,
                                          const juce::String& pluginName = "Unknown");

    /** Export compression curve to JSON string. */
    juce::String compressionCurveToJSON (const CompressionCurve::Result& result,
                                          const juce::String& pluginName = "Unknown");

    /** Write a JSON string to a file. Returns true on success. */
    bool writeToFile (const juce::String& json,
                      const juce::File& file);

}  // namespace Export
