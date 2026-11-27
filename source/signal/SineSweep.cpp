#include "SineSweep.h"
#include <cmath>

SineSweep::SineSweep() {}

void SineSweep::setFrequencyRange (double startFreqHz, double endFreqHz)
{
    startFreq = startFreqHz;
    endFreq = endFreqHz;
}

void SineSweep::setDuration (double seconds)
{
    durationSec = seconds;
}

void SineSweep::setAmplitude (double amp)
{
    amplitude = amp;
}

void SineSweep::prepare (double sr, int bs)
{
    SignalGenerator::prepare (sr, bs);
    sweepRate = durationSec / std::log (endFreq / startFreq);
}

int64_t SineSweep::getTotalLength() const
{
    // Always finite: the frozen SweepRunner falls back to 10 s of silence on a
    // non-positive length, so a degenerate duration must still yield >= 1
    // sample (issue #104 acceptance line: never return -1).
    return juce::jmax<int64_t> (1, static_cast<int64_t> (sampleRate * durationSec));
}

void SineSweep::reset()
{
    currentSample = 0.0;
}

void SineSweep::generate (juce::AudioBuffer<float>& buffer,
                          int startSample,
                          int numSamples)
{
    const auto totalLen = getTotalLength();

    for (int s = 0; s < numSamples; ++s)
    {
        auto samplePos = currentSample + s;

        // Clamp to sweep length
        if (samplePos >= totalLen)
        {
            // Fill remaining with silence
            writeSample (buffer, startSample + s, 0.0f);
            continue;
        }

        double t = samplePos / sampleRate;  // time in seconds
        double phase = 2.0 * juce::MathConstants<double>::pi
                       * startFreq * sweepRate
                       * (std::exp (t / sweepRate) - 1.0);
        float sample = static_cast<float> (amplitude * std::sin (phase));

        // Channel-polarity mode (mid/side) is applied here - issue #104.
        writeSample (buffer, startSample + s, sample);
    }

    currentSample += numSamples;
}
