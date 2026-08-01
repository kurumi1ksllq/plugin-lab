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

    bool running = false;
    std::atomic<bool> cancelled { false };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SweepRunner)
};
