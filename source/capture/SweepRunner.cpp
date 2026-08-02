#include "SweepRunner.h"
#include "../utils/CrashLog.h"

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

    // Prepare generator.
    generator->prepare (sampleRate, blockSize);
    generator->reset();

    // Call prepareToPlay on THIS thread (IPC thread) so that it and all
    // subsequent processBlock calls share the same thread — required by some
    // VST3 plugins that track thread affinity (e.g. FabFilter Pro-Q 4).
    // (prepareToPlay was intentionally removed from PluginManager::loadPlugin
    // because that ran on a different ThreadPool thread.)
    if (pluginPrepared)
        plugin->releaseResources();
    plugin->setNonRealtime (true);
    plugin->prepareToPlay (sampleRate, blockSize);
    pluginPrepared = true;

    CRASH_LOG_INFO ("Sweep start", juce::String (sampleRate) + " Hz, "
        + juce::String (blockSize) + " samples");

    int64_t totalLength = generator->getTotalLength();
    if (totalLength <= 0)
        totalLength = static_cast<int64_t> (sampleRate * 10);  // fallback 10s

    int numInputChannels  = plugin->getTotalNumInputChannels();
    int numOutputChannels = plugin->getTotalNumOutputChannels();
    juce::AudioBuffer<float> dryBlock (numInputChannels, blockSize);
    juce::AudioBuffer<float> wetBlock (numOutputChannels, blockSize);
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

        // The measurement runs on the message thread (Pro-Q 4 thread-affinity
        // requirement); yield the message loop 2 ms per block so the UI stays
        // responsive (dragging, repaints, button clicks) during the sweep.
        // Never triggered on other threads (e.g. unit tests on std::thread).
        if (juce::MessageManager::getInstance()->isThisTheMessageThread())
        {
            juce::MessageManager::getInstance()->runDispatchLoopUntil (2);
        }

        // Report progress
        if (progressCallback && (samplesGenerated % reportInterval == 0))
        {
            float progress = static_cast<float> (samplesGenerated)
                           / static_cast<float> (totalLength);
            progressCallback (progress);
        }
    }

    CRASH_LOG_INFO ("Sweep done", juce::String (samplesGenerated) + " samples generated");
    result.trim();  // shrink to recorded length so analyzers see real data
    if (pluginPrepared)
    {
        plugin->releaseResources();
        pluginPrepared = false;
    }
    plugin->setNonRealtime (false);
    running = false;

    return ! cancelled;
}
