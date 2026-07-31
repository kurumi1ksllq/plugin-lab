#pragma once

#include <JuceHeader.h>

/**
 * Base class for all test signal generators.
 * Generators fill a buffer with audio samples that are routed through
 * the plugin-under-test.
 */
class SignalGenerator
{
public:
    SignalGenerator() = default;
    virtual ~SignalGenerator() = default;

    //==============================================================================
    /** Configure the generator for a given sample rate and block size. */
    virtual void prepare (double sampleRate, int blockSize);

    /** Fill the given buffer with generated samples.
     *  @param buffer  Audio buffer to fill (may be stereo; all channels get same signal).
     *  @param startSample  Offset within the buffer to start writing.
     *  @param numSamples   Number of samples to generate.
     */
    virtual void generate (juce::AudioBuffer<float>& buffer,
                           int startSample,
                           int numSamples) = 0;

    /** Returns the total number of samples this generator will produce,
     *  or -1 if the generator runs indefinitely. */
    virtual int64_t getTotalLength() const = 0;

    /** Reset the generator to its initial state. */
    virtual void reset() = 0;

protected:
    double sampleRate = 48000.0;
    int blockSize = 512;
    double currentSample = 0.0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SignalGenerator)
};
