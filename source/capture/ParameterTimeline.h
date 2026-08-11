#pragma once
#include <JuceHeader.h>
#include <atomic>
#include <mutex>
#include <vector>

struct TimelineEvent
{
    int64_t timeMs = 0;           // ms since record start (or effective playback ms)
    juce::String paramId;         // stable id (matches getParams)
    float valueNormalized = 0.0f; // 0..1
};

/** Records and replays parameter automation.
 *  Recording: attach as AudioProcessorListener; every parameter change
 *  (setValueNotifyingHost, from any thread) is stamped with the wall-clock
 *  ms since startRecording and queued under a mutex (C8).
 *  Playback: rate pre-applied (effectiveMs = timeMs / rate); applyEventsUpTo
 *  is called from the measurement thread between blocks. */
class ParameterTimeline final : public juce::AudioProcessorListener
{
public:
    ParameterTimeline() = default;
    ~ParameterTimeline() override;   // RAII: removeListener if attached

    // ---- recording ----
    void startRecording (juce::AudioPluginInstance* plugin);
    std::vector<TimelineEvent> stopRecording();   // detach + return events (sorted by timeMs)
    bool isRecording() const;

    // ---- playback ----
    void setPlayback (std::vector<TimelineEvent> events, double playbackRate);
    int applyEventsUpTo (int64_t nowMs, juce::AudioPluginInstance* plugin);

    /** Finds the hosted parameter whose stable id matches (or nullptr).
     *  Shared by playback (applyEventsUpTo) and the R2 restore path in
     *  MeasurementSession — one lookup, no ipc/ dependency. */
    static juce::AudioProcessorParameter* findParam (juce::AudioProcessor& processor,
                                                     const juce::String& paramId);

    /** The playback events after rate pre-application (sorted by timeMs).
     *  Read-only view used by the caller to snapshot the affected params. */
    const std::vector<TimelineEvent>& getEvents() const { return playbackEvents; }

    /** Playback progress (issue #2): how many events have been applied so
     *  far. Read on the measurement (message) thread only — the same thread
     *  that advances the cursor via applyEventsUpTo — so no atomic is needed.
     *  The event index displayed by the GUI / pushed over IPC. */
    size_t getPlaybackCursor() const { return playbackCursor; }

    /** Total playback events after rate pre-application (progress denominator). */
    size_t getPlaybackEventCount() const { return playbackEvents.size(); }

    // ---- AudioProcessorListener (both pure virtuals) ----
    void audioProcessorParameterChanged (juce::AudioProcessor* processor, int parameterIndex, float newValue) override;
    void audioProcessorChanged (juce::AudioProcessor*, const juce::AudioProcessorListener::ChangeDetails&) override {}

private:
    // param_id resolution: processor->getParameters()[index] →
    // dynamic_cast<juce::HostedAudioProcessorParameter*> → getParameterID();
    // empty id → skip (R9).
    mutable std::mutex recMutex;
    juce::AudioProcessor* recordedPlugin = nullptr;
    std::atomic<int64_t> recordStartMs { 0 };
    std::vector<TimelineEvent> recordedEvents;

    std::vector<TimelineEvent> playbackEvents;   // rate pre-applied, sorted
    size_t playbackCursor = 0;
    // R2 snapshot/restore is handled by the caller (MeasurementSession): it
    // snapshots getEvents()'s param ids at setPlayback time and restores the
    // values after the play run.

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ParameterTimeline)
};
