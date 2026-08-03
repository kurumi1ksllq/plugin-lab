#include <JuceHeader.h>
#include <catch2/catch_test_macros.hpp>
#include "../source/host/PluginManager.h"
#include "TestPlugin.h"

namespace
{
    //==============================================================================
    /** Creates a unique temp directory for one test; removed on destruction. */
    struct TempCacheDir
    {
        juce::File dir;

        TempCacheDir()
        {
            dir = juce::File::getSpecialLocation (
                      juce::File::SpecialLocationType::tempDirectory)
                      .getNonexistentChildFile ("pluginlab_cache_test_", "_dir");
            dir.createDirectory();
        }

        ~TempCacheDir()
        {
            dir.deleteRecursively();
        }

        juce::File cacheFile() const   { return dir.getChildFile ("pluginlist.xml"); }
        juce::File pedalFile() const   { return dir.getChildFile ("deadMansPedal"); }
    };

    /** A real file that exists on disk (prune keeps entries whose file exists). */
    juce::File createTempPluginFile (const juce::File& dir, const juce::String& name)
    {
        auto f = dir.getChildFile (name);
        f.replaceWithText ("fake plugin marker");
        return f;
    }

    juce::PluginDescription makeDesc (const juce::String& name,
                                      const juce::String& fileOrIdentifier,
                                      int uid)
    {
        juce::PluginDescription d;
        d.name = name;
        d.descriptiveName = name;
        d.fileOrIdentifier = fileOrIdentifier;
        d.pluginFormatName = "VST3";
        d.uniqueId = uid;
        return d;
    }
}  // namespace

//==============================================================================
TEST_CASE ("PluginManager: saveCache-to-loadCache round-trips types", "[pluginmanager][cache]")
{
    // Arrange
    TempCacheDir tmp;
    auto fileA = createTempPluginFile (tmp.dir, "PluginA.vst3");
    auto fileB = createTempPluginFile (tmp.dir, "PluginB.vst3");

    PluginManager mgr;
    mgr.setCacheFile (tmp.cacheFile());
    mgr.getKnownPlugins().addType (makeDesc ("Plugin A", fileA.getFullPathName(), 1001));
    mgr.getKnownPlugins().addType (makeDesc ("Plugin B", fileB.getFullPathName(), 1002));

    // Act
    mgr.saveCache();

    // Assert: cache written
    REQUIRE (tmp.cacheFile().existsAsFile());
    REQUIRE (! tmp.cacheFile().withFileExtension (".xml.tmp").exists());

    // Act: load into a fresh manager (simulates restart)
    PluginManager fresh;
    fresh.setCacheFile (tmp.cacheFile());
    const bool loaded = fresh.loadCache();

    // Assert: types restored
    REQUIRE (loaded);
    REQUIRE (fresh.getKnownPlugins().getNumTypes() == 2);
    auto types = fresh.getKnownPlugins().getTypes();
    juce::StringArray names;
    for (const auto& t : types)
        names.add (t.name);
    REQUIRE (names.contains ("Plugin A"));
    REQUIRE (names.contains ("Plugin B"));
}

TEST_CASE ("PluginManager: blacklist round-trips through the cache",
           "[pluginmanager][cache][blacklist]")
{
    // Arrange
    TempCacheDir tmp;
    const auto blPath = tmp.dir.getChildFile ("BadPlugin.vst3").getFullPathName();

    PluginManager mgr;
    mgr.setCacheFile (tmp.cacheFile());
    mgr.getKnownPlugins().addToBlacklist (blPath);

    // Act
    mgr.saveCache();
    PluginManager fresh;
    fresh.setCacheFile (tmp.cacheFile());
    const bool loaded = fresh.loadCache();

    // Assert
    REQUIRE (loaded);
    REQUIRE (fresh.getKnownPlugins().getBlacklistedFiles().contains (blPath));
}

TEST_CASE ("PluginManager: loadCache returns false for a missing cache",
           "[pluginmanager][cache]")
{
    // Arrange
    TempCacheDir tmp;
    PluginManager mgr;
    mgr.setCacheFile (tmp.cacheFile());   // never written

    // Act / Assert
    REQUIRE_FALSE (mgr.loadCache());
}

TEST_CASE ("PluginManager: loadCache returns false for corrupted XML",
           "[pluginmanager][cache]")
{
    // Arrange
    TempCacheDir tmp;
    tmp.cacheFile().replaceWithText ("this is <not valid> xml {{{");

    PluginManager mgr;
    mgr.setCacheFile (tmp.cacheFile());

    // Act / Assert
    REQUIRE_FALSE (mgr.loadCache());
}

TEST_CASE ("PluginManager: loadCache returns false for a version mismatch",
           "[pluginmanager][cache]")
{
    // Arrange
    TempCacheDir tmp;
    tmp.cacheFile().replaceWithText (
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<KNOWNPLUGINS version=\"0\">\n"
        "  <PLUGIN name=\"Old\" uid=\"1\" file=\"x.vst3\" format=\"VST3\"/>\n"
        "</KNOWNPLUGINS>\n");

    PluginManager mgr;
    mgr.setCacheFile (tmp.cacheFile());

    // Act / Assert
    REQUIRE_FALSE (mgr.loadCache());
}

TEST_CASE ("PluginManager: loadCache prunes ghost entries (files that no longer exist)",
           "[pluginmanager][cache][prune]")
{
    // Arrange: cache XML referencing a plugin whose file does not exist
    TempCacheDir tmp;
    const auto ghostPath = tmp.dir.getChildFile ("Ghost.vst3").getFullPathName();  // never created
    tmp.cacheFile().replaceWithText (
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<KNOWNPLUGINS version=\"1\">\n"
        "  <PLUGIN name=\"Ghost\" uid=\"7\" file=\"" + ghostPath + "\" format=\"VST3\"/>\n"
        "</KNOWNPLUGINS>\n");

    PluginManager mgr;
    mgr.setCacheFile (tmp.cacheFile());

    // Act
    const bool loaded = mgr.loadCache();

    // Assert: loaded but the ghost was pruned
    REQUIRE (loaded);
    REQUIRE (mgr.getKnownPlugins().getNumTypes() == 0);
}

TEST_CASE ("PluginManager: loadCache prunes Pianoteq (host-killing) entries",
           "[pluginmanager][cache][prune][blacklist]")
{
    // Arrange: cache XML containing a Pianoteq entry alongside a healthy one
    TempCacheDir tmp;
    auto healthy = createTempPluginFile (tmp.dir, "Healthy.vst3");
    tmp.cacheFile().replaceWithText (
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<KNOWNPLUGINS version=\"1\">\n"
        "  <PLUGIN name=\"Pianoteq 9 Stage\" uid=\"8\" file=\"C:\\fake\\pianoteq.vst3\" format=\"VST3\"/>\n"
        "  <PLUGIN name=\"Healthy\" uid=\"9\" file=\"" + healthy.getFullPathName() + "\" format=\"VST3\"/>\n"
        "</KNOWNPLUGINS>\n");

    PluginManager mgr;
    mgr.setCacheFile (tmp.cacheFile());

    // Act
    const bool loaded = mgr.loadCache();

    // Assert
    REQUIRE (loaded);
    REQUIRE (mgr.getKnownPlugins().getNumTypes() == 1);
    auto types = mgr.getKnownPlugins().getTypes();
    REQUIRE (types[0].name == "Healthy");
}

TEST_CASE ("PluginManager: isBlacklistedName matches Pianoteq variants case-insensitively",
           "[pluginmanager][blacklist]")
{
    // Arrange / Act / Assert
    REQUIRE (PluginManager::isBlacklistedName ("Pianoteq 9"));
    REQUIRE (PluginManager::isBlacklistedName ("Pianoteq 8 STAGE"));
    REQUIRE (PluginManager::isBlacklistedName ("PIANOTEQ 7"));
    REQUIRE (PluginManager::isBlacklistedName ("Pianoteq 9.5 Pro"));
    REQUIRE_FALSE (PluginManager::isBlacklistedName ("Pianoteq 6"));
    REQUIRE_FALSE (PluginManager::isBlacklistedName ("Some Other Plugin"));
    REQUIRE_FALSE (PluginManager::isBlacklistedName (""));
}

TEST_CASE ("PluginManager: saveCache writes a versioned, parseable cache atomically",
           "[pluginmanager][cache]")
{
    // Arrange
    TempCacheDir tmp;
    PluginManager mgr;
    mgr.setCacheFile (tmp.cacheFile());
    mgr.getKnownPlugins().addType (makeDesc ("Only One", tmp.dir.getChildFile ("Only.vst3").getFullPathName(), 42));
    // Note: Only.vst3 does not exist → would be pruned on load, but saveCache
    // does not prune — it serializes what is in the list.

    // Act
    mgr.saveCache();

    // Assert: no temp leftover
    REQUIRE_FALSE (tmp.cacheFile().withFileExtension (".xml.tmp").exists());

    // Assert: valid XML with root KNOWNPLUGINS + version attribute
    auto xml = juce::XmlDocument::parse (tmp.cacheFile());
    REQUIRE (xml != nullptr);
    REQUIRE (xml->hasTagName ("KNOWNPLUGINS"));
    REQUIRE (xml->getIntAttribute ("version", -1) == PluginManager::kCacheVersion);
}

//==============================================================================
// Step 6 (startup lag): cacheIsCurrent — directory VST3 bundles without
// moduleinfo.json are cached under their inner DLL path (...\Contents\x86_64-win\X.vst3)
// while enumeration always produces the bundle path (...\X.vst3). The incremental
// scan must therefore match bundle paths against both exact and inner-prefix
// entries, comparing the inner DLL's mtime (the correct update-detection
// baseline) — otherwise these bundles are re-scanned on every hot start (1-3s
// each, the 31s hot-scan root cause).

TEST_CASE ("PluginManager: cacheIsCurrent matches a bundle path to its inner-DLL cache entry",
           "[pluginmanager][cache][incremental]")
{
    // Arrange: real bundle directory structure with an inner DLL
    TempCacheDir tmp;
    auto bundle = tmp.dir.getChildFile ("MyBundle.vst3");
    bundle.createDirectory();
    auto arch = bundle.getChildFile ("Contents").getChildFile ("x86_64-win");
    arch.createDirectory();
    auto inner = arch.getChildFile ("MyBundle.vst3");
    inner.replaceWithText ("marker");

    auto desc = makeDesc ("MyBundle", inner.getFullPathName(), 2001);
    desc.lastFileModTime = inner.getLastModificationTime();
    PluginManager mgr;
    mgr.getKnownPlugins().addType (desc);

    // Act / Assert: the bundle enumeration path matches the inner entry
    REQUIRE (mgr.cacheIsCurrent (bundle.getFullPathName()));
    // and the inner path itself matches exactly
    REQUIRE (mgr.cacheIsCurrent (inner.getFullPathName()));
}

TEST_CASE ("PluginManager: cacheIsCurrent returns false when the inner DLL was updated",
           "[pluginmanager][cache][incremental]")
{
    // Arrange
    TempCacheDir tmp;
    auto bundle = tmp.dir.getChildFile ("MyBundle.vst3");
    bundle.createDirectory();
    auto arch = bundle.getChildFile ("Contents").getChildFile ("x86_64-win");
    arch.createDirectory();
    auto inner = arch.getChildFile ("MyBundle.vst3");
    inner.replaceWithText ("marker");

    auto desc = makeDesc ("MyBundle", inner.getFullPathName(), 2002);
    desc.lastFileModTime = inner.getLastModificationTime();
    PluginManager mgr;
    mgr.getKnownPlugins().addType (desc);
    REQUIRE (mgr.cacheIsCurrent (bundle.getFullPathName()));

    // Act: touch the inner DLL (simulates a plugin update)
    inner.setLastModificationTime (juce::Time (juce::Time::currentTimeMillis() + 5000));

    // Assert: no longer current → must re-scan
    REQUIRE_FALSE (mgr.cacheIsCurrent (bundle.getFullPathName()));
}

TEST_CASE ("PluginManager: cacheIsCurrent returns false for unknown paths",
           "[pluginmanager][cache][incremental]")
{
    // Arrange / Act / Assert
    TempCacheDir tmp;
    PluginManager mgr;
    REQUIRE_FALSE (mgr.cacheIsCurrent (tmp.dir.getChildFile ("Nope.vst3").getFullPathName()));
}

TEST_CASE ("PluginManager: cacheIsCurrent matches a single-file path exactly",
           "[pluginmanager][cache][incremental]")
{
    // Arrange
    TempCacheDir tmp;
    auto pluginFile = createTempPluginFile (tmp.dir, "Flat.vst3");
    auto desc = makeDesc ("Flat", pluginFile.getFullPathName(), 2003);
    desc.lastFileModTime = pluginFile.getLastModificationTime();
    PluginManager mgr;
    mgr.getKnownPlugins().addType (desc);

    // Act / Assert
    REQUIRE (mgr.cacheIsCurrent (pluginFile.getFullPathName()));
}

TEST_CASE ("PluginManager: cacheIsCurrent returns false when the cached mtime is stale",
           "[pluginmanager][cache][incremental]")
{
    // Arrange: cache mtime deliberately older than the file
    TempCacheDir tmp;
    auto pluginFile = createTempPluginFile (tmp.dir, "Flat2.vst3");
    auto desc = makeDesc ("Flat2", pluginFile.getFullPathName(), 2004);
    desc.lastFileModTime = juce::Time (pluginFile.getLastModificationTime().toMilliseconds() - 100000);
    PluginManager mgr;
    mgr.getKnownPlugins().addType (desc);

    // Act / Assert
    REQUIRE_FALSE (mgr.cacheIsCurrent (pluginFile.getFullPathName()));
}

TEST_CASE ("PluginManager: cacheIsCurrent handles a bundle-path entry whose mtime is the inner DLL's",
           "[pluginmanager][cache][incremental]")
{
    // Arrange: legacy entry — fileOrIdentifier is the bundle directory but
    // lastFileModTime is the inner DLL's (produced by the pre-step-6 normalize).
    // File(bundle).mtime (directory) != lastFileModTime, so the check must probe
    // the bundle's inner DLL before deciding.
    TempCacheDir tmp;
    auto bundle = tmp.dir.getChildFile ("MyBundle.vst3");
    bundle.createDirectory();
    auto arch = bundle.getChildFile ("Contents").getChildFile ("x86_64-win");
    arch.createDirectory();
    auto inner = arch.getChildFile ("MyBundle.vst3");
    inner.replaceWithText ("marker");

    auto desc = makeDesc ("MyBundle", bundle.getFullPathName(), 2010);
    desc.lastFileModTime = inner.getLastModificationTime();   // bundle path + inner DLL mtime
    PluginManager mgr;
    mgr.getKnownPlugins().addType (desc);

    // Act / Assert
    REQUIRE (mgr.cacheIsCurrent (bundle.getFullPathName()));
}

TEST_CASE ("PluginManager: dedupeKnownPlugins keeps one entry per bundle (inner preferred)",
           "[pluginmanager][cache][dedupe]")
{
    // Arrange: inner entry + legacy bundle entry for the same plugin
    TempCacheDir tmp;
    auto bundle = tmp.dir.getChildFile ("MyBundle.vst3");
    bundle.createDirectory();
    auto arch = bundle.getChildFile ("Contents").getChildFile ("x86_64-win");
    arch.createDirectory();
    auto inner = arch.getChildFile ("MyBundle.vst3");
    inner.replaceWithText ("marker");

    PluginManager mgr;
    mgr.getKnownPlugins().addType (makeDesc ("MyBundle", inner.getFullPathName(), 2011));
    mgr.getKnownPlugins().addType (makeDesc ("MyBundle", bundle.getFullPathName(), 2012));
    REQUIRE (mgr.getKnownPlugins().getNumTypes() == 2);

    // Act
    mgr.dedupeKnownPlugins();

    // Assert: exactly one entry remains, and it is the inner-DLL one
    REQUIRE (mgr.getKnownPlugins().getNumTypes() == 1);
    auto types = mgr.getKnownPlugins().getTypes();
    REQUIRE (types[0].fileOrIdentifier == inner.getFullPathName());
}

TEST_CASE ("PluginManager: dedupeKnownPlugins keeps single entries and distinct bundles",
           "[pluginmanager][cache][dedupe]")
{
    // Arrange: two distinct bundles + one plain file, no duplicates
    TempCacheDir tmp;
    auto b1 = tmp.dir.getChildFile ("One.vst3");
    b1.createDirectory();
    auto b2 = tmp.dir.getChildFile ("Two.vst3");
    b2.createDirectory();
    auto plain = createTempPluginFile (tmp.dir, "Plain.vst3");

    PluginManager mgr;
    mgr.getKnownPlugins().addType (makeDesc ("One", b1.getFullPathName(), 2021));
    mgr.getKnownPlugins().addType (makeDesc ("Two", b2.getFullPathName(), 2022));
    mgr.getKnownPlugins().addType (makeDesc ("Plain", plain.getFullPathName(), 2023));
    REQUIRE (mgr.getKnownPlugins().getNumTypes() == 3);

    // Act
    mgr.dedupeKnownPlugins();

    // Assert: nothing removed
    REQUIRE (mgr.getKnownPlugins().getNumTypes() == 3);
}

//==============================================================================
// Step 3 (load timeout): loadPlugin runs creation asynchronously with a
// WaitableEvent timeout; on timeout it blacklists the plugin (bundle key) and
// returns nullptr; late callbacks from a stale generation are ignored.

TEST_CASE ("PluginManager: loadPlugin returns the instance when creation succeeds",
           "[pluginmanager][load]")
{
    // Arrange
    TempCacheDir tmp;
    PluginManager mgr;
    mgr.setAsyncCreateOverride ([] (const juce::PluginDescription&, double, int,
                                    PluginManager::PluginCreationCallback cb)
    {
        cb (std::make_unique<TestPlugin>(), juce::String());
    });

    // Act
    auto inst = mgr.loadPlugin (makeDesc ("Fake", "C:\\fake\\fake.vst3", 3001), 48000.0, 512);

    // Assert
    REQUIRE (inst != nullptr);
}

TEST_CASE ("PluginManager: loadPlugin returns nullptr when creation fails fast",
           "[pluginmanager][load]")
{
    // Arrange
    TempCacheDir tmp;
    PluginManager mgr;
    mgr.setAsyncCreateOverride ([] (const juce::PluginDescription&, double, int,
                                    PluginManager::PluginCreationCallback cb)
    {
        cb (nullptr, "cannot load");
    });

    // Act
    auto inst = mgr.loadPlugin (makeDesc ("Fake", "C:\\fake\\fake.vst3", 3002), 48000.0, 512);

    // Assert
    REQUIRE (inst == nullptr);
}

TEST_CASE ("PluginManager: loadPlugin times out and blacklists the plugin",
           "[pluginmanager][load][timeout]")
{
    // Arrange: the override swallows the callback → creation "hangs"
    TempCacheDir tmp;
    PluginManager mgr;
    mgr.setLoadTimeoutMs (50);
    mgr.setAsyncCreateOverride ([] (const juce::PluginDescription&, double, int,
                                    PluginManager::PluginCreationCallback)
    {
        // never invoke the callback
    });

    // Act
    auto inst = mgr.loadPlugin (makeDesc ("Fake", "C:\\fake\\fake.vst3", 3003), 48000.0, 512);

    // Assert: nullptr + blacklisted
    REQUIRE (inst == nullptr);
    REQUIRE (mgr.getKnownPlugins().getBlacklistedFiles().contains ("C:\\fake\\fake.vst3"));
}

TEST_CASE ("PluginManager: loadPlugin timeout blacklists the bundle key for inner-DLL descriptions",
           "[pluginmanager][load][timeout]")
{
    // Arrange: description uses the inner DLL path — the blacklist key must be
    // the bundle path so the scan-stage blacklist check (which enumerates
    // bundle paths) actually hits.
    TempCacheDir tmp;
    PluginManager mgr;
    mgr.setLoadTimeoutMs (50);
    mgr.setAsyncCreateOverride ([] (const juce::PluginDescription&, double, int,
                                    PluginManager::PluginCreationCallback) {});

    // Act
    auto inst = mgr.loadPlugin (makeDesc ("Fake",
        "C:\\fake\\Fake.vst3\\Contents\\x86_64-win\\Fake.vst3", 3004), 48000.0, 512);

    // Assert
    REQUIRE (inst == nullptr);
    REQUIRE (mgr.getKnownPlugins().getBlacklistedFiles().contains ("C:\\fake\\Fake.vst3"));
}

TEST_CASE ("PluginManager: loadPlugin ignores late callbacks from a stale generation",
           "[pluginmanager][load][timeout]")
{
    // Arrange: both loads hang (callback swallowed); the first load's callback
    // is saved and only invoked after a second load bumped the generation.
    TempCacheDir tmp;
    PluginManager mgr;
    mgr.setLoadTimeoutMs (50);
    PluginManager::PluginCreationCallback stale;
    mgr.setAsyncCreateOverride ([&] (const juce::PluginDescription&, double, int,
                                     PluginManager::PluginCreationCallback cb)
    {
        if (! stale)   // keep only the FIRST (generation 1) callback
            stale = std::move (cb);
    });

    auto first = mgr.loadPlugin (makeDesc ("Fake", "C:\\fake\\fake.vst3", 3005), 48000.0, 512);
    REQUIRE (first == nullptr);
    auto second = mgr.loadPlugin (makeDesc ("Fake", "C:\\fake\\fake.vst3", 3006), 48000.0, 512);
    REQUIRE (second == nullptr);

    // Act: the stale (first-generation) callback finally arrives
    REQUIRE_NOTHROW (stale (std::make_unique<TestPlugin>(), juce::String()));

    // Assert: no crash, blacklist intact
    REQUIRE (mgr.getKnownPlugins().getBlacklistedFiles().contains ("C:\\fake\\fake.vst3"));
}
