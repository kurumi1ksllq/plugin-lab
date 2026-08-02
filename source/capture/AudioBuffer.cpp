#include "AudioBuffer.h"

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
    dryBuffer.clear();
    wetBuffer.clear();
    totalSamples = 0;
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
}

void CaptureBuffer::trim()
{
    dryBuffer.setSize (numChannels, static_cast<int> (totalSamples), true, false, false);
    wetBuffer.setSize (numChannels, static_cast<int> (totalSamples), true, false, false);
}
