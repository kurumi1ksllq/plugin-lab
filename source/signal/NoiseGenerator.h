#pragma once

#include "SignalGenerator.h"

#include <random>
#include <cstdint>

/**
 * White- and pink-noise generator with a fixed-seed reproducible RNG and a
 * finite, configurable duration.
 *
 * White noise is produced from a std::mt19937 seeded with the configured
 * seed, mapped to a uniform [-1, 1) distribution and scaled by the
 * amplitude. Pink noise is produced by filtering that white noise through
 * the classic Paul Kellet 3-pole IIR approximation of a -3 dB/octave
 * spectrum (see http://www.firstpr.com.au/dsp/pink-noise/).
 *
 * The generator is fully deterministic: the same seed always produces the
 * same sample sequence, and reset() replays it from the start.
 */
class NoiseGenerator final : public SignalGenerator
{
public:
    enum class Type
    {
        white,
        pink
    };

    NoiseGenerator() = default;
    ~NoiseGenerator() override = default;

    //==============================================================================
    /** Set the noise colour (white or pink). */
    void setType (Type t);

    /** Set the total signal length in seconds (finite output). */
    void setDuration (double seconds);

    /** Set the output amplitude (peak sample level). */
    void setAmplitude (double amp);

    /** Set the RNG seed. Default: 0x2E42A5. */
    void setSeed (uint32_t seed);

    /** Get the currently configured RNG seed. */
    uint32_t getSeed() const noexcept;

    //==============================================================================
    void prepare (double sampleRate, int blockSize) override;
    void generate (juce::AudioBuffer<float>& buffer,
                   int startSample,
                   int numSamples) override;
    int64_t getTotalLength() const override;
    void reset() override;

private:
    /** Re-seed the RNG with getSeed() and clear the pink filter state. */
    void reseed();

    Type type = Type::white;
    double durationSec = 0.0;      // 0.0 = no duration configured -> length 0
    double amplitude = 1.0;
    uint32_t seed = 0x2E42A5;

    std::mt19937 rng;
    std::uniform_real_distribution<double> uniform { -1.0, 1.0 };

    // Paul Kellet pink-noise filter state.
    double b0 = 0.0, b1 = 0.0, b2 = 0.0, b3 = 0.0, b4 = 0.0, b5 = 0.0, b6 = 0.0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (NoiseGenerator)
};
