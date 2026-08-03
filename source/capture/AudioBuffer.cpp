#include "AudioBuffer.h"

#include <cstring>
#include <vector>

namespace
{
//==============================================================================
// Little-endian byte helpers for the hand-rolled WAV header

void writeU16LE (uint8_t* dest, uint16_t value)
{
    dest[0] = static_cast<uint8_t> (value & 0xFF);
    dest[1] = static_cast<uint8_t> ((value >> 8) & 0xFF);
}

void writeU32LE (uint8_t* dest, uint32_t value)
{
    dest[0] = static_cast<uint8_t> (value & 0xFF);
    dest[1] = static_cast<uint8_t> ((value >> 8) & 0xFF);
    dest[2] = static_cast<uint8_t> ((value >> 16) & 0xFF);
    dest[3] = static_cast<uint8_t> ((value >> 24) & 0xFF);
}
} // namespace

void CaptureBuffer::prepare (int channels, int /*expectedBlockSize*/)
{
    numChannels = channels;
    clear();

    // Pre-allocate ~30 seconds at 48kHz as initial capacity
    int initialSize = static_cast<int> (sampleRate * 30);
    dryBuffer.setSize (numChannels, initialSize, false, true, false);
    wetBuffer.setSize (numChannels, initialSize, false, true, false);
}

void CaptureBuffer::clear()
{
    // Flush any captured tail and close the wav writer before resetting, so
    // the file keeps the old run's data (a later re-open truncates it — see
    // flush())
    flush();
    finaliseWav();

    dryBuffer.clear();
    wetBuffer.clear();
    totalSamples = 0;
    lastFlushedSamples = 0;
    wavDataBytes = 0;
    flushFailed = false;
}

void CaptureBuffer::append (const juce::AudioBuffer<float>& dry,
                           const juce::AudioBuffer<float>& wet,
                           int numSamples)
{
    const int numNewSamples = juce::jmin (numSamples,
                                          dry.getNumSamples(),
                                          wet.getNumSamples());

    if (numNewSamples <= 0)
        return;

    int requiredSize = static_cast<int> (totalSamples + numNewSamples);

    // Grow buffers if needed
    if (requiredSize > dryBuffer.getNumSamples())
    {
        int newSize = juce::nextPowerOfTwo (requiredSize);
        dryBuffer.setSize (numChannels, newSize, false, true, false);
        wetBuffer.setSize (numChannels, newSize, false, true, false);
    }

    // Copy dry and wet samples
    for (int ch = 0; ch < juce::jmin (numChannels, dry.getNumChannels()); ++ch)
    {
        dryBuffer.copyFrom (ch, static_cast<int> (totalSamples),
                            dry.getReadPointer (ch), numNewSamples);
    }

    for (int ch = 0; ch < juce::jmin (numChannels, wet.getNumChannels()); ++ch)
    {
        wetBuffer.copyFrom (ch, static_cast<int> (totalSamples),
                            wet.getReadPointer (ch), numNewSamples);
    }

    totalSamples += numNewSamples;

    // Incremental WAV flush: mirror captured audio to disk every
    // flushIntervalSamples so a plugin crash loses at most one interval
    if (flushEnabled()
        && ! flushFailed
        && totalSamples - lastFlushedSamples >= flushIntervalSamples)
    {
        flush();
    }
}

void CaptureBuffer::trim()
{
    // Write any remaining tail and close the writer; the header is patched
    // with the final sizes so the file is fully valid on normal completion
    flush();
    finaliseWav();

    dryBuffer.setSize (numChannels, static_cast<int> (totalSamples), true, false, false);
    wetBuffer.setSize (numChannels, static_cast<int> (totalSamples), true, false, false);
}

void CaptureBuffer::setFlushConfig (const juce::File& wavPath, double intervalSec)
{
    flushWavPath = wavPath;
    flushIntervalSec = intervalSec;
    flushIntervalSamples = static_cast<int64_t> (sampleRate * intervalSec);
}

//==============================================================================
void CaptureBuffer::flush()
{
    if (! flushEnabled() || flushFailed)
        return;

    const int64_t numNewSamples = totalSamples - lastFlushedSamples;
    if (numNewSamples <= 0)
        return;

    // Lazily open the writer on the first flush: delete any file left by a
    // previous run so the stream starts fresh (FileOutputStream opens an
    // existing file in append mode and does NOT truncate it)
    if (wavStream == nullptr)
    {
        flushWavPath.deleteFile();

        // Buffer size 0 → unbuffered writes, so setPosition() seeks stay
        // consistent with the byte stream (the default 16 KiB buffering
        // would corrupt the header patches)
        wavStream = std::make_unique<juce::FileOutputStream> (flushWavPath, 0);
        if (wavStream->failedToOpen())
        {
            flushFailed = true;
            wavStream.reset();
            return;
        }

        wavDataBytes = 0;
        writeWavHeader (0);   // placeholder sizes — patched after every flush
    }

    // Interleave dry/wet into a temp buffer: [dry ch0..N-1, wet ch0..N-1]
    const int totalChannels = 2 * numChannels;
    const int n = static_cast<int> (numNewSamples);
    const int start = static_cast<int> (lastFlushedSamples);

    juce::AudioBuffer<float> interleaved (totalChannels, n);
    for (int ch = 0; ch < numChannels; ++ch)
    {
        if (ch < dryBuffer.getNumChannels())
            interleaved.copyFrom (ch, 0, dryBuffer.getReadPointer (ch, start), n);
        if (ch < wetBuffer.getNumChannels())
            interleaved.copyFrom (numChannels + ch, 0, wetBuffer.getReadPointer (ch, start), n);
    }

    // Convert to 24-bit PCM: sample → int32 in [-8388608, 8388607], written
    // little-endian (low, mid, high byte)
    std::vector<uint8_t> pcmBytes (static_cast<size_t> (n) * static_cast<size_t> (totalChannels) * 3u);
    size_t offset = 0;

    for (int s = 0; s < n; ++s)
    {
        for (int ch = 0; ch < totalChannels; ++ch)
        {
            const float sample = interleaved.getSample (ch, s);
            const int32_t value = static_cast<int32_t> (juce::jlimit (-1.0f, 1.0f, sample) * 8388607.0f);
            pcmBytes[offset++] = static_cast<uint8_t> (value & 0xFF);
            pcmBytes[offset++] = static_cast<uint8_t> ((value >> 8) & 0xFF);
            pcmBytes[offset++] = static_cast<uint8_t> ((value >> 16) & 0xFF);
        }
    }

    if (! wavStream->write (pcmBytes.data(), pcmBytes.size()))
    {
        flushFailed = true;
        return;
    }

    wavDataBytes += static_cast<int64_t> (pcmBytes.size());
    lastFlushedSamples = totalSamples;

    // Keep the file valid at every flush boundary: patch sizes, force to disk
    patchWavHeader();
    wavStream->flush();
}

void CaptureBuffer::finaliseWav()
{
    if (wavStream == nullptr)
        return;

    patchWavHeader();   // sizes match the final data size
    wavStream->flush();
    wavStream.reset();  // closes the file
}

void CaptureBuffer::writeWavHeader (uint32_t dataSizeBytes)
{
    if (wavStream == nullptr)
        return;

    const uint32_t totalChannels = static_cast<uint32_t> (2 * numChannels);
    const uint32_t bytesPerFrame = totalChannels * 3u;

    uint8_t header[44] = {};
    std::memcpy (header, "RIFF", 4);
    writeU32LE (header + 4, dataSizeBytes + 36);        // RIFF chunk size (file size - 8)
    std::memcpy (header + 8, "WAVE", 4);
    std::memcpy (header + 12, "fmt ", 4);
    writeU32LE (header + 16, static_cast<uint32_t> (16));   // fmt chunk size
    writeU16LE (header + 20, static_cast<uint16_t> (1));    // PCM
    writeU16LE (header + 22, static_cast<uint16_t> (totalChannels));
    writeU32LE (header + 24, static_cast<uint32_t> (sampleRate));
    writeU32LE (header + 28, static_cast<uint32_t> (sampleRate * static_cast<double> (bytesPerFrame)));  // byte rate
    writeU16LE (header + 32, static_cast<uint16_t> (bytesPerFrame));  // block align
    writeU16LE (header + 34, static_cast<uint16_t> (24));   // bits per sample
    std::memcpy (header + 36, "data", 4);
    writeU32LE (header + 40, dataSizeBytes);            // data chunk size

    if (! wavStream->write (header, sizeof (header)))
        flushFailed = true;
}

void CaptureBuffer::patchWavHeader()
{
    if (wavStream == nullptr)
        return;

    uint8_t sizeBytes[4];

    writeU32LE (sizeBytes, static_cast<uint32_t> (wavDataBytes + 36));
    if (! wavStream->setPosition (4) || ! wavStream->write (sizeBytes, 4))
    {
        flushFailed = true;
        return;
    }

    writeU32LE (sizeBytes, static_cast<uint32_t> (wavDataBytes));
    if (! wavStream->setPosition (40) || ! wavStream->write (sizeBytes, 4))
    {
        flushFailed = true;
        return;
    }

    // Back to the end of the data for the next append; a seek failure here
    // would misplace the next data write, so it counts as an I/O failure too.
    if (! wavStream->setPosition (44 + wavDataBytes))
        flushFailed = true;
}
