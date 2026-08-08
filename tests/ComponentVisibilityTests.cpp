/**
 * JUCE Component-visibility contract pin.
 *
 * juce::Component::addAndMakeVisible() internally calls setVisible(true)
 * (juce_Component.cpp), so a component that must start hidden has to be
 * hidden AFTER it is added — as MainContentComponent now does for grPlot
 * (source/Main.cpp). MainContentComponent itself is excluded from the test
 * build by design (tests/AGENTS.md), so this file pins the JUCE contract the
 * constructor relies on, keeping the call order from silently regressing.
 */
#include <catch2/catch_test_macros.hpp>

#include <JuceHeader.h>

//==============================================================================
TEST_CASE ("component visibility: addAndMakeVisible forces visible")
{
    // Component construction requires the message thread (jassert).
    juce::MessageManager::getInstance();

    juce::Component parent;
    juce::Component child;

    SECTION ("hiding before addAndMakeVisible is defeated")
    {
        // JUCE contract: addAndMakeVisible() re-shows the child.
        child.setVisible (false);
        parent.addAndMakeVisible (child);

        CHECK (child.isVisible());
    }

    SECTION ("hiding after addAndMakeVisible sticks")
    {
        // The order MainContentComponent uses for grPlot.
        parent.addAndMakeVisible (child);
        child.setVisible (false);

        CHECK (! child.isVisible());
    }
}
