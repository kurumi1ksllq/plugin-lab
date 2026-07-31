#include "FftHelper.h"
#include <cmath>

FftHelper::FftHelper (int order)
    : fftOrder (order),
      fftSize (1 << order),
      fft (std::make_unique<juce::dsp::FFT> (order))
{
    // Pre-compute Hann window
    window.resize (fftSize);
    for (int i = 0; i < fftSize; ++i)
        window[i] = 0.5f * (1.0f - std::cos (2.0f * juce::MathConstants<float>::pi * i / (fftSize - 1)));
}

void FftHelper::forwardReal (const float* input,
                              float* realOut,
                              float* imagOut,
                              bool applyWindow)
{
    // Prepare buffer (real data, will be overwritten with interleaved complex)
    std::vector<float> fftBuf (fftSize * 2, 0.0f);

    for (int i = 0; i < fftSize; ++i)
        fftBuf[i] = applyWindow ? input[i] * window[i] : input[i];

    // Use real-only forward transform (more efficient for real input)
    fft->performRealOnlyForwardTransform (fftBuf.data());

    // Extract real and imag parts (first fftSize/2 + 1 bins)
    // After performRealOnlyForwardTransform, the data is packed as:
    // [0] = DC real, [1] = DC imag (0), [2] = bin1 real, [3] = bin1 imag, ...
    int numBins = fftSize / 2 + 1;
    for (int i = 0; i < numBins; ++i)
    {
        realOut[i] = fftBuf[i * 2];
        imagOut[i] = fftBuf[i * 2 + 1];
    }
}

void FftHelper::getMagnitudes (float* magnitudes,
                                const float* realOut,
                                const float* imagOut,
                                int numBins)
{
    for (int i = 0; i < numBins; ++i)
        magnitudes[i] = std::sqrt (realOut[i] * realOut[i] + imagOut[i] * imagOut[i]);
}

void FftHelper::getPhases (float* phases,
                            const float* realOut,
                            const float* imagOut,
                            int numBins)
{
    for (int i = 0; i < numBins; ++i)
        phases[i] = std::atan2 (imagOut[i], realOut[i]);
}

void FftHelper::applyHannWindow (float* buffer, int size)
{
    for (int i = 0; i < size; ++i)
        buffer[i] *= 0.5f * (1.0f - std::cos (2.0f * juce::MathConstants<float>::pi * i / (size - 1)));
}
