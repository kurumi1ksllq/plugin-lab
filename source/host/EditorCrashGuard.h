#pragma once

#include <JuceHeader.h>
#include <functional>

//==============================================================================
/**
 * Crash-guarded editor lifecycle operations.
 *
 * Every function here touches plugin-DLL code paths (createEditorAndMakeActive,
 * view->attached/removed via the VST3 wrapper destructor) that can throw or
 * raise hardware exceptions. This TU is compiled with /EHa so that catch (...)
 * also catches SEH faults, keeping a misbehaving plugin from taking down the
 * host. Call these from the message thread only.
 */
namespace EditorCrashGuard
{
    /** Creates and activates the plugin editor. Returns nullptr on failure. */
    juce::AudioProcessorEditor* createEditor (juce::AudioPluginInstance* plugin);

    /** Destroys an editor: removes it from its parent, notifies the processor,
        then deletes it. Safe to call with a null editor (no-op). */
    void deleteEditor (juce::AudioPluginInstance* plugin, juce::AudioProcessorEditor* editor);
}
