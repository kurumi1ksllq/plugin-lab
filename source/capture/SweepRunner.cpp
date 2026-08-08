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
    //
    // Exception protection (block C task 1): the measurement path runs
    // synchronously on the message thread with zero prior try/catch — a
    // plugin throwing from prepareToPlay/processBlock escaped into the
    // message loop → std::terminate → abort with no crash log (LA-2A).
    // Every plugin-DLL call below is guarded; this TU is compiled with /EHa
    // so catch (...) also intercepts SEH faults. A failed run returns false
    // (surfaced as an IPC error response), and the host survives.
    try
    {
        if (pluginPrepared)
            plugin->releaseResources();
        plugin->setNonRealtime (true);
        plugin->prepareToPlay (sampleRate, blockSize);
    }
    catch (const std::exception& e)
    {
        CRASH_LOG_ERR ("Sweep prepare", e.what());
        running = false;
        return false;
    }
    catch (...)
    {
        CRASH_LOG_ERR ("Sweep prepare", "unknown exception");
        running = false;
        return false;
    }
    pluginPrepared = true;

    CRASH_LOG_INFO ("Sweep start", juce::String (sampleRate) + " Hz, "
        + juce::String (blockSize) + " samples");

    int64_t totalLength = generator->getTotalLength();
    if (totalLength <= 0)
        totalLength = static_cast<int64_t> (sampleRate * 10);  // fallback 10s

    const int64_t tailTarget = static_cast<int64_t> (juce::jmax (0, tailPadSamples));
    const int64_t totalDoneTarget = totalLength + tailTarget;

    int numInputChannels  = plugin->getTotalNumInputChannels();
    int numOutputChannels = plugin->getTotalNumOutputChannels();
    juce::AudioBuffer<float> dryBlock (numInputChannels, blockSize);
    juce::AudioBuffer<float> wetBlock (numOutputChannels, blockSize);
    juce::MidiBuffer emptyMidi;

    // Guarded plugin processBlock: any C++/SEH exception inside the plugin
    // fails the measurement instead of escaping to the message loop.
    const auto safeProcessBlock = [&] (juce::AudioBuffer<float>& block) -> bool
    {
        try
        {
            plugin->processBlock (block, emptyMidi);
            return true;
        }
        catch (const std::exception& e)
        {
            CRASH_LOG_ERR ("Sweep process", e.what());
            return false;
        }
        catch (...)
        {
            CRASH_LOG_ERR ("Sweep process", "unknown exception");
            return false;
        }
    };

    int64_t samplesGenerated = 0;
    int64_t tailSamples = 0;
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
        if (! safeProcessBlock (wetBlock))
        {
            running = false;
            return false;
        }

        // Record both. Only the actually-generated samples are appended: the
        // final block is often partial, and the plugin's full-block output
        // beyond numToGenerate corresponds to silence-padded input that must
        // not appear in the measurement.
        //
        // Tail pad: the final block's zero-padded region also carries the
        // plugin's response to the last signal samples (its wet tail for
        // latency > 0, or ringing that has not yet decayed). Appending that
        // region as tail-pad samples captures the true tail — otherwise the
        // tail loop below would feed an already-flushed plugin and record
        // silence instead. The dry region appended here is the same silence
        // the tail loop would have produced.
        int numAppend = numToGenerate;
        if (tailSamples < tailTarget)
        {
            const int paddingInBlock = blockSize - numToGenerate;
            const int64_t tailFromBlock = std::min<int64_t> (
                tailTarget - tailSamples, paddingInBlock);
            numAppend += static_cast<int> (tailFromBlock);
            tailSamples += tailFromBlock;
        }
        result.append (dryBlock, wetBlock, numAppend);

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

        // Report the block (dry/wet contents + total progress incl. tail)
        if (blockCallback)
        {
            float progress = static_cast<float> (samplesGenerated + tailSamples)
                           / static_cast<float> (totalDoneTarget);
            blockCallback (progress, dryBlock, wetBlock);
        }
    }

    // Tail pad: feed the remaining silent samples through the plugin so its
    // wet tail (reverb decay, compressor release, ...) is captured. The dry
    // blocks stay silent, so analyzers can distinguish signal from tail.
    while (tailSamples < tailTarget && ! cancelled)
    {
        int numToGenerate = static_cast<int> (
            std::min<int64_t> (blockSize, tailTarget - tailSamples));

        // Silence: cleared blocks, no generator call
        dryBlock.clear();
        wetBlock.clear();
        wetBlock.makeCopyOf (dryBlock, true);

        // Process through plugin
        if (! safeProcessBlock (wetBlock))
        {
            running = false;
            return false;
        }

        result.append (dryBlock, wetBlock, numToGenerate);

        tailSamples += numToGenerate;

        if (juce::MessageManager::getInstance()->isThisTheMessageThread())
        {
            juce::MessageManager::getInstance()->runDispatchLoopUntil (2);
        }

        if (blockCallback)
        {
            float progress = static_cast<float> (samplesGenerated + tailSamples)
                           / static_cast<float> (totalDoneTarget);
            blockCallback (progress, dryBlock, wetBlock);
        }
    }

    CRASH_LOG_INFO ("Sweep done", juce::String (samplesGenerated) + " samples generated");
    result.trim();  // shrink to recorded length so analyzers see real data

    // Teardown is also plugin-DLL code (VST3 releaseResources + setNonRealtime
    // round-trip into the plugin); guard it too so a throwing teardown never
    // escapes the measurement path.
    try
    {
        if (pluginPrepared)
        {
            plugin->releaseResources();
            pluginPrepared = false;
        }
        plugin->setNonRealtime (false);
    }
    catch (const std::exception& e)
    {
        CRASH_LOG_WARN ("Sweep teardown", e.what());
    }
    catch (...)
    {
        CRASH_LOG_WARN ("Sweep teardown", "unknown exception");
    }
    running = false;

    return ! cancelled;
}
