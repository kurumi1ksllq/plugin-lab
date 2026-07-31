#pragma once

#include <JuceHeader.h>

/**
 * Harmonic Analyzer.
 *
 * Takes recorded wet audio from a multi-tone test and computes
 * the harmonic structure (THD and individual harmonic magnitudes).
 */
class HarmonicAnalysis
{
public:
    HarmonicAnalysis() = default;
    ~HarmonicAnalysis() = default;

    //==============================================================================
    /** A single harmonic. */
    struct Harmonic
    {
        int order = 0;
        double frequency = 0.0;
        double magnitudeDB = 0.0;
        double percent = 0.0;  // % of fundamental
    };

    /** Result for one test tone. */
    struct ToneResult
    {
        double fundamentalFreq = 0.0;
        double fundamentalDB = 0.0;
        double thdPercent = 0.0;
        std::vector<Harmonic> harmonics;
    };

    /** Result containing analysis for all test frequencies. */
    struct Result
    {
        std::vector<ToneResult> tones;
        double sampleRate = 0.0;
    };

    //==============================================================================
    /** Analyze harmonics from wet (processed) audio.
     *  @param wet   Wet (output/processed) audio buffer
     *  @param sr    Sample rate of the recording
     *  @param fundamentalFreqs  List of fundamental frequencies to analyze
     */
    Result analyze (const juce::AudioBuffer<float>& wet,
                    double sr,
                    const std::vector<double>& fundamentalFreqs);

private:
    /** Analyze a specific fundamental frequency. */
    ToneResult analyzeTone (const float* audio,
                            int numSamples,
                            double sampleRate,
                            double fundamentalFreq,
                            int fftOrder,
                            int numHarmonics = 10);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (HarmonicAnalysis)
};
