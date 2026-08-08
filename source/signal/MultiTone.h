#pragma once

#include "SignalGenerator.h"

/**
 * Multi-tone test signal — multiple sine waves summed together.
 * Useful for harmonic analysis (testing multiple frequencies at once).
 */
class MultiTone : public SignalGenerator
{
public:
    MultiTone();
    ~MultiTone() override = default;

    //==============================================================================
    /** Set frequencies in Hz (e.g. {100, 300, 500, 1000, 3000, 5000}). */
    void setFrequencies (const std::vector<double>& freqs);

    /** Set duration in seconds (default: 2.0). */
    void setDuration (double seconds);

    /** Set overall amplitude (0.0 - 1.0). */
    void setAmplitude (double amp);

    /** Deterministic random initial phases (one per frequency) to lower the
     *  crest factor of the summed multi-tone. Seed 0 (default) keeps the
     *  legacy all-zero-phase waveform; any non-zero seed reproduces the same
     *  waveform on every run (xorshift32, deterministic). */
    void setRandomPhaseSeed (uint32_t seed) { phaseSeed = seed; }

    //==============================================================================
    void prepare (double sampleRate, int blockSize) override;
    void generate (juce::AudioBuffer<float>& buffer,
                   int startSample,
                   int numSamples) override;
    int64_t getTotalLength() const override;
    void reset() override;

private:
    std::vector<double> frequencies;
    double durationSec = 2.0;
    double amplitude = 0.3;
    uint32_t phaseSeed = 0;
    std::vector<double> phases;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MultiTone)
};
