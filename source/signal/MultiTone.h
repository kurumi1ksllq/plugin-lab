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

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MultiTone)
};
