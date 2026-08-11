#include "WavCaptureReader.h"
#include "../utils/CrashLog.h"

#include <cstring>
#include <vector>

namespace
{
//==============================================================================
// Little-endian byte helpers (mirror AudioBuffer.cpp / WavExporter.cpp)

uint16_t readU16LE (const uint8_t* src)
{
    return static_cast<uint16_t> (src[0] | (src[1] << 8));
}

uint32_t readU32LE (const uint8_t* src)
{
    return static_cast<uint32_t> (src[0])
         | (static_cast<uint32_t> (src[1]) << 8)
         | (static_cast<uint32_t> (src[2]) << 16)
         | (static_cast<uint32_t> (src[3]) << 24);
}

/** Sign-extend a 24-bit little-endian sample (3 bytes) to int32. */
int32_t sample24ToInt32 (const uint8_t* src)
{
    int32_t value = static_cast<int32_t> (src[0])
                  | (static_cast<int32_t> (src[1]) << 8)
                  | (static_cast<int32_t> (src[2]) << 16);
    if ((value & 0x00800000) != 0)
        value |= static_cast<int32_t> (0xFF000000);   // sign-extend bit 23
    return value;
}

bool hasMagic (const uint8_t* bytes, const char* magic)
{
    return bytes[0] == static_cast<uint8_t> (magic[0])
        && bytes[1] == static_cast<uint8_t> (magic[1])
        && bytes[2] == static_cast<uint8_t> (magic[2])
        && bytes[3] == static_cast<uint8_t> (magic[3]);
}
} // namespace

namespace WavCaptureReader
{

bool readDryWet (const juce::File& wavPath,
                 int numChannels,
                 juce::AudioBuffer<float>& dry,
                 juce::AudioBuffer<float>& wet)
{
    // Failure contract: the output buffers are always left empty.
    dry.setSize (0, 0);
    wet.setSize (0, 0);

    if (numChannels <= 0)
    {
        CRASH_LOG_WARN ("WavCaptureReader", "invalid channel count " + juce::String (numChannels)
                         + " for " + wavPath.getFullPathName());
        return false;
    }

    juce::FileInputStream stream (wavPath);
    if (stream.failedToOpen())
    {
        CRASH_LOG_WARN ("WavCaptureReader", "failed to open " + wavPath.getFullPathName());
        return false;
    }

    const auto fileSize = static_cast<size_t> (stream.getTotalLength());
    if (fileSize < 44)
    {
        CRASH_LOG_WARN ("WavCaptureReader", wavPath.getFullPathName() + " too small for a WAV header");
        return false;
    }

    std::vector<uint8_t> bytes (fileSize);
    const auto bytesRead = stream.read (bytes.data(), static_cast<int> (bytes.size()));
    if (bytesRead < 0 || static_cast<size_t> (bytesRead) != fileSize)
    {
        CRASH_LOG_WARN ("WavCaptureReader", "short read on " + wavPath.getFullPathName());
        return false;
    }

    if (! hasMagic (bytes.data(), "RIFF") || ! hasMagic (bytes.data() + 8, "WAVE"))
    {
        CRASH_LOG_WARN ("WavCaptureReader", "bad RIFF/WAVE magic in " + wavPath.getFullPathName());
        return false;
    }

    // Walk the RIFF chunks to find fmt + data. The project's own writers emit
    // the plain 44-byte layout (fmt at 12, data at 36), but third-party files
    // may carry extra chunks (e.g. LIST) — a scan keeps the reader robust.
    size_t offset = 12;
    bool foundFmt = false;
    bool foundData = false;
    uint16_t format = 0;
    uint16_t channels = 0;
    uint16_t bitsPerSample = 0;
    uint32_t dataSize = 0;
    size_t dataOffset = 0;

    while (offset + 8 <= bytes.size())
    {
        const uint32_t chunkSize = readU32LE (bytes.data() + offset + 4);
        if (chunkSize > bytes.size())
            break;

        if (std::memcmp (bytes.data() + offset, "fmt ", 4) == 0)
        {
            if (offset + 8 + 16 > bytes.size())
                break;
            format        = readU16LE (bytes.data() + offset + 8);
            channels      = readU16LE (bytes.data() + offset + 8 + 2);
            bitsPerSample = readU16LE (bytes.data() + offset + 8 + 14);
            foundFmt = true;
        }
        else if (std::memcmp (bytes.data() + offset, "data", 4) == 0)
        {
            dataOffset = offset + 8;
            dataSize   = chunkSize;
            foundData  = true;
        }

        offset += 8 + chunkSize;
        if ((chunkSize & 1u) != 0)   // chunks are word-aligned
            ++offset;
    }

    if (! foundFmt || ! foundData)
    {
        CRASH_LOG_WARN ("WavCaptureReader", "missing fmt/data chunk in " + wavPath.getFullPathName());
        return false;
    }
    if (format != 1)
    {
        CRASH_LOG_WARN ("WavCaptureReader", "non-PCM format " + juce::String (format) + " in "
                         + wavPath.getFullPathName());
        return false;
    }
    if (channels != static_cast<uint16_t> (2 * numChannels))
    {
        CRASH_LOG_WARN ("WavCaptureReader", "channel count " + juce::String (channels)
                         + " != 2 * " + juce::String (numChannels) + " in "
                         + wavPath.getFullPathName());
        return false;
    }
    if (bitsPerSample != 24)
    {
        CRASH_LOG_WARN ("WavCaptureReader", "bit depth " + juce::String (bitsPerSample)
                         + " != 24 in " + wavPath.getFullPathName());
        return false;
    }
    if (dataOffset + dataSize > bytes.size())
    {
        CRASH_LOG_WARN ("WavCaptureReader", "truncated data chunk in " + wavPath.getFullPathName());
        return false;
    }

    const size_t bytesPerFrame = static_cast<size_t> (channels) * 3u;
    if (dataSize % bytesPerFrame != 0)
    {
        CRASH_LOG_WARN ("WavCaptureReader", "data size not a multiple of the frame size in "
                         + wavPath.getFullPathName());
        return false;
    }

    const size_t frameCount = dataSize / bytesPerFrame;
    const int totalChannels = 2 * numChannels;
    dry.setSize (numChannels, static_cast<int> (frameCount), false, true, false);
    wet.setSize (numChannels, static_cast<int> (frameCount), false, true, false);

    // 24-bit → float: mirror of the writer's quantization
    // (jlimit(-1,1,sample) * 8388607, AudioBuffer.cpp:173) — clamp the decoded
    // int to the same range, then scale by 1/8388607. The round trip is
    // bit-exact for any sample the writer produced.
    constexpr float kInv24 = 1.0f / 8388607.0f;
    const uint8_t* data = bytes.data() + dataOffset;
    for (size_t s = 0; s < frameCount; ++s)
    {
        const uint8_t* frame = data + s * bytesPerFrame;
        for (int c = 0; c < totalChannels; ++c)
        {
            const int32_t raw = juce::jlimit (-8388607, 8388607,
                                              sample24ToInt32 (frame + c * 3));
            const float sample = static_cast<float> (raw) * kInv24;
            if (c < numChannels)
                dry.setSample (c, static_cast<int> (s), sample);
            else
                wet.setSample (c - numChannels, static_cast<int> (s), sample);
        }
    }
    return true;
}

} // namespace WavCaptureReader
