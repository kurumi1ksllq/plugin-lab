#pragma once

#include <JuceHeader.h>

namespace MathUtils
{

    //==============================================================================
    /** Convert amplitude ratio to dB. */
    inline double amplitudeToDB (double amplitude, double minDb = -120.0)
    {
        if (amplitude <= 0.0) return minDb;
        return juce::jmax (minDb, 20.0 * std::log10 (amplitude));
    }

    /** Convert dB to amplitude ratio. */
    inline double dbToAmplitude (double dB)
    {
        return std::pow (10.0, dB / 20.0);
    }

    //==============================================================================
    /** Smooth a magnitude array using fractional octave smoothing.
     *  @param magnitudes  Input magnitudes (linear, not dB). Will be modified in-place.
     *  @param freqStep    Frequency step per bin (Fs / FFT_size).
     *  @param numBins     Number of frequency bins.
     *  @param fraction    Octave fraction (e.g. 3 = 1/3 octave, 12 = 1/12 octave).
     */
    void smoothOctave (float* magnitudes,
                       double freqStep,
                       int numBins,
                       int fraction = 3);

    //==============================================================================
    /** Build a list of frequencies for each FFT bin. */
    std::vector<double> getBinFrequencies (double sampleRate, int fftSize);

    /** Find the peak bin index near a given frequency. */
    int findPeakBin (const float* magnitudes,
                     int numBins,
                     double targetFreq,
                     double sampleRate,
                     int fftSize);

    /** Simple linear interpolation. */
    float interpolate (float x, float x0, float y0, float x1, float y1);

}  // namespace MathUtils
