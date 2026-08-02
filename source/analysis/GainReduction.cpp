#include "GainReduction.h"
#include <cmath>

// Dry RMS below this level is treated as silence (no GR information).
static constexpr double dryRMSSilenceThreshold = 1e-6;

GainReduction::Result GainReduction::analyze (
    const juce::AudioBuffer<float>& dry,
    const juce::AudioBuffer<float>& wet,
    double sr,
    int latencySamples,
    double windowSec)
{
    Result result;
    result.sampleRate = sr;

    const int numSamples = juce::jmin (dry.getNumSamples(), wet.getNumSamples());
    const int windowSize = juce::jmax (1, static_cast<int> (std::lround (sr * windowSec)));
    const int hopSize = windowSize;  // non-overlapping windows

    // Use channel 0 for analysis (both dry and wet are mono-compatible).
    const float* dryData = dry.getReadPointer (0);
    const float* wetData = wet.getReadPointer (0);

    // Stop when the wet window (shifted by the plugin latency) would run past
    // the recorded samples — same as dry index i running out of signal.
    const int lastWindowStart = numSamples - latencySamples - windowSize;

    for (int i = 0; i <= lastWindowStart; i += hopSize)
    {
        // Dry RMS over [i, i+W)
        double drySumSq = 0.0;
        for (int j = i; j < i + windowSize; ++j)
        {
            const double s = static_cast<double> (dryData[j]);
            drySumSq += s * s;
        }
        const double dryRMS = std::sqrt (drySumSq / static_cast<double> (windowSize));

        // Wet RMS over [i+latency, i+latency+W)
        double wetSumSq = 0.0;
        for (int j = i + latencySamples; j < i + latencySamples + windowSize; ++j)
        {
            const double s = static_cast<double> (wetData[j]);
            wetSumSq += s * s;
        }
        const double wetRMS = std::sqrt (wetSumSq / static_cast<double> (windowSize));

        Point p;
        p.timeSec = static_cast<double> (i) / sr;

        if (dryRMS < dryRMSSilenceThreshold)
        {
            // Silence carries no GR information; report 0 instead of
            // dividing ~0/~0 (NaN) or log10(0) (-inf).
            p.grDB = 0.0;
        }
        else
        {
            // Clamp the ratio to avoid -inf when the wet side is fully silent.
            const double ratio = std::max (wetRMS / dryRMS, 1e-15);
            p.grDB = 20.0 * std::log10 (ratio);
        }

        result.timeline.push_back (p);
    }

    result.numPoints = static_cast<int> (result.timeline.size());
    return result;
}
