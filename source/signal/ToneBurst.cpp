#include "ToneBurst.h"
#include <cmath>

ToneBurst::ToneBurst()
{
    levels = { 0.01, 0.02, 0.05, 0.1, 0.2, 0.3, 0.5, 0.7, 0.9 };
}

void ToneBurst::setFrequency (double freqHz)
{
    frequency = freqHz;
}

void ToneBurst::setBurstDurationMs (double ms)
{
    burstSamples = sampleRate * ms / 1000.0;
}

void ToneBurst::setGapDurationMs (double ms)
{
    gapSamples = sampleRate * ms / 1000.0;
}

void ToneBurst::setLevels (const std::vector<double>& amplitudes)
{
    levels = amplitudes;
}

void ToneBurst::setMasterAmplitude (double scale)
{
    masterAmplitude = scale;
}

void ToneBurst::prepare (double sr, int bs)
{
    SignalGenerator::prepare (sr, bs);
    burstSamples = sr * 50.0 / 1000.0;    // 50ms burst
    gapSamples   = sr * 150.0 / 1000.0;   // 150ms gap
}

int64_t ToneBurst::getTotalLength() const
{
    if (levels.empty()) return 0;
    return static_cast<int64_t> (levels.size() * (burstSamples + gapSamples));
}

void ToneBurst::reset()
{
    currentSample = 0.0;
    currentBurstIndex = 0;
    positionInBurst = 0;
}

void ToneBurst::generate (juce::AudioBuffer<float>& buffer,
                          int startSample,
                          int numSamples)
{
    const auto numChannels = buffer.getNumChannels();
    const auto burstLen   = static_cast<int64_t> (burstSamples);
    const auto gapLen     = static_cast<int64_t> (gapSamples);
    const auto totalBurstLen = burstLen + gapLen;

    for (int s = 0; s < numSamples; ++s)
    {
        float sample = 0.0f;

        if (currentBurstIndex < (int) levels.size())
        {
            if (positionInBurst < burstLen)
            {
                // Active burst — generate sine tone at this level
                double t = currentSample / sampleRate;
                double phase = 2.0 * juce::MathConstants<double>::pi * frequency * t;
                sample = static_cast<float> (levels[currentBurstIndex] * masterAmplitude * std::sin (phase));
            }
            // else: in gap — sample stays 0

            positionInBurst++;

            if (positionInBurst >= totalBurstLen)
            {
                positionInBurst = 0;
                currentBurstIndex++;
            }
        }

        for (int ch = 0; ch < numChannels; ++ch)
            buffer.getWritePointer (ch)[startSample + s] = sample;

        currentSample += 1.0;
    }
}
