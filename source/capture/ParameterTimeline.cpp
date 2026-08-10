#include "ParameterTimeline.h"

#include <algorithm>
#include <cmath>

juce::AudioProcessorParameter* ParameterTimeline::findParam (juce::AudioProcessor& processor,
                                                             const juce::String& paramId)
{
    // Hosted parameters expose a stable id that survives display-name
    // changes; anything else is not a match. Deliberately mirrors
    // CommandParser's findParamByStableId (the 6-line lookup is replicated,
    // not shared, to keep capture/ free of ipc/ dependencies).
    for (auto* candidate : processor.getParameters())
    {
        auto* hosted = dynamic_cast<juce::HostedAudioProcessorParameter*> (candidate);
        if (hosted != nullptr && hosted->getParameterID() == paramId)
            return candidate;
    }
    return nullptr;
}

ParameterTimeline::~ParameterTimeline()
{
    // RAII: never leave a dangling listener on the plugin (e.g. when the
    // owning CommandParser is destroyed mid-recording). Detach OUTSIDE the
    // mutex: the notifier thread holds JUCE's listener lock while invoking
    // our callback, which blocks on recMutex — removing under the lock
    // would deadlock the two threads.
    juce::AudioProcessor* toDetach = nullptr;
    {
        std::lock_guard<std::mutex> lock (recMutex);
        toDetach = recordedPlugin;
        recordedPlugin = nullptr;
    }
    if (toDetach != nullptr)
        toDetach->removeListener (this);
}

void ParameterTimeline::startRecording (juce::AudioPluginInstance* plugin)
{
    if (plugin == nullptr || isRecording())
        return;   // double-start guard: silently ignore

    {
        std::lock_guard<std::mutex> lock (recMutex);
        recordedEvents.clear();
        recordedPlugin = plugin;
    }
    // Timestamp BEFORE attaching so a notification that fires immediately
    // still gets a valid timeMs.
    recordStartMs.store (static_cast<int64_t> (juce::Time::getMillisecondCounter()));
    plugin->addListener (this);
}

std::vector<TimelineEvent> ParameterTimeline::stopRecording()
{
    juce::AudioProcessor* toDetach = nullptr;
    {
        std::lock_guard<std::mutex> lock (recMutex);
        toDetach = recordedPlugin;
    }

    // Detach BEFORE copying the events (and never under recMutex — see the
    // destructor comment): removeListener blocks until any in-flight
    // notification loop finishes, so every event that fired before
    // stopRecording() was called has already been queued.
    if (toDetach != nullptr)
        toDetach->removeListener (this);

    std::vector<TimelineEvent> events;
    {
        std::lock_guard<std::mutex> lock (recMutex);
        recordedPlugin = nullptr;   // stale notifications from here are dropped
        events = recordedEvents;
        recordedEvents.clear();
    }

    // Stable sort: events stay in per-parameter arrival order within the
    // same millisecond.
    std::stable_sort (events.begin(), events.end(),
                      [] (const TimelineEvent& a, const TimelineEvent& b)
                      { return a.timeMs < b.timeMs; });
    return events;
}

bool ParameterTimeline::isRecording() const
{
    std::lock_guard<std::mutex> lock (recMutex);
    return recordedPlugin != nullptr;
}

void ParameterTimeline::setPlayback (std::vector<TimelineEvent> events, double playbackRate)
{
    playbackEvents.clear();
    playbackEvents.reserve (events.size());

    for (const auto& ev : events)
    {
        TimelineEvent scaled = ev;
        // Pre-apply the playback rate: effectiveMs = timeMs / rate, so the
        // measurement-thread caller just compares wall-clock elapsed ms.
        if (playbackRate > 0.0)
            scaled.timeMs = static_cast<int64_t> (std::round (static_cast<double> (ev.timeMs) / playbackRate));
        playbackEvents.push_back (scaled);
    }

    std::stable_sort (playbackEvents.begin(), playbackEvents.end(),
                      [] (const TimelineEvent& a, const TimelineEvent& b)
                      { return a.timeMs < b.timeMs; });
    playbackCursor = 0;
}

int ParameterTimeline::applyEventsUpTo (int64_t nowMs, juce::AudioPluginInstance* plugin)
{
    int applied = 0;

    // Events whose (rate-scaled) timestamp has been reached. Events for
    // parameters that no longer exist are skipped (not counted) — the cursor
    // still advances so a missing parameter cannot stall the timeline.
    while (playbackCursor < playbackEvents.size()
           && playbackEvents[playbackCursor].timeMs <= nowMs)
    {
        const auto& ev = playbackEvents[playbackCursor];
        if (plugin != nullptr)
        {
            if (auto* param = findParam (*plugin, ev.paramId))
            {
                param->setValueNotifyingHost (ev.valueNormalized);
                ++applied;
            }
        }
        ++playbackCursor;
    }
    return applied;
}

void ParameterTimeline::audioProcessorParameterChanged (juce::AudioProcessor* processor,
                                                        int parameterIndex, float newValue)
{
    if (processor == nullptr
        || parameterIndex < 0
        || parameterIndex >= processor->getParameters().size())
        return;

    auto* param = processor->getParameters().getUnchecked (parameterIndex);
    auto* hosted = dynamic_cast<juce::HostedAudioProcessorParameter*> (param);
    if (hosted == nullptr || hosted->getParameterID().isEmpty())
        return;   // R9: only record hosted params with a stable id

    const auto rawNow   = static_cast<uint32_t> (juce::Time::getMillisecondCounter());
    const auto rawStart = static_cast<uint32_t> (recordStartMs.load());
    const int64_t timeMs = static_cast<int64_t> (rawNow - rawStart);

    // C8: the callback may fire on ANY thread (setParam runs on the IPC
    // thread); queue under the mutex and never touch plugin APIs here.
    const std::lock_guard<std::mutex> lock (recMutex);
    if (recordedPlugin == nullptr)
        return;   // a stale notification racing stopRecording: drop it
    recordedEvents.push_back ({ timeMs, hosted->getParameterID(), newValue });
}
