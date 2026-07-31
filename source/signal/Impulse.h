#pragma once

#include "SignalGenerator.h"

/**
 * Impulse generator — a single sample impulse or a Maximum Length
 * Sequence (MLS) for impulse response measurement.
 */
class Impulse : public SignalGenerator
{
public:
    Impulse();
    ~Impulse() override = default;

    //==============================================================================
    /** Set amplitude of the impulse. */
    void setAmplitude (double amp);

    /** Use MLS instead of a single impulse. */
    void useMLS (bool useMLS);

    /** Set MLS sequence length (must be 2^n - 1, e.g. 1023, 4095, 16383). */
    void setMLSLength (int length);

    //==============================================================================
    void prepare (double sampleRate, int blockSize) override;
    void generate (juce::AudioBuffer<float>& buffer,
                   int startSample,
                   int numSamples) override;
    int64_t getTotalLength() const override;
    void reset() override;

private:
    double amplitude = 0.5;
    bool useMls = false;
    int mlsLength = 32767;

    std::vector<float> mlsSequence;
    int mlsIndex = 0;
    bool impulseDone = false;

    void generateMLS();

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Impulse)
};
