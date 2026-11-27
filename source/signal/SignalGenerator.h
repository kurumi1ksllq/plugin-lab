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
    /**
     * Channel-polarity mode (issue #104 / ADR-0003). Decides how one generated
     * sample is written into a multi-channel buffer:
     *
     *   mono  - the same value in every channel (the historical behaviour)
     *   mid   - L = R = value (MidSide mid-only excitation, Run A)
     *   side  - L = value, R = -value (MidSide side-only excitation, Run B)
     *
     * mono and mid are identical on a stereo buffer; they stay distinct so a
     * caller can state its intent. Generators that write per channel
     * themselves ignore this mode (opt-in via writeSample).
     */
    enum class ChannelMode
    {
        mono,
        mid,
        side
    };

    SignalGenerator() = default;
    virtual ~SignalGenerator() = default;

    //==============================================================================
    /** Configure the generator for a given sample rate and block size. */
    virtual void prepare (double sampleRate, int blockSize);

    /** Fill the given buffer with generated samples.
     *  @param buffer  Audio buffer to fill (all channels get the same signal
     *                 unless a ChannelMode is set - see writeSample).
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

    //==============================================================================
    /** Select the channel-polarity mode used by writeSample (default: mono). */
    void setChannelMode (ChannelMode mode)  { channelMode = mode; }
    ChannelMode getChannelMode() const      { return channelMode; }

protected:
    double sampleRate = 48000.0;
    int blockSize = 512;
    double currentSample = 0.0;
    ChannelMode channelMode = ChannelMode::mono;

    /** Write one generated sample into every channel of the buffer at index,
     *  applying the channel-polarity mode. Generators call this instead of
     *  writing the channels themselves, so the mid/side excitation lives in
     *  exactly one place. */
    void writeSample (juce::AudioBuffer<float>& buffer, int index, float sample) const;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SignalGenerator)
};
