#include "FilePlayback.h"

#include <cmath>

FilePlayback::FilePlayback()
{
    formatManager.registerBasicFormats();
}

FilePlayback::~FilePlayback() = default;

void FilePlayback::setFile (const juce::File& f)
{
    sourceFile = f;
}

bool FilePlayback::isLoaded() const noexcept
{
    return loaded;
}

void FilePlayback::prepare (double sr, int bs)
{
    // Stores sr/bs and calls the virtual reset() (harmless on any stale chain).
    SignalGenerator::prepare (sr, bs);

    // Drop any previous chain before building a new one.
    resampler.reset();
    readerSource.reset();

    // Reset metadata to the "not loaded" state.
    sourceSampleRate = 0.0;
    fileNumChannels = 0;
    fileLengthInSamples = 0;
    outputLength = 0;
    resampleRatio = 0.0;
    sourcePath.clear();
    loaded = false;

    if (! sourceFile.existsAsFile())
        return;

    // createReaderFor() returns a caller-owned reader (nullptr on failure).
    // It is wrapped in a unique_ptr so a failure below cannot leak it, then
    // released into the reader source which takes ownership.
    std::unique_ptr<juce::AudioFormatReader> reader (formatManager.createReaderFor (sourceFile));
    if (reader == nullptr)
        return;

    sourceSampleRate    = reader->sampleRate;
    fileNumChannels     = static_cast<int> (reader->numChannels);
    fileLengthInSamples = reader->lengthInSamples;

    if (sourceSampleRate <= 0.0 || fileNumChannels <= 0)
        return;

    readerSource = std::make_unique<juce::AudioFormatReaderSource> (reader.release(), true);
    readerSource->setNextReadPosition (0);

    outputLength = static_cast<int64_t> (std::llround (fileLengthInSamples * sr / sourceSampleRate));

    if (! juce::approximatelyEqual (sourceSampleRate, sr))
    {
        resampleRatio = sourceSampleRate / sr;
        resampler = std::make_unique<juce::ResamplingAudioSource> (readerSource.get(), false, 2);
        resampler->setResamplingRatio (resampleRatio);
        resampler->prepareToPlay (bs, sr);
    }

    sourcePath = sourceFile.getFullPathName();
    loaded = true;
}

void FilePlayback::generate (juce::AudioBuffer<float>& buffer,
                             int startSample,
                             int numSamples)
{
    if (! loaded)
    {
        buffer.clear (startSample, numSamples);
        return;
    }

    // Both the reader source (no resampler) and the resampler emit silence
    // past the end of the file, so no explicit end-of-file handling is needed.
    const juce::AudioSourceChannelInfo info (&buffer, startSample, numSamples);

    if (resampler != nullptr)
        resampler->getNextAudioBlock (info);
    else
        readerSource->getNextAudioBlock (info);

    currentSample += numSamples;
}

int64_t FilePlayback::getTotalLength() const
{
    return loaded ? outputLength : 0;
}

void FilePlayback::reset()
{
    currentSample = 0.0;

    if (resampler != nullptr)
    {
        // ResamplingAudioSource is not a PositionableAudioSource, so the seek
        // goes through the underlying reader source; flushBuffers() clears the
        // resampler's filters and internal buffers so the next generate() pass
        // is bit-identical to the first one.
        readerSource->setNextReadPosition (0);
        resampler->flushBuffers();
    }
    else if (readerSource != nullptr)
    {
        readerSource->setNextReadPosition (0);
    }
}

double FilePlayback::getSourceSampleRate() const noexcept
{
    return sourceSampleRate;
}

double FilePlayback::getResampleRatio() const noexcept
{
    return loaded ? resampleRatio : 0.0;
}

double FilePlayback::getDurationSec() const noexcept
{
    return sourceSampleRate > 0.0 ? fileLengthInSamples / sourceSampleRate : 0.0;
}

int FilePlayback::getFileNumChannels() const noexcept
{
    return fileNumChannels;
}

juce::String FilePlayback::getSourcePath() const
{
    return sourcePath;
}
