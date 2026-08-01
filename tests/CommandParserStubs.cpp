/**
 * Stub implementations of CrashLog and EditorCrashGuard so that
 * PluginManager.cpp can be compiled into the unit test target without
 * pulling in the GUI/UDP dependencies.
 *
 * These are intentionally no-ops — the tests never exercise code paths
 * that depend on crash logging or editor creation.
 */

#include <JuceHeader.h>
#include "../source/utils/CrashLog.h"
#include "../source/host/EditorCrashGuard.h"

//==============================================================================
// CrashLog stub
void CrashLog::write (CrashLog::Level,
                      const juce::String&,
                      const juce::String&,
                      const juce::String&,
                      int)
{
    // no-op: tests don't need crash logging
}

//==============================================================================
// EditorCrashGuard stubs
namespace EditorCrashGuard
{
    juce::AudioProcessorEditor* createEditor (juce::AudioPluginInstance*)
    {
        return nullptr;  // tests don't create plugin editors
    }

    void deleteEditor (juce::AudioPluginInstance*, juce::AudioProcessorEditor*)
    {
        // no-op
    }
}  // namespace EditorCrashGuard
