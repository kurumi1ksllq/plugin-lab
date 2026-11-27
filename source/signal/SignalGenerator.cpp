#include "SignalGenerator.h"

void SignalGenerator::prepare (double sr, int bs)
{
    sampleRate = sr;
    blockSize = bs;
    reset();
}

void SignalGenerator::writeSample (juce::AudioBuffer<float>& buffer, int index, float sample) const
{
    const int numChannels = buffer.getNumChannels();

    for (int ch = 0; ch < numChannels; ++ch)
    {
        // side: the right channel carries the inverted polarity (L = -R).
        const float value = (channelMode == ChannelMode::side && ch == 1) ? -sample : sample;
        buffer.getWritePointer (ch)[index] = value;
    }
}
