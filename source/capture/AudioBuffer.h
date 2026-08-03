#pragma once

#include <JuceHeader.h>

/**
 * Holds recorded dry (reference) and wet (processed) audio data
 * from a measurement pass.
 *
 * Thread-safe: written during capture, read after capture completes.
 *
 * Incremental WAV flush: when configured via setFlushConfig(), captured audio
 * is mirrored to a 24-bit PCM .wav file every intervalSec so a plugin crash
 * (which kills the process) loses at most the last interval of audio. The
 * file is kept valid at every flush boundary (sizes patched after each write)
 * so it stays readable with simple tools immediately after a crash.
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
                 const juce::AudioBuffer<float>& wet,
                 int numSamples);

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
    void setSampleRate (double sr)
    {
        sampleRate = sr;
        if (flushEnabled())
            flushIntervalSamples = static_cast<int64_t> (sampleRate * flushIntervalSec);
    }

    /** Configure incremental flushing of captured audio to a WAV file so a
     *  plugin crash loses at most intervalSec of audio. Call before capture
     *  starts; an empty wavPath disables flushing (the default).
     *
     *  Format: single interleaved 24-bit PCM WAV with 2 * numChannels
     *  channels, layout [dry ch0..N-1, wet ch0..N-1]. The file is valid at
     *  every flush boundary, so it stays readable even if the process dies
     *  mid-capture without trim().
     */
    void setFlushConfig (const juce::File& wavPath, double intervalSec);

private:
    juce::AudioBuffer<float> dryBuffer;
    juce::AudioBuffer<float> wetBuffer;
    int64_t totalSamples = 0;
    int numChannels = 2;
    double sampleRate = 48000.0;

    //==============================================================================
    // Incremental WAV flush state (single-writer during capture, like the
    // in-memory buffers — no mutex, synchronous writes on the capture thread)
    bool flushEnabled() const { return ! flushWavPath.getFullPathName().isEmpty(); }

    /** Write all samples since lastFlushedSamples to the wav and patch the
     *  header so the file is valid at every flush boundary. */
    void flush();

    /** Patch the header with the final sizes and close the wav stream. */
    void finaliseWav();

    /** Write the 44-byte RIFF header (dataSizeBytes = 0 for a placeholder). */
    void writeWavHeader (uint32_t dataSizeBytes);

    /** Patch the RIFF/data chunk sizes to match the current wavDataBytes. */
    void patchWavHeader();

    juce::File flushWavPath;
    double flushIntervalSec = 0.0;
    int64_t flushIntervalSamples = 0;
    int64_t lastFlushedSamples = 0;
    int64_t wavDataBytes = 0;
    bool flushFailed = false;
    std::unique_ptr<juce::FileOutputStream> wavStream;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CaptureBuffer)
};
