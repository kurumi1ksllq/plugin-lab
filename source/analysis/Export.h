#pragma once

#include <JuceHeader.h>
#include "FreqResponse.h"
#include "HarmonicAnalysis.h"
#include "CompressionCurve.h"
#include "CompressionFamily.h"
#include "../scan/ScanEngine.h"
#include "../capture/MeasurementSession.h"

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

        /** Frequency-response excitation (Protocol::Excitation value). Only
         *  non-default values are emitted into the measurement block. */
        juce::String excitation = "sweep";
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

    /** Export a parameter scan (one measurement round per parameter value) to
     *  JSON. type selects which per-round analysis result is embedded in each
     *  family entry (ScanResult does not record the measurement type). */
    juce::String scanToJSON (const ScanEngine::ScanResult& scan,
                             MeasurementSession::Type type,
                             const Context& context);

    /** Export a compression-response family (level × speed grid) to JSON:
     *  one family entry per cell with the static curve, the GR timeline and
     *  the attack/release time constants. */
    juce::String compressionFamilyToJSON (const CompressionFamily::FamilyResult& result,
                                          const Context& context);

    /** Export a gain-reduction timeline (T4.4) to JSON: the GR-over-time
     *  curve (GainReduction convention — negative dB = reduction) plus the
     *  attack/release time constants (valid=false when no controlled edges
     *  were found, e.g. file sources). */
    juce::String grTimelineToJSON (const GainReduction::Result& gr,
                                   const TimeConstants::Result& tau,
                                   const Context& context);

    //==============================================================================
    /** A modelling data package (T5.1): aggregates the parameter-scan family,
     *  the compression-response family and the GR timeline into one
     *  self-contained JSON document that an AI can consume to replicate the
     *  plugin's behaviour. Every field is an optional measurement — the
     *  caller fills only the completed ones, and omitted ones are not
     *  emitted. */
    struct Dataset
    {
        const ScanEngine::ScanResult* scan = nullptr;              // parameter-scan family (optional)
        MeasurementSession::Type scanType = MeasurementSession::Type::frequencyResponse;  // scan's analysis type
        const CompressionFamily::FamilyResult* compressionFamily = nullptr;  // compression-response family (optional)
        const GainReduction::Result* grTimeline = nullptr;         // GR timeline (optional)
        const TimeConstants::Result* grTau = nullptr;              // GR time constants (optional, with grTimeline)
        const FreqResponse::Result* freq = nullptr;                // frequency-response measurement (optional)
        const HarmonicAnalysis::Result* harmonic = nullptr;        // harmonic analysis (optional)
        const CompressionCurve::Result* compression = nullptr;     // compression curve (optional)
        juce::String measurementNote;                              // fitting advice / notes (optional)
    };

    /** Export a modelling data package to a single self-contained JSON
     *  document (type + context + optional note + optional scan /
     *  compression_family / gr_timeline blocks). */
    juce::String datasetToJSON (const Dataset& dataset,
                                const Context& context);

    /** Write a JSON string to a file. Returns true on success. */
    bool writeToFile (const juce::String& json,
                      const juce::File& file);

}  // namespace Export
