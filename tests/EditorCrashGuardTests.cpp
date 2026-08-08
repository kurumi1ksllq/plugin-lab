/**
 * EditorCrashGuard unit tests (block C task 2).
 *
 * EditorCrashGuard (an /EHa TU) wraps plugin-DLL editor create/delete calls
 * in catch(...) so a crashing plugin cannot take down the host. These tests
 * compile the REAL EditorCrashGuard.cpp into the unit test target (the
 * previous no-op stubs are removed in tests/CommandParserStubs.cpp) and
 * verify:
 *   - createEditor returns null for an editorless plugin (no crash)
 *   - createEditor catches a plugin exception and returns null
 *   - deleteEditor tolerates a null editor
 *   - deleteEditor notifies the processor (editorBeingDeleted) before delete
 *   - deleteEditor catches a hardware fault (SEH) from the editor's
 *     destructor — the plugin-DLL crash path /EHa protects against
 */
#include <catch2/catch_test_macros.hpp>

#include "TestPlugin.h"
#include "../source/host/EditorCrashGuard.h"

// CrashLog recording-stub helpers (defined in CommandParserStubs.cpp)
extern void clearCrashLog();
extern int crashLogErrorCount();
extern bool crashLogContains (const juce::String& substr);

namespace
{
    /** Minimal concrete editor (AudioProcessorEditor's constructor is
     *  protected, so tests need a subclass). */
    class TestEditor final : public juce::AudioProcessorEditor
    {
    public:
        explicit TestEditor (juce::AudioProcessor& p) : AudioProcessorEditor (&p) {}
        ~TestEditor() override = default;
        void paint (juce::Graphics&) override {}
    };

    /** Minimal concrete editor whose destructor dereferences a null pointer
     *  (a hardware fault, like a crashing plugin's view teardown). */
    class CrashOnDestructEditor final : public juce::AudioProcessorEditor
    {
    public:
        explicit CrashOnDestructEditor (juce::AudioProcessor& p) : AudioProcessorEditor (&p) {}
        ~CrashOnDestructEditor() override
        {
            volatile int* nullPtr = nullptr;
            *nullPtr = 42;   // deliberate access violation (SEH)
        }
        void paint (juce::Graphics&) override {}
    };
}

TEST_CASE ("EditorCrashGuard: createEditor returns null for an editorless plugin",
           "[editorcrashguard]")
{
    // Arrange
    TestPlugin plugin;

    // Act
    auto* editor = EditorCrashGuard::createEditor (&plugin);

    // Assert — no editor, no crash
    REQUIRE (editor == nullptr);
}

TEST_CASE ("EditorCrashGuard: createEditor catches a throwing plugin and returns null",
           "[editorcrashguard][exception]")
{
    // Arrange
    TestPlugin plugin;
    plugin.setThrowOnCreateEditor (true);
    clearCrashLog();

    // Act — must not escape the guard
    auto* editor = EditorCrashGuard::createEditor (&plugin);

    // Assert — null editor, exception recorded
    REQUIRE (editor == nullptr);
    REQUIRE (crashLogErrorCount() >= 1);
    REQUIRE (crashLogContains ("Editor crash"));
}

TEST_CASE ("EditorCrashGuard: deleteEditor tolerates a null editor",
           "[editorcrashguard]")
{
    // Arrange
    TestPlugin plugin;

    // Act — must be a no-op
    REQUIRE_NOTHROW (EditorCrashGuard::deleteEditor (&plugin, nullptr));
}

TEST_CASE ("EditorCrashGuard: deleteEditor notifies the processor before deleting",
           "[editorcrashguard]")
{
    // Arrange — a real editor, active on the processor
    TestPlugin plugin;
    auto* editor = new TestEditor (plugin);
    plugin.setEditorToReturn (editor);
    REQUIRE (EditorCrashGuard::createEditor (&plugin) == editor);   // becomes active

    // Act
    EditorCrashGuard::deleteEditor (&plugin, editor);

    // Assert — editorBeingDeleted ran (active editor cleared) before delete
    REQUIRE (plugin.getActiveEditor() == nullptr);
}

TEST_CASE ("EditorCrashGuard: deleteEditor survives a hardware fault in editor teardown",
           "[editorcrashguard][seh]")
{
    // Arrange — the editor's destructor raises an access violation, as a
    // crashing plugin's view teardown would. /EHa catch(...) must intercept it.
    TestPlugin plugin;
    auto* editor = new CrashOnDestructEditor (plugin);
    plugin.setEditorToReturn (editor);
    clearCrashLog();

    // Act — must not escape the guard, must not terminate the process
    REQUIRE_NOTHROW (EditorCrashGuard::deleteEditor (&plugin, editor));

    // Assert — fault recorded
    REQUIRE (crashLogErrorCount() >= 1);
    REQUIRE (crashLogContains ("Editor delete crash"));
}
