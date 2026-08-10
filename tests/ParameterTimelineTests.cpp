// ParameterTimeline unit tests (TDD: RED phase).
//
// Block B task 2: parameter-automation recording (recordTimeline) and
// playback (playTimeline). The timeline captures every parameter change
// fired through the JUCE AudioProcessorListener chain and replays them
// during a measurement run.
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "TestPlugin.h"
#include "../source/capture/ParameterTimeline.h"

TEST_CASE ("ParameterTimeline: recording captures param changes with ids", "[paramtimeline][record]")
{
    TestPlugin plugin;
    auto* drive = plugin.addTestParameter ("drive", "Drive", 0.0f);
    auto* mix   = plugin.addTestParameter ("mix", "Mix", 0.5f);

    ParameterTimeline tl;
    tl.startRecording (&plugin);

    drive->setValueNotifyingHost (0.7f);
    juce::Thread::sleep (30);
    mix->setValueNotifyingHost (0.2f);

    auto events = tl.stopRecording();

    REQUIRE (events.size() == 2);
    REQUIRE (events[0].paramId == "drive");
    REQUIRE (events[0].valueNormalized == Catch::Approx (0.7f));
    REQUIRE (events[1].paramId == "mix");
    REQUIRE (events[1].valueNormalized == Catch::Approx (0.2f));
    REQUIRE (events[1].timeMs >= events[0].timeMs);   // monotonic
}

TEST_CASE ("ParameterTimeline: playback applies events up to a timestamp", "[paramtimeline][playback]")
{
    TestPlugin plugin;
    auto* drive = plugin.addTestParameter ("drive", "Drive", 0.0f);

    ParameterTimeline tl;
    tl.setPlayback ({{ 0, "drive", 0.3f }, { 500, "drive", 0.9f }}, 1.0);

    REQUIRE (tl.applyEventsUpTo (100, &plugin) == 1);
    REQUIRE (drive->getValue() == Catch::Approx (0.3f));
    REQUIRE (tl.applyEventsUpTo (600, &plugin) == 1);
    REQUIRE (drive->getValue() == Catch::Approx (0.9f));
    REQUIRE (tl.applyEventsUpTo (900, &plugin) == 0);   // nothing new
}

TEST_CASE ("ParameterTimeline: playback rate scales timestamps", "[paramtimeline][playback]")
{
    // rate 2.0 -> the event at t=500ms applies when applyEventsUpTo is called
    // with nowMs=250 (effective time = timeMs / rate, pre-applied).
    TestPlugin plugin;
    auto* drive = plugin.addTestParameter ("drive", "Drive", 0.0f);
    ParameterTimeline tl;
    tl.setPlayback ({{ 500, "drive", 0.9f }}, 2.0);
    REQUIRE (tl.applyEventsUpTo (200, &plugin) == 0);
    REQUIRE (tl.applyEventsUpTo (250, &plugin) == 1);
    REQUIRE (drive->getValue() == Catch::Approx (0.9f));
}
