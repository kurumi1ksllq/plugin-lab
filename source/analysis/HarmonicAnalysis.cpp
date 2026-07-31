#include "HarmonicAnalysis.h"
#include "../utils/FftHelper.h"
#include "../utils/MathUtils.h"
#include <cmath>

HarmonicAnalysis::Result HarmonicAnalysis::analyze (
    const juce::AudioBuffer<float>& wet,
    double sr,
    const std::vector<double>& fundamentalFreqs)
{
    Result result;
    result.sampleRate = sr;

    const float* wetData = wet.getReadPointer (0);
    const int numSamples = wet.getNumSamples();
    const int fftOrder = 16;  // 65536-point FFT for good low-freq resolution

    for (double freq : fundamentalFreqs)
    {
        auto toneResult = analyzeTone (wetData, numSamples, sr, freq, fftOrder);
        if (! toneResult.harmonics.empty())
            result.tones.push_back (toneResult);
    }

    return result;
}

HarmonicAnalysis::ToneResult HarmonicAnalysis::analyzeTone (
    const float* audio,
    int numSamples,
    double sampleRate,
    double fundamentalFreq,
    int fftOrder,
    int numHarmonics)
{
    ToneResult result;
    result.fundamentalFreq = fundamentalFreq;

    const int fftSize = 1 << fftOrder;
    const int numBins = fftSize / 2 + 1;
    const double freqStep = sampleRate / fftSize;

    // Take a section of audio centered on the tone (use middle portion)
    int analysisLen = juce::jmin (fftSize, numSamples);
    int startPos = (numSamples - analysisLen) / 2;

    FftHelper fft (fftOrder);
    std::vector<float> window (fftSize, 0.0f);

    // Copy audio into window
    for (int i = 0; i < analysisLen; ++i)
        window[i] = audio[startPos + i];

    // Apply window
    FftHelper::applyHannWindow (window.data(), fftSize);

    // FFT
    std::vector<float> real (numBins), imag (numBins);
    fft.forwardReal (window.data(), real.data(), imag.data(), false);

    std::vector<float> mag (numBins);
    FftHelper::getMagnitudes (mag.data(), real.data(), imag.data(), numBins);

    // Find fundamental peak
    int fundamentalBin = MathUtils::findPeakBin (mag.data(), numBins,
                                                   fundamentalFreq, sampleRate, fftSize);
    double actualFreq = fundamentalBin * freqStep;
    result.fundamentalFreq = actualFreq;
    result.fundamentalDB = MathUtils::amplitudeToDB (mag[fundamentalBin]);

    // Measure harmonics
    double fundamentalAmp = mag[fundamentalBin];
    double sumHarmonicAmpSq = 0.0;

    for (int h = 2; h <= numHarmonics; ++h)
    {
        Harmonic harm;
        harm.order = h;
        harm.frequency = actualFreq * h;

        int harmonicBin = static_cast<int> (harm.frequency / freqStep + 0.5);
        if (harmonicBin >= numBins)
            break;

        // Search ±2 bins for peak
        int peakBin = MathUtils::findPeakBin (mag.data(), numBins,
                                               harm.frequency, sampleRate, fftSize);
        harm.frequency = peakBin * freqStep;
        harm.magnitudeDB = MathUtils::amplitudeToDB (mag[peakBin]);

        if (fundamentalAmp > 0.0f)
            harm.percent = 100.0 * mag[peakBin] / fundamentalAmp;
        else
            harm.percent = 0.0;

        sumHarmonicAmpSq += static_cast<double> (mag[peakBin]) * mag[peakBin];
        result.harmonics.push_back (harm);
    }

    // Compute THD
    double fundamentalAmpSq = static_cast<double> (fundamentalAmp) * fundamentalAmp;
    result.thdPercent = (fundamentalAmpSq > 0.0)
        ? 100.0 * std::sqrt (sumHarmonicAmpSq / fundamentalAmpSq)
        : 0.0;

    return result;
}
