#pragma once

#include <JuceHeader.h>

//==============================================================================
/**
 * Manages VST3 plugin discovery, loading, and lifecycle.
 *
 * Usage:
 *   PluginManager mgr;
 *   mgr.scanSystemDirectories();
 *   auto list = mgr.getKnownPlugins();
 *   auto plugin = mgr.loadPlugin (list[0], 48000.0, 512);
 */
class PluginManager
{
public:
    PluginManager();
    ~PluginManager();

    //==============================================================================
    /** Scan a directory for VST3 plugins (non-recursive), passing a real
        dead-man's-pedal file so plugins that crash/hang the process are
        auto-blacklisted on the next scan. */
    void scanDirectory (const juce::File& directory, const juce::File& deadMansPedalFile);

    /** Scan the system VST3 directories incrementally: load the on-disk cache
        first (mtime-unchanged plugins get zero DLL loads), rescan changed/new
        files, then save the cache. */
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

    //==============================================================================
    // Persistent plugin cache + dead-man's-pedal (plan step 1, see STATUS.md)

    /** Bumped whenever the on-disk cache format changes. */
    static constexpr int kCacheVersion = 1;

    /** Test seam: override where the cache lives (default %APPDATA%/PluginLab/pluginlist.xml). */
    void setCacheFile (const juce::File& f)  { cacheFile = f; }
    const juce::File& getCacheFile() const   { return cacheFile; }

    /** Load the cache into knownPlugins (validating version, pruning ghost
        entries and host-killing plugins). Returns true on success; false means
        the caller must fall back to a full rescan. */
    bool loadCache();

    /** Serialize knownPlugins (including the blacklist) to the cache atomically
        (temp file + rename). */
    void saveCache();

    /** True if a plugin name is in the host-killing blacklist (Pianoteq family
        terminates the host process; see STATUS.md). */
    static bool isBlacklistedName (const juce::String& name);

private:
    /** Remove ghost entries (plugin files that no longer exist) and
        host-killing plugins from the in-memory list. */
    void pruneKnownPlugins();

    juce::AudioPluginFormatManager formatManager;
    juce::KnownPluginList knownPlugins;
    juce::File cacheFile;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginManager)
};
