#include "PluginEditorWindow.h"
#include "../utils/CrashLog.h"

PluginEditorWindow::PluginEditorWindow (
    std::unique_ptr<juce::AudioPluginInstance> instance,
    juce::AudioProcessorEditor* editor,
    const juce::String& pluginName,
    std::function<void()> onWindowClosedCb)
    : DocumentWindow (pluginName,
                      juce::Desktop::getInstance().getDefaultLookAndFeel()
                          .findColour (juce::ResizableWindow::backgroundColourId),
                      DocumentWindow::allButtons)
{
    jassert (juce::MessageManager::existsAndIsCurrentThread());

    // Native titlebar → getContentComponentBorder() returns {},
    // so the window size equals the content (editor) size exactly.
    setUsingNativeTitleBar (true);

    // resizeToFitWhenContentChangesSize=true → childBoundsChanged
    // automatically resizes the window to content+border. With native
    // titlebar the border is 0, so window == editor native size.
    setContentOwned (editor, true);

    setResizable (false, false);

    pluginInstance = std::move (instance);
    this->onWindowClosed = std::move (onWindowClosedCb);

    centreWithSize (getWidth(), getHeight());
    setVisible (true);

    CRASH_LOG_INFO ("Editor window created",
                    pluginName + " "
                    + juce::String (getWidth()) + "x" + juce::String (getHeight()));
}

PluginEditorWindow::~PluginEditorWindow()
{
    jassert (juce::MessageManager::existsAndIsCurrentThread());

    // Must explicitly clear content before ~DocumentWindow/~ResizableWindow,
    // because ~ResizableWindow deletes content AFTER derived members are
    // destroyed. If the editor outlives pluginInstance, VST3 destruction
    // may access a dead instance pointer (UB).
    clearContentComponent();
}

void PluginEditorWindow::closeButtonPressed()
{
    if (onWindowClosed)
        onWindowClosed();
    // Do NOT call systemRequestedQuit — Main owns the unique_ptr and
    // will reset it via onWindowClosed.
}

juce::AudioPluginInstance* PluginEditorWindow::getPluginInstance() const noexcept
{
    return pluginInstance.get();
}
