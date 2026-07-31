#pragma once

#include <JuceHeader.h>

/**
 * Frequency Response Analyzer.
 *
 * Takes recorded dry (input) and wet (output) audio from a sine sweep
 * and computes magnitude (dB) and phase (degrees) vs frequency.
 */
class FreqResponse
{
public:
    FreqResponse() = default;
    ~FreqResponse() = default;

    //==============================================================================
    /** Data point in the frequency response. */
    struct Point
    {
        double frequency = 0.0;
        double magnitudeDB = 0.0;
        double phaseDeg = 0.0;
    };

    /** Result containing the full frequency response curve. */
    struct Result
    {
        std::vector<Point> raw;
        std::vector<Point> smoothed_1_12;  // 1/12 octave smoothed
        std::vector<Point> smoothed_1_3;   // 1/3 octave smoothed
        double sampleRate = 0.0;
    };

    //==============================================================================
    /** Analyze frequency response from dry/wet sweep recording.
     *  @param dry  Dry (input/reference) audio buffer
     *  @param wet  Wet (output/processed) audio buffer
     *  @param sr   Sample rate of the recording
     */
    Result analyze (const juce::AudioBuffer<float>& dry,
                    const juce::AudioBuffer<float>& wet,
                    double sr);

private:
    /** Process a single channel pair. */
    void processChannel (const float* dryData,
                         const float* wetData,
                         int numSamples,
                         double sampleRate,
                         int fftOrder,
                         std::vector<Point>& points);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FreqResponse)
};
