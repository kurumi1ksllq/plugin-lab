#pragma once

#include <JuceHeader.h>
#include <functional>

/**
 * Standalone fixed-size plugin editor window.
 *
 * Wraps a JUCE DocumentWindow with a plugin editor as content.
 * Uses native titlebar so that getContentComponentBorder() returns {} —
 * the window size exactly matches the editor's native size.
 *
 * Lifetime: owns the AudioPluginInstance and the editor.
 * On close, invokes the onWindowClosed callback.
 * Must be created and destroyed on the message thread.
 */
class PluginEditorWindow final : public juce::DocumentWindow
{
public:
    /** Construct and show the editor window.
     *
     *  @param instance       Plugin instance (ownership transferred)
     *  @param editor         Plugin editor (ownership transferred to setContentOwned)
     *  @param pluginName     Window title
     *  @param onWindowClosed Callback invoked when the close button is pressed
     */
    PluginEditorWindow (std::unique_ptr<juce::AudioPluginInstance> instance,
                        juce::AudioProcessorEditor* editor,
                        const juce::String& pluginName,
                        std::function<void()> onWindowClosed);

    /** Destructor.
     *
     *  Explicitly calls clearContentComponent() to delete the editor (and
     *  therefore the VST3 wrapper, which calls editorBeingDeleted + view->removed)
     *  BEFORE the pluginInstance member is destroyed.
     *
     *  Must be called on the message thread.
     */
    ~PluginEditorWindow() override;

    void closeButtonPressed() override;

    juce::AudioPluginInstance* getPluginInstance() const noexcept;

private:
    std::unique_ptr<juce::AudioPluginInstance> pluginInstance;
    std::function<void()> onWindowClosed;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginEditorWindow)
};
