#include "Impulse.h"

Impulse::Impulse() {}

void Impulse::setAmplitude (double amp)
{
    amplitude = amp;
}

void Impulse::useMLS (bool use)
{
    useMls = use;
}

void Impulse::setMLSLength (int length)
{
    // MLS length must be 2^n - 1; find the nearest valid length
    mlsLength = length;
    // Force to nearest valid MLS length (2^n - 1)
    int n = 1;
    while ((1 << n) - 1 < length) ++n;
    mlsLength = (1 << n) - 1;
}

void Impulse::prepare (double sr, int bs)
{
    SignalGenerator::prepare (sr, bs);

    if (useMls)
    {
        generateMLS();
        mlsIndex = 0;
    }

    impulseDone = false;
}

int64_t Impulse::getTotalLength() const
{
    if (useMls)
        return mlsLength;
    return 1;  // single sample
}

void Impulse::reset()
{
    currentSample = 0.0;
    mlsIndex = 0;
    impulseDone = false;
}

void Impulse::generateMLS()
{
    // Generate a Maximum Length Sequence using a linear feedback shift register
    mlsSequence.resize (mlsLength);

    // Determine the LFSR taps based on the length.
    // For length 2^n - 1, we need the primitive polynomial taps.
    // A LEFT-shifting LFSR is used (state = (state << 1) | feedback, low bit
    // output); every tap set below was verified to produce the full 2^n - 1
    // cycle from the all-ones start state.
    uint32_t taps = 0;
    int n = 0;
    int len = mlsLength + 1;
    while (len > 1) { len >>= 1; ++n; }

    switch (n)
    {
        case 15: taps = 0x6000; break;  // x^15 + x^14 + 1
        case 14: taps = 0x2015; break;  // x^14 + x^5 + x^3 + x + 1
        case 13: taps = 0x100D; break;  // x^13 + x^4 + x^3 + x + 1
        case 12: taps = 0x0829; break;  // x^12 + x^6 + x^4 + x + 1
        case 11: taps = 0x0402; break;  // x^11 + x^2 + 1
        case 10: taps = 0x0204; break;  // x^10 + x^3 + 1
        case 9:  taps = 0x0108; break;  // x^9 + x^4 + 1
        case 8:  taps = 0x008E; break;  // x^8 + x^4 + x^3 + x^2 + 1
        case 7:  taps = 0x0041; break;  // x^7 + x + 1
        case 6:  taps = 0x0021; break;  // x^6 + x + 1
        case 5:  taps = 0x0012; break;  // x^5 + x^2 + 1
        case 4:  taps = 0x0009; break;  // x^4 + x + 1
        case 3:  taps = 0x0005; break;  // x^3 + x + 1
        default: taps = 0x0400; break;  // fallback
    }

    uint32_t state = 0x7FFFFFFF & ((1u << n) - 1);  // all-ones starting state
    uint32_t mask = (1u << n) - 1;

    for (int i = 0; i < mlsLength; ++i)
    {
        // Output the LSB mapped to ±1
        mlsSequence[i] = (state & 1) ? 1.0f : -1.0f;

        // Shift left and apply feedback
        uint32_t feedback = 0;
        for (int bit = 0; bit < n; ++bit)
        {
            if ((taps >> bit) & 1)
                feedback ^= (state >> bit) & 1;
        }
        state = ((state << 1) | feedback) & mask;
    }

    // Normalize amplitude
    double scale = amplitude;
    for (auto& s : mlsSequence)
        s = static_cast<float> (s * scale);
}

void Impulse::generate (juce::AudioBuffer<float>& buffer,
                        int startSample,
                        int numSamples)
{
    const auto numChannels = buffer.getNumChannels();

    if (useMls)
    {
        for (int s = 0; s < numSamples; ++s)
        {
            float sample = 0.0f;
            if (mlsIndex < mlsLength)
                sample = mlsSequence[mlsIndex++];

            for (int ch = 0; ch < numChannels; ++ch)
                buffer.getWritePointer (ch)[startSample + s] = sample;
        }
    }
    else
    {
        // Single impulse
        for (int s = 0; s < numSamples; ++s)
        {
            float sample = 0.0f;
            if (! impulseDone && (currentSample + s) < 1.0)
                sample = static_cast<float> (amplitude);

            for (int ch = 0; ch < numChannels; ++ch)
                buffer.getWritePointer (ch)[startSample + s] = sample;
        }

        if (currentSample == 0.0)
            impulseDone = true;
    }

    currentSample += numSamples;
}
