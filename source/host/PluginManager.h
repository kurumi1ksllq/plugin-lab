#pragma once

#include <JuceHeader.h>
#include <atomic>
#include <functional>

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

    /** True if the cache already has a current (mtime-consistent) entry for the
        given scan identifier (a directory-bundle path or a single-file path).
        Directory VST3 bundles without moduleinfo.json are cached under their
        inner DLL path (...\\Contents\\x86_64-win\\X.vst3) while enumeration
        produces the bundle path — so this matches the bundle path against both
        exact and inner-prefix entries, comparing the inner DLL's mtime (the
        correct update-detection baseline). The scan loop skips identifiers this
        returns true for, fixing the hot-start lag from re-scanning the same
        12 bundles every launch (plan step 6). */
    bool cacheIsCurrent (const juce::String& fileOrIdentifier) const;

    /** Remove duplicate entries for the same plugin (scanAndAddFile only adds,
        never replaces — re-scanning a file that already has a legacy bundle-path
        entry piles up a second inner-DLL entry). Keyed by the bundle path; the
        inner-DLL entry (correct mtime baseline) is kept over the bundle-path one. */
    void dedupeKnownPlugins();

    /** True if a plugin name is in the host-killing blacklist (Pianoteq family
        terminates the host process; see STATUS.md). */
    static bool isBlacklistedName (const juce::String& name);

    //==============================================================================
    // Load timeout (plan step 3): loadPlugin runs creation asynchronously with a
    // WaitableEvent timeout so a hung plugin creation can't block the loader
    // thread forever. On timeout the plugin is blacklisted (preventive — the
    // next scan/load skips it). A real hang still freezes the message thread
    // (JUCE creates instances there); that is documented as out of reach
    // (kill process) and mitigated by the blacklist + dead-man's pedal.

    /** Default creation timeout. */
    static constexpr int kLoadTimeoutMs = 30000;

    using PluginCreationCallback = juce::AudioPluginFormat::PluginCreationCallback;
    using AsyncCreateFn = std::function<void (const juce::PluginDescription&, double, int,
                                              PluginCreationCallback)>;

    /** Test seam: override the creation-timeout duration. */
    void setLoadTimeoutMs (int ms)                 { loadTimeoutMs = ms; }

    /** Test seam: substitute the async creation call (unit tests can't load
        real VST3 DLLs). */
    void setAsyncCreateOverride (AsyncCreateFn fn) { asyncCreateOverride = std::move (fn); }

    //==============================================================================
    // Scan watchdog (plan step 4): a plugin DLL hung in DllMain/InitDll/
    // GetPluginFactory can never be terminated in-process and scanNextFile never
    // returns — the watchdog (message-thread timer polling scan progress) detects
    // a stalled scan, blacklists the hung file (persisted immediately), and the
    // host abandons the stuck scan thread. Hang count is capped so repeated
    // hangs (each leaks a thread + pins a DLL) can't degrade the process forever.

    static constexpr int kScanHangTimeoutMs = 60000;
    static constexpr int kMaxScanHangs = 3;

    /** Scan-loop lifecycle: call beginScan() before, endScan() after (also on
        exception). updateScanProgress() is called per scanned file with the
        scanner's progress (0..1) and the current file's bundle path. */
    void beginScan();
    void updateScanProgress (float progress, const juce::String& currentFile);
    void endScan();

    bool isScanRunning() const         { return scanRunning.load(); }
    float getScanProgress() const      { return scanProgress.load(); }
    int getScanHangCount() const       { return scanHangCount.load(); }
    int64 getLastScanProgressTimeMs() const { return lastScanProgressMs.load(); }
    juce::String getCurrentScanFile() const;

    /** Watchdog action after a stall is detected: blacklist the current file
        (bundle key), persist the blacklist immediately (the stuck scan never
        reaches saveCache), bump the hang count. Returns true when the hang cap
        is reached (host should stop auto-rescanning). */
    bool handleScanHang();

    /** Remove all blacklist entries (UI "clear blacklist and rescan" entry,
        risk R4/R7: a once-hung plugin may have been fixed). */
    void clearBlacklist()              { knownPlugins.clearBlacklistedFiles(); }

private:
    /** Remove ghost entries (plugin files that no longer exist) and
        host-killing plugins from the in-memory list. */
    void pruneKnownPlugins();

    juce::AudioPluginFormatManager formatManager;
    juce::KnownPluginList knownPlugins;
    juce::File cacheFile;
    int loadTimeoutMs = kLoadTimeoutMs;
    AsyncCreateFn asyncCreateOverride;

    // Scan-state members for the watchdog (written by the scan thread, read by
    // the message-thread timer; atomics + a small mutex-guarded current file).
    std::atomic<bool> scanRunning { false };
    std::atomic<float> scanProgress { 0.0f };
    std::atomic<int> scanHangCount { 0 };
    std::atomic<int64> lastScanProgressMs { 0 };
    mutable std::mutex scanFileLock;     // mutable: locked from const getCurrentScanFile()
    juce::String currentScanFile;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginManager)
};
