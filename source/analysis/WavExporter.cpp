#include "WavExporter.h"
#include "../utils/CrashLog.h"

#include <cstring>
#include <vector>

namespace
{
//==============================================================================
// Little-endian byte helpers for the hand-rolled WAV header (mirror
// CaptureBuffer's writeWavHeader style — AudioBuffer.cpp)

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

namespace WavExporter
{

bool exportTracks (const juce::AudioBuffer<float>& dry,
                   const juce::AudioBuffer<float>& wet,
                   double sampleRate,
                   const juce::File& wavPath)
{
    // Layout contract: 3 * dry channels [dry, wet, bypass]; wet may carry
    // more channels than dry (reads are clamped), but fewer would leave a
    // gap in the interleaved layout — reject it.
    const int numChannels   = dry.getNumChannels();
    const int totalChannels = 3 * numChannels;
    const int numSamples    = dry.getNumSamples();

    if (wet.getNumChannels() < numChannels)
    {
        CRASH_LOG_WARN ("WavExporter", "wet channel count " + juce::String (wet.getNumChannels())
                         + " < dry channel count " + juce::String (numChannels) + " for "
                         + wavPath.getFullPathName());
        return false;
    }

    // Sizes are computable up front — the dry/wet buffers are fully in
    // memory, so the 44-byte header carries real sizes (no placeholder
    // patching needed).
    const uint32_t dataSizeBytes = static_cast<uint32_t> (numSamples)
                                 * static_cast<uint32_t> (totalChannels) * 3u;
    const uint32_t bytesPerFrame = static_cast<uint32_t> (totalChannels) * 3u;

    // Delete any existing file first: FileOutputStream opens an existing
    // file in append mode and does NOT truncate it (mirror CaptureBuffer::flush).
    wavPath.deleteFile();

    // Buffer size 0 → unbuffered writes (mirror CaptureBuffer::flush).
    juce::FileOutputStream stream (wavPath, 0);
    if (stream.failedToOpen())
    {
        CRASH_LOG_WARN ("WavExporter", "failed to open " + wavPath.getFullPathName());
        return false;
    }

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

    if (! stream.write (header, sizeof (header)))
    {
        CRASH_LOG_WARN ("WavExporter", "failed to write header to " + wavPath.getFullPathName());
        return false;
    }

    // Interleave [dry ch0..N-1, wet ch0..N-1, dry ch0..N-1] into a PCM byte
    // buffer, then write the whole file in one pass. 24-bit quantization is
    // identical to CaptureBuffer::flush: sample → int32 clamped to
    // [-8388607, 8388607] (jlimit(-1, 1) × 8388607), little-endian (low, mid,
    // high byte).
    std::vector<uint8_t> pcmBytes (static_cast<size_t> (numSamples)
                                 * static_cast<size_t> (totalChannels) * 3u);
    size_t offset = 0;

    for (int s = 0; s < numSamples; ++s)
    {
        for (int ch = 0; ch < totalChannels; ++ch)
        {
            float sample;
            if (ch < numChannels)
                sample = dry.getSample (ch, s);
            else if (ch < 2 * numChannels)
                sample = wet.getSample (ch - numChannels, s);
            else
                sample = dry.getSample (ch - 2 * numChannels, s);   // bypass = dry copy (v1)

            const int32_t value = static_cast<int32_t> (juce::jlimit (-1.0f, 1.0f, sample) * 8388607.0f);
            pcmBytes[offset++] = static_cast<uint8_t> (value & 0xFF);
            pcmBytes[offset++] = static_cast<uint8_t> ((value >> 8) & 0xFF);
            pcmBytes[offset++] = static_cast<uint8_t> ((value >> 16) & 0xFF);
        }
    }

    if (! stream.write (pcmBytes.data(), pcmBytes.size()))
    {
        CRASH_LOG_WARN ("WavExporter", "failed to write audio data to " + wavPath.getFullPathName());
        return false;
    }

    return true;
}

} // namespace WavExporter
