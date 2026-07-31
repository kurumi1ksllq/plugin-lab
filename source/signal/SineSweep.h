#pragma once

#include "SignalGenerator.h"

/**
 * Logarithmic sine sweep from f0 to f1 over a configurable duration.
 * Used for frequency response and impulse response measurement.
 *
 * Formula: x(t) = sin(2π * f0 * L * (exp(t/L) - 1))
 * where L = T / ln(f1/f0)
 */
class SineSweep : public SignalGenerator
{
public:
    SineSweep();
    ~SineSweep() override = default;

    //==============================================================================
    /** Set the sweep frequency range. */
    void setFrequencyRange (double startFreqHz, double endFreqHz);

    /** Set the sweep duration in seconds. */
    void setDuration (double seconds);

    /** Set the output amplitude (0.0 - 1.0). */
    void setAmplitude (double amp);

    //==============================================================================
    void prepare (double sampleRate, int blockSize) override;
    void generate (juce::AudioBuffer<float>& buffer,
                   int startSample,
                   int numSamples) override;
    int64_t getTotalLength() const override;
    void reset() override;

private:
    double startFreq = 20.0;
    double endFreq = 20000.0;
    double durationSec = 5.0;
    double amplitude = 0.5;

    double sweepRate = 0.0;  // L = duration / ln(end/start)

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SineSweep)
};
