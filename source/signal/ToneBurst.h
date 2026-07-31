#pragma once

#include "SignalGenerator.h"

/**
 * ToneBurst — short bursts of a sine tone at configurable levels.
 * Used for compression curve measurement (sweep input levels,
 * measure output levels).
 *
 * Each burst is a short sine tone followed by silence.
 * Bursts can be at different amplitudes to measure gain reduction
 * across the dynamic range.
 */
class ToneBurst : public SignalGenerator
{
public:
    ToneBurst();
    ~ToneBurst() override = default;

    //==============================================================================
    /** Set the tone frequency in Hz. */
    void setFrequency (double freqHz);

    /** Set burst duration in milliseconds. */
    void setBurstDurationMs (double ms);

    /** Set silence gap between bursts in milliseconds. */
    void setGapDurationMs (double ms);

    /** Set amplitude levels for each burst (0.0 - 1.0). */
    void setLevels (const std::vector<double>& amplitudes);

    /** Set a single overall amplitude (one burst only). */
    void setAmplitude (double amp);

    //==============================================================================
    void prepare (double sampleRate, int blockSize) override;
    void generate (juce::AudioBuffer<float>& buffer,
                   int startSample,
                   int numSamples) override;
    int64_t getTotalLength() const override;
    void reset() override;

private:
    double frequency = 1000.0;
    double burstSamples = 0;
    double gapSamples = 0;
    std::vector<double> levels;

    int currentBurstIndex = 0;
    int64_t positionInBurst = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ToneBurst)
};
