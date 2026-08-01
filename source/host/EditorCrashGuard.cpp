#include "EditorCrashGuard.h"
#include "../utils/CrashLog.h"

namespace EditorCrashGuard
{

juce::AudioProcessorEditor* createEditor (juce::AudioPluginInstance* plugin)
{
    if (plugin == nullptr)
        return nullptr;

    try
    {
        return plugin->createEditorAndMakeActive();
    }
    catch (...)
    {
        CRASH_LOG_ERR ("Editor crash", plugin->getName());
        return nullptr;
    }
}

void deleteEditor (juce::AudioPluginInstance* plugin, juce::AudioProcessorEditor* editor)
{
    if (editor == nullptr)
        return;

    try
    {
        // Detach from any parent component first.
        if (auto* parent = editor->getParentComponent())
            parent->removeChildComponent (editor);

        // Notify the processor that the editor is going away. Must happen
        // before the editor is deleted (AudioProcessorEditor's destructor
        // asserts that it is no longer the active editor). For a VST3 editor
        // this also runs the plugin's view->removed() path.
        if (plugin != nullptr)
            plugin->editorBeingDeleted (editor);

        delete editor;
    }
    catch (...)
    {
        CRASH_LOG_ERR ("Editor delete crash",
            editor->getName().isEmpty() ? juce::String ("<unnamed>") : editor->getName());
    }
}

} // namespace EditorCrashGuard
