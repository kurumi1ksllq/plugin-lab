#pragma once

#include "SignalGenerator.h"

/**
 * FilePlayback — plays back an audio file as a test signal.
 *
 * The file path is set via setFile() (before prepare()); the file is
 * actually opened during prepare(), which records its metadata and builds
 * the playback chain:
 *
 *   AudioFormatReader  →  AudioFormatReaderSource  →  [ResamplingAudioSource]
 *
 * The resampler stage is only inserted when the file's sample rate differs
 * from the session rate. A mono file is duplicated to every output channel
 * by the underlying reader.
 *
 * Error contract: if the file is missing or unreadable, isLoaded() returns
 * false, getTotalLength() returns 0, all metadata getters return zero/empty
 * values, and generate() writes silence — nothing ever throws.
 */
class FilePlayback final : public SignalGenerator
{
public:
    FilePlayback();
    ~FilePlayback() override;

    //==============================================================================
    /** Set the file to play back. Must be called before prepare(). */
    void setFile (const juce::File& f);

    /** Returns true if the file was successfully opened during prepare(). */
    bool isLoaded() const noexcept;

    //==============================================================================
    void prepare (double sampleRate, int blockSize) override;
    void generate (juce::AudioBuffer<float>& buffer,
                   int startSample,
                   int numSamples) override;
    int64_t getTotalLength() const override;
    void reset() override;

    //==============================================================================
    /** Sample rate of the loaded file, or 0 if not loaded. */
    double getSourceSampleRate() const noexcept;

    /** Resample ratio (sourceSR / sessionSR); 0 when not loaded or at 1:1. */
    double getResampleRatio() const noexcept;

    /** Duration of the file in seconds, or 0 if not loaded. */
    double getDurationSec() const noexcept;

    /** Number of channels in the loaded file, or 0 if not loaded. */
    int getFileNumChannels() const noexcept;

    /** Full path of the loaded file, or empty if not loaded. */
    juce::String getSourcePath() const;

private:
    juce::AudioFormatManager formatManager;
    juce::File sourceFile;

    // Ownership: reader is owned by readerSource (deleteWhenDeleted=true);
    // resampler does NOT own readerSource (deleteInput=false).
    std::unique_ptr<juce::AudioFormatReaderSource> readerSource;
    std::unique_ptr<juce::ResamplingAudioSource> resampler;

    double sourceSampleRate = 0.0;
    int fileNumChannels = 0;
    int64_t fileLengthInSamples = 0;
    int64_t outputLength = 0;
    double resampleRatio = 0.0;
    juce::String sourcePath;
    bool loaded = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FilePlayback)
};
