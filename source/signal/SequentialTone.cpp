#include "SequentialTone.h"
#include <cmath>

SequentialTone::SequentialTone()
{
    // Default: the octave fundamental set used by the harmonic measurement
    // (mirrors MeasurementSession.cpp Type::harmonicAnalysis).
    frequencies = { 100.0, 200.0, 400.0, 800.0, 1600.0, 3200.0, 6400.0, 12800.0 };
}

void SequentialTone::setFrequencies (const std::vector<double>& freqs)
{
    frequencies = freqs;
}

void SequentialTone::setSegmentDuration (double seconds)
{
    segmentDurationSec = seconds;
}

void SequentialTone::setAmplitude (double amp)
{
    amplitude = amp;
}

void SequentialTone::setGapSeconds (double seconds)
{
    gapSec = seconds;
}

void SequentialTone::prepare (double sr, int bs)
{
    SignalGenerator::prepare (sr, bs);

    toneLenSamples = static_cast<int64_t> (sampleRate * segmentDurationSec);
    segmentLenSamples = static_cast<int64_t> (sampleRate * (segmentDurationSec + gapSec));
}

int64_t SequentialTone::getTotalLength() const
{
    // Finite by construction (contract: never return -1 — -1 would trigger
    // SweepRunner's silent 10 s fallback).
    if (frequencies.empty() || segmentLenSamples <= 0)
        return 0;
    return static_cast<int64_t> (frequencies.size()) * segmentLenSamples;
}

void SequentialTone::reset()
{
    currentSample = 0.0;
}

void SequentialTone::generate (juce::AudioBuffer<float>& buffer,
                               int startSample,
                               int numSamples)
{
    const auto totalLen = getTotalLength();
    const auto numChannels = buffer.getNumChannels();

    for (int s = 0; s < numSamples; ++s)
    {
        const int64_t samplePos = static_cast<int64_t> (currentSample) + s;

        if (frequencies.empty() || segmentLenSamples <= 0 || samplePos >= totalLen)
        {
            for (int ch = 0; ch < numChannels; ++ch)
                buffer.getWritePointer (ch)[startSample + s] = 0.0f;
            continue;
        }

        const int64_t segmentIndex = samplePos / segmentLenSamples;
        const int64_t posInSegment = samplePos % segmentLenSamples;

        if (posInSegment >= toneLenSamples)   // gap: silent
        {
            for (int ch = 0; ch < numChannels; ++ch)
                buffer.getWritePointer (ch)[startSample + s] = 0.0f;
            continue;
        }

        // Phase restarts at 0 for every segment (deterministic per segment).
        const double t = static_cast<double> (posInSegment) / sampleRate;
        const double freq = frequencies[static_cast<size_t> (segmentIndex)];
        const float sample = static_cast<float> (
            amplitude * std::sin (2.0 * juce::MathConstants<double>::pi * freq * t));

        for (int ch = 0; ch < numChannels; ++ch)
            buffer.getWritePointer (ch)[startSample + s] = sample;
    }

    currentSample += numSamples;
}
