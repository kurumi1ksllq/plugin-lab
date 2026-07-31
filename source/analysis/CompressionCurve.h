#pragma once

#include <JuceHeader.h>

/**
 * Compression Curve Analyzer.
 *
 * Takes recorded dry/wet audio from a tone burst sweep at multiple
 * input levels and computes the gain reduction curve
 * (input dB vs output dB).
 */
class CompressionCurve
{
public:
    CompressionCurve() = default;
    ~CompressionCurve() = default;

    //==============================================================================
    /** A single point on the compression curve. */
    struct Point
    {
        double inputDB = 0.0;
        double outputDB = 0.0;
        double gainReductionDB = 0.0;
    };

    /** Fitted compression parameters. */
    struct FittedParams
    {
        double ratio = 1.0;
        double thresholdDB = 0.0;
        double kneeDB = 0.0;
    };

    /** Result containing the compression curve and fitted parameters. */
    struct Result
    {
        std::vector<Point> curve;
        FittedParams fitted;
    };

    //==============================================================================
    /** Analyze compression curve from dry/wet tone burst recording.
     *  @param dry  Dry (input/reference) audio buffer
     *  @param wet  Wet (output/processed) audio buffer
     *  @param sr   Sample rate
     *  @param levels  Expected input levels in dB (-60 to 0)
     */
    Result analyze (const juce::AudioBuffer<float>& dry,
                    const juce::AudioBuffer<float>& wet,
                    double sr,
                    const std::vector<double>& inputLevelsDB);

private:
    /** Measure RMS level of a section of audio. */
    double measureRMS (const float* data, int startSample, int numSamples);

    /** Fit compression parameters using least-squares. */
    FittedParams fitParams (const std::vector<Point>& curve);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CompressionCurve)
};
