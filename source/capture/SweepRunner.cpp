#include "SweepRunner.h"

void SweepRunner::prepare (double sr, int bs)
{
    sampleRate = sr;
    blockSize = bs;
    result.prepare (2, bs);
    result.setSampleRate (sr);
}

void SweepRunner::setGenerator (SignalGenerator* gen)
{
    generator = gen;
}

void SweepRunner::setPlugin (juce::AudioPluginInstance* p)
{
    plugin = p;
}

void SweepRunner::setProgressCallback (std::function<void(float)> callback)
{
    progressCallback = std::move (callback);
}

bool SweepRunner::run()
{
    if (generator == nullptr || plugin == nullptr)
        return false;

    running = true;
    cancelled = false;
    result.clear();

    // Prepare generator and plugin
    generator->prepare (sampleRate, blockSize);
    generator->reset();
    plugin->prepareToPlay (sampleRate, blockSize);

    int64_t totalLength = generator->getTotalLength();
    if (totalLength <= 0)
        totalLength = static_cast<int64_t> (sampleRate * 10);  // fallback 10s

    juce::AudioBuffer<float> dryBlock (2, blockSize);
    juce::AudioBuffer<float> wetBlock (2, blockSize);
    juce::MidiBuffer emptyMidi;

    int64_t samplesGenerated = 0;
    const int reportInterval = std::max (1, static_cast<int> (totalLength / 100));

    while (samplesGenerated < totalLength && ! cancelled)
    {
        int numToGenerate = static_cast<int> (
            std::min<int64_t> (blockSize, totalLength - samplesGenerated));

        // Reset blocks
        dryBlock.clear();
        wetBlock.clear();

        // Generate dry signal
        generator->generate (dryBlock, 0, numToGenerate);

        // Copy dry to wet (plugin processes in-place by default)
        wetBlock.makeCopyOf (dryBlock, true);

        // Process through plugin
        plugin->processBlock (wetBlock, emptyMidi);

        // Record both
        result.append (dryBlock, wetBlock);

        samplesGenerated += numToGenerate;

        // Report progress
        if (progressCallback && (samplesGenerated % reportInterval == 0))
        {
            float progress = static_cast<float> (samplesGenerated)
                           / static_cast<float> (totalLength);
            progressCallback (progress);
        }
    }

    plugin->releaseResources();
    running = false;

    return ! cancelled;
}
