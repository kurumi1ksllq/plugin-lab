#include "FreqResponse.h"
#include "../utils/FftHelper.h"
#include "../utils/MathUtils.h"
#include <cmath>

FreqResponse::Result FreqResponse::analyze (
    const juce::AudioBuffer<float>& dry,
    const juce::AudioBuffer<float>& wet,
    double sr)
{
    Result result;
    result.sampleRate = sr;

    const int numSamples = juce::jmin (dry.getNumSamples(), wet.getNumSamples());
    const int fftOrder = 14;  // 16384-point FFT for good frequency resolution
    const int fftSize = 1 << fftOrder;

    std::vector<Point> points;

    // Use channel 0 for analysis (both dry and wet are mono-compatible)
    const float* dryData = dry.getReadPointer (0);
    const float* wetData = wet.getReadPointer (0);

    processChannel (dryData, wetData, numSamples, sr, fftOrder, points);

    result.raw = points;

    // Apply smoothing
    if (! points.empty())
    {
        // Extract magnitudes for smoothing
        std::vector<float> mags (points.size());
        std::vector<float> freqs (points.size());
        for (size_t i = 0; i < points.size(); ++i)
        {
            mags[i] = std::pow (10.0f, static_cast<float> (points[i].magnitudeDB / 20.0));
            freqs[i] = static_cast<float> (points[i].frequency);
        }

        // 1/12 octave smoothing
        auto mags_12 = mags;
        double freqStep = sr / fftSize;
        MathUtils::smoothOctave (mags_12.data(), freqStep, (int) mags_12.size(), 12);
        for (size_t i = 0; i < points.size(); ++i)
        {
            Point p;
            p.frequency = points[i].frequency;
            p.magnitudeDB = MathUtils::amplitudeToDB (mags_12[i]);
            p.phaseDeg = points[i].phaseDeg;
            result.smoothed_1_12.push_back (p);
        }

        // 1/3 octave smoothing
        auto mags_3 = mags;
        MathUtils::smoothOctave (mags_3.data(), freqStep, (int) mags_3.size(), 3);
        for (size_t i = 0; i < points.size(); ++i)
        {
            Point p;
            p.frequency = points[i].frequency;
            p.magnitudeDB = MathUtils::amplitudeToDB (mags_3[i]);
            p.phaseDeg = points[i].phaseDeg;
            result.smoothed_1_3.push_back (p);
        }
    }

    return result;
}

void FreqResponse::processChannel (
    const float* dryData,
    const float* wetData,
    int numSamples,
    double sampleRate,
    int fftOrder,
    std::vector<Point>& points)
{
    const int fftSize = 1 << fftOrder;
    const int hopSize = fftSize / 4;
    const int numBins = fftSize / 2 + 1;
    const double freqStep = sampleRate / fftSize;

    FftHelper fft (fftOrder);
    std::vector<float> dryWindow (fftSize);
    std::vector<float> wetWindow (fftSize);
    std::vector<float> dryReal (numBins), dryImag (numBins);
    std::vector<float> wetReal (numBins), wetImag (numBins);
    std::vector<float> dryMag (numBins), wetMag (numBins);
    std::vector<float> dryPhase (numBins), wetPhase (numBins);

    for (int pos = 0; pos + fftSize <= numSamples; pos += hopSize)
    {
        for (int i = 0; i < fftSize; ++i)
        {
            dryWindow[i] = dryData[pos + i];
            wetWindow[i] = wetData[pos + i];
        }

        // Apply Hann window
        FftHelper::applyHannWindow (dryWindow.data(), fftSize);
        FftHelper::applyHannWindow (wetWindow.data(), fftSize);

        // Forward FFT
        fft.forwardReal (dryWindow.data(), dryReal.data(), dryImag.data(), false);
        fft.forwardReal (wetWindow.data(), wetReal.data(), wetImag.data(), false);

        FftHelper::getMagnitudes (dryMag.data(), dryReal.data(), dryImag.data(), numBins);
        FftHelper::getMagnitudes (wetMag.data(), wetReal.data(), wetImag.data(), numBins);
        FftHelper::getPhases (dryPhase.data(), dryReal.data(), dryImag.data(), numBins);
        FftHelper::getPhases (wetPhase.data(), wetReal.data(), wetImag.data(), numBins);

        for (int bin = 1; bin < numBins - 1; ++bin)
        {
            double freq = bin * freqStep;
            if (freq < 20.0 || freq > 20000.0)
                continue;

            // Only include bins with significant energy in the dry signal
            if (dryMag[bin] < 0.001f)
                continue;

            Point p;
            p.frequency = freq;
            p.magnitudeDB = MathUtils::amplitudeToDB (wetMag[bin] / dryMag[bin]);
            
            // Phase difference, unwrapped
            double phaseDiff = wetPhase[bin] - dryPhase[bin];
            // Normalize to -180..180
            while (phaseDiff > juce::MathConstants<double>::pi) phaseDiff -= 2.0 * juce::MathConstants<double>::pi;
            while (phaseDiff < -juce::MathConstants<double>::pi) phaseDiff += 2.0 * juce::MathConstants<double>::pi;
            p.phaseDeg = phaseDiff * 180.0 / juce::MathConstants<double>::pi;

            points.push_back (p);
        }
    }

    // Sort by frequency and remove duplicates (average overlapping bins)
    std::sort (points.begin(), points.end(),
               [](const Point& a, const Point& b) { return a.frequency < b.frequency; });

    // Average overlapping frequency bins
    std::vector<Point> merged;
    for (size_t i = 0; i < points.size();)
    {
        double freq = points[i].frequency;
        double sumMag = 0.0, sumPhase = 0.0;
        int count = 0;

        while (i < points.size() && std::abs (points[i].frequency - freq) < freqStep * 0.5)
        {
            double amp = std::pow (10.0, points[i].magnitudeDB / 20.0);
            sumMag += amp;
            sumPhase += points[i].phaseDeg;
            count++;
            i++;
        }

        Point p;
        p.frequency = freq;
        p.magnitudeDB = 20.0 * std::log10 (sumMag / count);
        p.phaseDeg = sumPhase / count;
        merged.push_back (p);
    }

    points = merged;
}
