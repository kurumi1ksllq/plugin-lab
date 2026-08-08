/**
 * Stub implementations of CrashLog so that PluginManager.cpp can be compiled
 * into the unit test target without pulling in the GUI/UDP dependencies.
 *
 * CrashLog is a RECORDING stub: entries are captured (thread-safe) so tests
 * can assert that error paths logged an exception (block C task 1). The
 * test helpers clearCrashLog() / crashLogErrorCount() / crashLogContains()
 * are declared here and consumed via extern in test files.
 *
 * EditorCrashGuard is NOT stubbed here anymore: the real implementation
 * (EditorCrashGuard.cpp) is compiled into the test target (block C task 2).
 */

#include <JuceHeader.h>
#include <mutex>
#include "../source/utils/CrashLog.h"

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
