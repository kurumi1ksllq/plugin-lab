#pragma once

#include <JuceHeader.h>
#include <atomic>
#include "../signal/SignalGenerator.h"
#include "AudioBuffer.h"

/**
 * SweepRunner — coordinates signal generation, plugin processing,
 * and audio capture for a single measurement pass.
 *
 * Flow:
 *   prepare(sampleRate, blockSize) → setGenerator(sweep) → setPlugin(instance)
 *   → run() [blocking] → getResult()
 */
class CaptureBuffer;  // forward declaration

class SweepRunner
{
public:
    SweepRunner() = default;
    ~SweepRunner() = default;

    //==============================================================================
    /** Prepare the runner for a measurement session. */
    void prepare (double sampleRate, int blockSize);

    /** Set the signal generator to use. */
    void setGenerator (SignalGenerator* generator);

    /** Set the plugin instance to process through. */
    void setPlugin (juce::AudioPluginInstance* plugin);

    /** Register a callback for progress updates (0.0 - 1.0). */
    void setProgressCallback (std::function<void(float)> callback);

    /** Set how many silent samples to append after the signal ends.
     *  The silence is still routed through the plugin, so plugin tails
     *  (reverb, delays, compressors releasing) are captured in the wet
     *  buffer. Default: 0 (no padding).
     */
    void setTailPadSamples (int samples) { tailPadSamples = samples; }

    /** Register a per-block callback, invoked after every processed block
     *  (generator blocks and tail-pad blocks alike) with the total progress
     *  at the end of that block (0.0 - 1.0, including the tail) and the
     *  dry/wet block contents that were just appended to the result.
     *  Default: empty (no callback).
     */
    void setBlockCallback (std::function<void(float progress,
                                              const juce::AudioBuffer<float>& dryBlock,
                                              const juce::AudioBuffer<float>& wetBlock)> callback)
    {
        blockCallback = std::move (callback);
    }

    //==============================================================================
    /** Run the full measurement. Blocks until complete. */
    bool run();

    /** Get the recorded audio result. */
    CaptureBuffer& getResult() { return result; }
    const CaptureBuffer& getResult() const { return result; }

    /** Returns true if currently running. */
    bool isRunning() const { return running; }

    /** Request cancellation (called from another thread). */
    void cancel() { cancelled = true; }

private:
    double sampleRate = 48000.0;
    int blockSize = 512;

    SignalGenerator* generator = nullptr;
    juce::AudioPluginInstance* plugin = nullptr;

    CaptureBuffer result;
    std::function<void(float)> progressCallback;
    std::function<void(float, const juce::AudioBuffer<float>&,
                       const juce::AudioBuffer<float>&)> blockCallback;

    int tailPadSamples = 0;

    bool running = false;
    std::atomic<bool> cancelled { false };
    bool pluginPrepared = false;  // tracks our own prepareToPlay/releaseResources

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SweepRunner)
};
