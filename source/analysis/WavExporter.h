// WavExporter.h (namespace-style deep module; static functions)
#pragma once
#include <JuceHeader.h>

namespace WavExporter
{
    /** Export dry/wet/bypass tracks to a single 24-bit PCM WAV.
     *  Layout: 3 * numChannels interleaved channels
     *  [dry ch0..N-1, wet ch0..N-1, dry ch0..N-1] (bypass = dry copy, v1).
     *  numChannels = dry.getNumChannels(). Returns false on any I/O failure
     *  (file create / write); logs CRASH_LOG_WARN. */
    bool exportTracks (const juce::AudioBuffer<float>& dry,
                       const juce::AudioBuffer<float>& wet,
                       double sampleRate,
                       const juce::File& wavPath);
}
