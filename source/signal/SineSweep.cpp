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
    return static_cast<int64_t> (sampleRate * durationSec);
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
    const auto numChannels = buffer.getNumChannels();

    for (int s = 0; s < numSamples; ++s)
    {
        auto samplePos = currentSample + s;

        // Clamp to sweep length
        if (samplePos >= totalLen)
        {
            // Fill remaining with silence
            for (int ch = 0; ch < numChannels; ++ch)
                buffer.getWritePointer (ch)[startSample + s] = 0.0f;
            continue;
        }

        double t = samplePos / sampleRate;  // time in seconds
        double phase = 2.0 * juce::MathConstants<double>::pi
                       * startFreq * sweepRate
                       * (std::exp (t / sweepRate) - 1.0);
        float sample = static_cast<float> (amplitude * std::sin (phase));

        for (int ch = 0; ch < numChannels; ++ch)
            buffer.getWritePointer (ch)[startSample + s] = sample;
    }

    currentSample += numSamples;
}
