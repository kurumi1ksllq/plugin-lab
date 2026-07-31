#include "MathUtils.h"
#include <cmath>

namespace MathUtils
{

void smoothOctave (float* magnitudes,
                    double freqStep,
                    int numBins,
                    int fraction)
{
    if (numBins < 2 || fraction < 1)
        return;

    std::vector<float> output (numBins);

    double centreFreq = 0.0;

    for (int i = 0; i < numBins; ++i)
    {
        centreFreq = i * freqStep;
        if (centreFreq <= 0.0)
        {
            output[i] = magnitudes[i];
            continue;
        }

        // Calculate the smoothing window bounds
        double lowerRatio = std::pow (2.0, -1.0 / (2.0 * fraction));
        double upperRatio = std::pow (2.0,  1.0 / (2.0 * fraction));

        double lowerFreq = centreFreq * lowerRatio;
        double upperFreq = centreFreq * upperRatio;

        int lowerBin = juce::jmax (0, static_cast<int> (lowerFreq / freqStep));
        int upperBin = juce::jmin (numBins - 1, static_cast<int> (upperFreq / freqStep));

        // Average magnitudes in the window (power average, not arithmetic)
        double sumSq = 0.0;
        int count = 0;

        for (int j = lowerBin; j <= upperBin; ++j)
        {
            sumSq += static_cast<double> (magnitudes[j]) * magnitudes[j];
            count++;
        }

        output[i] = (count > 0) ? static_cast<float> (std::sqrt (sumSq / count)) : 0.0f;
    }

    std::copy (output.begin(), output.end(), magnitudes);
}

std::vector<double> getBinFrequencies (double sampleRate, int fftSize)
{
    int numBins = fftSize / 2 + 1;
    double freqStep = sampleRate / fftSize;
    std::vector<double> freqs (numBins);

    for (int i = 0; i < numBins; ++i)
        freqs[i] = i * freqStep;

    return freqs;
}

int findPeakBin (const float* magnitudes,
                  int numBins,
                  double targetFreq,
                  double sampleRate,
                  int fftSize)
{
    double freqStep = sampleRate / fftSize;
    int targetBin = static_cast<int> (targetFreq / freqStep + 0.5);

    if (targetBin >= numBins)
        return numBins - 1;

    // Search ±5 bins around target for actual peak
    int searchStart = juce::jmax (0, targetBin - 5);
    int searchEnd   = juce::jmin (numBins - 1, targetBin + 5);

    int peakBin = targetBin;
    float peakVal = magnitudes[targetBin];

    for (int i = searchStart; i <= searchEnd; ++i)
    {
        if (magnitudes[i] > peakVal)
        {
            peakVal = magnitudes[i];
            peakBin = i;
        }
    }

    return peakBin;
}

float interpolate (float x, float x0, float y0, float x1, float y1)
{
    if (std::abs (x1 - x0) < 1e-15f)
        return y0;

    return y0 + (y1 - y0) * ((x - x0) / (x1 - x0));
}

}  // namespace MathUtils
