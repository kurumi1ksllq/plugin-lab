#include <JuceHeader.h>
#include <catch2/catch_test_macros.hpp>
#include "../source/host/PluginManager.h"

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
