#include "PluginEditorWindow.h"
#include "../host/EditorCrashGuard.h"
#include "../utils/CrashLog.h"

PluginEditorWindow::PluginEditorWindow (
    std::unique_ptr<juce::AudioPluginInstance> instance,
    juce::AudioProcessorEditor* editor,
    const juce::String& pluginName,
    std::function<void()> onWindowClosedCb,
    juce::Rectangle<int> preferredArea)
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
    // The editor is NOT owned by the window: its deletion is routed through
    // EditorCrashGuard::deleteEditor (an /EHa TU) in the destructor so that
    // crashes inside the plugin's view teardown don't kill the host.
    setContentNonOwned (editor, true);

    setResizable (false, false);

    pluginInstance = std::move (instance);
    this->onWindowClosed = std::move (onWindowClosedCb);

    // Place the editor window beside the main window (preferredArea) instead
    // of centring it, so it never covers the main window's right panel.
    // Preferred spot: right of the main window; if it would leave the screen,
    // below it; only then fall back to centring.
    if (preferredArea.isEmpty())
    {
        centreWithSize (getWidth(), getHeight());
    }
    else if (auto* display = juce::Desktop::getInstance().getDisplays()
                                 .getDisplayForRect (preferredArea))
    {
        auto screen = display->userBounds;
        int x = preferredArea.getRight() + 8;
        int y = preferredArea.getY() + 8;
        if (x + getWidth() > screen.getRight())
        {
            x = preferredArea.getX();
            y = preferredArea.getBottom() + 8;
        }
        setBounds (juce::Rectangle<int> (x, y, getWidth(), getHeight()));
    }
    else
    {
        centreWithSize (getWidth(), getHeight());
    }
    setVisible (true);

    CRASH_LOG_INFO ("Editor window created",
                    pluginName + " "
                    + juce::String (getWidth()) + "x" + juce::String (getHeight()));
}

PluginEditorWindow::~PluginEditorWindow()
{
    jassert (juce::MessageManager::existsAndIsCurrentThread());

    // The editor is not owned by the window (setContentNonOwned). Detach it
    // here so ~ResizableWindow doesn't touch it later, then destroy it via
    // the crash-guarded path (editorBeingDeleted + delete) BEFORE the
    // pluginInstance member dies — otherwise VST3 teardown may access a dead
    // instance pointer (UB).
    if (auto* editor = dynamic_cast<juce::AudioProcessorEditor*> (getContentComponent()))
    {
        clearContentComponent();
        EditorCrashGuard::deleteEditor (pluginInstance.get(), editor);
    }
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
