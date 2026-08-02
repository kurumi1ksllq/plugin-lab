#include "NoiseGenerator.h"

#include <algorithm>
#include <cmath>

namespace
{
// Paul Kellet pink-noise filter coefficients (firstpr.com.au/dsp/pink-noise/).
constexpr double kPoleB0 = 0.99886;
constexpr double kPoleB1 = 0.99332;
constexpr double kPoleB2 = 0.96900;
constexpr double kPoleB3 = 0.86650;
constexpr double kPoleB4 = 0.55000;
constexpr double kPoleB5 = -0.7616;

constexpr double kCoeff0 = 0.0555179;
constexpr double kCoeff1 = 0.0750759;
constexpr double kCoeff2 = 0.1538520;
constexpr double kCoeff3 = 0.3104856;
constexpr double kCoeff4 = 0.5329522;
constexpr double kCoeff5 = 0.0168980;
constexpr double kCoeff6 = 0.5362;
constexpr double kCoeff7 = 0.115926;
constexpr double kGain = 0.11;
} // namespace

void NoiseGenerator::setType (Type t)
{
    type = t;
}

void NoiseGenerator::setDuration (double seconds)
{
    durationSec = seconds;
}

void NoiseGenerator::setAmplitude (double amp)
{
    amplitude = amp;
}

void NoiseGenerator::setSeed (uint32_t s)
{
    seed = s;
    reseed();
}

uint32_t NoiseGenerator::getSeed() const noexcept
{
    return seed;
}

void NoiseGenerator::prepare (double sr, int bs)
{
    SignalGenerator::prepare (sr, bs);   // stores rate/block and calls reset()
}

void NoiseGenerator::reset()
{
    reseed();
}

void NoiseGenerator::reseed()
{
    rng.seed (seed);
    b0 = b1 = b2 = b3 = b4 = b5 = b6 = 0.0;
}

int64_t NoiseGenerator::getTotalLength() const
{
    if (durationSec <= 0.0)
        return 0;
    return static_cast<int64_t> (std::llround (sampleRate * durationSec));
}

void NoiseGenerator::generate (juce::AudioBuffer<float>& buffer,
                               int startSample,
                               int numSamples)
{
    const auto numChannels = buffer.getNumChannels();

    for (int s = 0; s < numSamples; ++s)
    {
        const double white = uniform (rng) * amplitude;   // uniform [-1, 1) * amp

        float sample;
        if (type == Type::white)
        {
            sample = static_cast<float> (white);
        }
        else
        {
            b0 = kPoleB0 * b0 + white * kCoeff0;
            b1 = kPoleB1 * b1 + white * kCoeff1;
            b2 = kPoleB2 * b2 + white * kCoeff2;
            b3 = kPoleB3 * b3 + white * kCoeff3;
            b4 = kPoleB4 * b4 + white * kCoeff4;
            b5 = kPoleB5 * b5 - white * kCoeff5;

            const double out = (b0 + b1 + b2 + b3 + b4 + b5 + b6 + white * kCoeff6) * kGain;
            b6 = white * kCoeff7;

            // Keep the peak within the configured amplitude (rarely triggered).
            sample = static_cast<float> (std::clamp (out, -amplitude, amplitude));
        }

        for (int ch = 0; ch < numChannels; ++ch)
            buffer.getWritePointer (ch)[startSample + s] = sample;
    }
}
