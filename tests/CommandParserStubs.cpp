/**
 * Stub implementations of CrashLog and EditorCrashGuard so that
 * PluginManager.cpp can be compiled into the unit test target without
 * pulling in the GUI/UDP dependencies.
 *
 * CrashLog is a RECORDING stub: entries are captured (thread-safe) so tests
 * can assert that error paths logged an exception (block C task 1). The
 * test helpers clearCrashLog() / crashLogErrorCount() / crashLogContains()
 * are declared here and consumed via extern in test files.
 *
 * EditorCrashGuard stubs are no-ops — tests use the real implementation
 * (EditorCrashGuard.cpp, task 2) or never exercise editor creation.
 */

#include <JuceHeader.h>
#include <mutex>
#include "../source/utils/CrashLog.h"
#include "../source/host/EditorCrashGuard.h"

//==============================================================================
// CrashLog recording stub
namespace
{
    std::mutex gCrashLogMutex;
    juce::StringArray gCrashLogErrors;
}

void CrashLog::write (CrashLog::Level level,
                      const juce::String& operation,
                      const juce::String& detail,
                      const juce::String&,
                      int)
{
    std::lock_guard<std::mutex> lock (gCrashLogMutex);
    if (level == CrashLog::Error)
        gCrashLogErrors.add (operation + (detail.isNotEmpty() ? " | " + detail : juce::String()));
}

//==============================================================================
// Test helpers (declared extern in test files)
void clearCrashLog()
{
    std::lock_guard<std::mutex> lock (gCrashLogMutex);
    gCrashLogErrors.clear();
}

int crashLogErrorCount()
{
    std::lock_guard<std::mutex> lock (gCrashLogMutex);
    return gCrashLogErrors.size();
}

bool crashLogContains (const juce::String& substr)
{
    std::lock_guard<std::mutex> lock (gCrashLogMutex);
    for (const auto& entry : gCrashLogErrors)
        if (entry.contains (substr))
            return true;
    return false;
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
