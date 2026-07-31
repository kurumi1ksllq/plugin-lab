#pragma once

#include <JuceHeader.h>

//==============================================================================
/**
 * Manages VST3 plugin discovery, loading, and lifecycle.
 *
 * Usage:
 *   PluginManager mgr;
 *   mgr.scanDirectory (File ("C:/Program Files/Common Files/VST3"));
 *   auto list = mgr.getKnownPlugins();
 *   auto plugin = mgr.loadPlugin (list[0]);
 */
class PluginManager
{
public:
    PluginManager();
    ~PluginManager();

    //==============================================================================
    /** Scan a directory for VST3 plugins (non-recursive). */
    void scanDirectory (const juce::File& directory);

    /** Scan the system VST3 directories. */
    void scanSystemDirectories();

    /** Returns the list of discovered plugins. */
    juce::KnownPluginList& getKnownPlugins()         { return knownPlugins; }
    const juce::KnownPluginList& getKnownPlugins() const { return knownPlugins; }

    /** Load a plugin instance from a PluginDescription. Returns nullptr on failure. */
    std::unique_ptr<juce::AudioPluginInstance> loadPlugin (const juce::PluginDescription& desc,
                                                            double sampleRate,
                                                            int blockSize);

    /** Create a plugin editor with crash protection. Returns nullptr on failure. */
    static juce::AudioProcessorEditor* createEditorSafe (juce::AudioPluginInstance* plugin);

private:
    juce::AudioPluginFormatManager formatManager;
    juce::KnownPluginList knownPlugins;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginManager)
};
