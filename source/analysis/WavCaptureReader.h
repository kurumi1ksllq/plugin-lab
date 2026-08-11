// WavCaptureReader.h (namespace-style deep module; static functions)
#pragma once
#include <JuceHeader.h>

/**
 * Hand-written 24-bit interleaved WAV reader for the out-of-process capture
 * mirror files (ADR-D-5 / D2b): a single file holding
 * [dry ch0..N-1, wet ch0..N-1] interleaved channels, produced by
 * CaptureBuffer::flush (AudioBuffer.cpp) and WavExporter::exportTracks.
 */
namespace WavCaptureReader
{
    /** Read a 2*numChannels-channel 24-bit PCM WAV (layout
     *  [dry ch0..N-1, wet ch0..N-1]) and split it into dry/wet float buffers.
     *  @param wavPath     the WAV file (written by the child process's
     *                     CaptureBuffer flush mirror)
     *  @param numChannels the plugin's channel count (the file must carry
     *                     exactly 2*numChannels interleaved channels)
     *  @param dry/wet     output buffers (left empty on failure)
     *  @return true on success. Any error (missing file, bad RIFF/WAVE magic,
     *          channel mismatch, bit depth != 24, truncated data) logs
     *          CRASH_LOG_WARN and returns false — never throws. */
    bool readDryWet (const juce::File& wavPath,
                     int numChannels,
                     juce::AudioBuffer<float>& dry,
                     juce::AudioBuffer<float>& wet);
}
