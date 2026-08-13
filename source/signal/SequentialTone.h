#pragma once

#include "SignalGenerator.h"

/**
 * Sequential single-tone test signal — one sine tone per segment, played
 * one after another (optionally separated by a silence gap).
 *
 * Purpose (DESIGN.md §3.2): THD measurement must use SINGLE-tone excitation
 * — one fundamental at a time — so each tone's harmonics are measured in a
 * window that contains ONLY that tone. The old MultiTone excitation made
 * octave-spaced harmonics (100 Hz H2 = 200 Hz, H4 = 400 Hz, …) land exactly
 * on co-injected fundamentals, inflating every tone's THD toward ~100% even
 * in a perfectly linear plugin (issue #38). MultiTone stays reserved for IMD
 * measurement — THD uses this generator, never the two mixed.
 *
 * Phase restarts at 0 for every segment (deterministic, test-friendly).
 * All channels receive the same signal.
 */
class SequentialTone : public SignalGenerator
{
public:
    SequentialTone();
    ~SequentialTone() override = default;

    //==============================================================================
    /** Set frequencies in Hz, one per segment (e.g. {100, 200, 400}). */
    void setFrequencies (const std::vector<double>& freqs);

    /** Set the duration of each tone segment in seconds (default: 1.0). */
    void setSegmentDuration (double seconds);

    /** Set overall amplitude (0.0 - 1.0). */
    void setAmplitude (double amp);

    /** Set an optional inter-segment silence gap in seconds (default 0.0).
     *  HarmonicAnalysis assumes contiguous segments (gapSeconds == 0.0) —
     *  keep the default unless a test shows FFT windowing leakage problems. */
    void setGapSeconds (double seconds);

    //==============================================================================
    void prepare (double sampleRate, int blockSize) override;
    void generate (juce::AudioBuffer<float>& buffer,
                   int startSample,
                   int numSamples) override;
    int64_t getTotalLength() const override;
    void reset() override;

private:
    std::vector<double> frequencies;
    double segmentDurationSec = 1.0;
    double gapSec = 0.0;
    double amplitude = 0.3;
    int64_t toneLenSamples = 0;     // samples per tone segment
    int64_t segmentLenSamples = 0;  // samples per segment incl. gap

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SequentialTone)
};
