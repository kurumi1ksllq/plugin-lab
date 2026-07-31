#pragma once

#include <JuceHeader.h>

/**
 * Wrapper around JUCE's FFT for frequency-domain analysis.
 * Provides forward FFT, magnitude/phase extraction, and windowing.
 */
class FftHelper
{
public:
    /** Create an FFT helper with the given order (FFT size = 2^order). */
    explicit FftHelper (int order = 12);  // 4096-point FFT by default
    ~FftHelper() = default;

    //==============================================================================
    /** Get the FFT size. */
    int getSize() const { return fftSize; }

    /** Get the FFT order. */
    int getOrder() const { return fftOrder; }

    //==============================================================================
    /** Perform forward FFT on a real input buffer.
     *  @param input    Input samples (must be fftSize in length)
     *  @param windowed Optional: apply Hann window (default: true)
     *  @param realOut  Output real part (fftSize/2 + 1 bins)
     *  @param imagOut  Output imag part (fftSize/2 + 1 bins)
     */
    void forwardReal (const float* input,
                      float* realOut,
                      float* imagOut,
                      bool applyWindow = true);

    /** Get magnitude spectrum from real/imag arrays.
     *  @param magnitudes  Output magnitudes (linear, not dB). Length: numBins.
     *  @param realOut     Real part from forwardReal
     *  @param imagOut     Imag part from forwardReal
     *  @param numBins     Number of bins (fftSize/2 + 1)
     */
    static void getMagnitudes (float* magnitudes,
                                const float* realOut,
                                const float* imagOut,
                                int numBins);

    /** Get phase spectrum from real/imag arrays. */
    static void getPhases (float* phases,
                            const float* realOut,
                            const float* imagOut,
                            int numBins);

    /** Apply Hanning window to a buffer. */
    static void applyHannWindow (float* buffer, int size);

private:
    int fftOrder;
    int fftSize;
    std::unique_ptr<juce::dsp::FFT> fft;
    std::vector<float> window;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FftHelper)
};
