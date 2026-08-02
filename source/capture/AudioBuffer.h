#pragma once

#include <JuceHeader.h>

/**
 * Holds recorded dry (reference) and wet (processed) audio data
 * from a measurement pass.
 *
 * Thread-safe: written during capture, read after capture completes.
 */
class CaptureBuffer
{
public:
    CaptureBuffer() = default;
    ~CaptureBuffer() = default;

    //==============================================================================
    /** Prepare the buffer for a recording session. */
    void prepare (int numChannels, int expectedBlockSize);

    /** Append a block of dry and wet audio. */
    void append (const juce::AudioBuffer<float>& dry,
                 const juce::AudioBuffer<float>& wet);

    /** Shrink the internal buffers to exactly the recorded sample count so
     *  that analyzers see the true captured length instead of the
     *  pre-allocated capacity (which is 30 s of silent padding). */
    void trim();

    /** Clear all recorded data. */
    void clear();

    //==============================================================================
    /** Get the recorded dry signal (reference / input). */
    const juce::AudioBuffer<float>& getDryBuffer() const { return dryBuffer; }

    /** Get the recorded wet signal (processed / output). */
    const juce::AudioBuffer<float>& getWetBuffer() const { return wetBuffer; }

    /** Get the total number of recorded samples per channel. */
    int64_t getNumRecordedSamples() const { return totalSamples; }

    /** Get the sample rate used during capture. */
    double getSampleRate() const { return sampleRate; }

    /** Set/get the sample rate metadata. */
    void setSampleRate (double sr) { sampleRate = sr; }

private:
    juce::AudioBuffer<float> dryBuffer;
    juce::AudioBuffer<float> wetBuffer;
    int64_t totalSamples = 0;
    int numChannels = 2;
    double sampleRate = 48000.0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CaptureBuffer)
};
