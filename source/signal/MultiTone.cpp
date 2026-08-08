#include "MultiTone.h"
#include <algorithm>
#include <cmath>

MultiTone::MultiTone()
{
    // Default: a set of logarithmically spaced frequencies
    frequencies = { 100.0, 200.0, 400.0, 800.0, 1600.0, 3200.0, 6400.0, 12800.0 };
}

void MultiTone::setFrequencies (const std::vector<double>& freqs)
{
    frequencies = freqs;
}

void MultiTone::setDuration (double seconds)
{
    durationSec = seconds;
}

void MultiTone::setAmplitude (double amp)
{
    amplitude = amp;
}

void MultiTone::prepare (double sr, int bs)
{
    SignalGenerator::prepare (sr, bs);

    phases.resize (frequencies.size());
    if (phaseSeed != 0)
    {
        uint32_t state = phaseSeed;
        const auto nextRand = [&state]()
        {
            state ^= state << 13;
            state ^= state >> 17;
            state ^= state << 5;
            return state;
        };
        for (auto& ph : phases)
            ph = 2.0 * juce::MathConstants<double>::pi
                 * (static_cast<double> (nextRand()) / 4294967296.0);
    }
    else
    {
        std::fill (phases.begin(), phases.end(), 0.0);
    }
}

int64_t MultiTone::getTotalLength() const
{
    return static_cast<int64_t> (sampleRate * durationSec);
}

void MultiTone::reset()
{
    currentSample = 0.0;
}

void MultiTone::generate (juce::AudioBuffer<float>& buffer,
                          int startSample,
                          int numSamples)
{
    const auto totalLen = getTotalLength();
    const auto numChannels = buffer.getNumChannels();
    const auto numFreqs = static_cast<double> (frequencies.size());
    const double invNumFreqs = 1.0 / numFreqs;  // spread energy across tones

    for (int s = 0; s < numSamples; ++s)
    {
        auto samplePos = currentSample + s;

        if (samplePos >= totalLen)
        {
            for (int ch = 0; ch < numChannels; ++ch)
                buffer.getWritePointer (ch)[startSample + s] = 0.0f;
            continue;
        }

        double t = samplePos / sampleRate;
        double value = 0.0;

        for (size_t i = 0; i < frequencies.size(); ++i)
        {
            const double freq = frequencies[i];
            double phase = 2.0 * juce::MathConstants<double>::pi * freq * t + phases[i];
            value += std::sin (phase);
        }

        value *= amplitude * invNumFreqs;
        float sample = static_cast<float> (value);

        for (int ch = 0; ch < numChannels; ++ch)
            buffer.getWritePointer (ch)[startSample + s] = sample;
    }

    currentSample += numSamples;
}
