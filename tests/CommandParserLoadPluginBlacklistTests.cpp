/**
 * CommandParser loadPlugin blacklist addressing tests (block D, D6 routing
 * gap fix — Spec gap 5, see docs/plan-block-d-out-of-process.md).
 *
 * A blacklisted plugin never appears in knownPlugins (the scanner skips it),
 * so the original loadPlugin lookup cannot address it and answers
 * "plugin not found" — the D6 child-host path (Main.cpp loadPluginByDescription
 * blacklist branch: setChildMeasurePath + orchestrator bind) is unreachable.
 *
 * These tests lock the fallback: loadPlugin matches the request against the
 * persistent blacklist (exact path, or file-name-without-extension,
 * case-insensitive — e.g. "Pianoteq 9" <-> "Pianoteq 9.vst3") and dispatches
 * a minimal PluginDescription through the same loadPluginCallback (async,
 * message thread). Whitelisted-plugin behaviour is unchanged (regression is
 * covered by the existing CommandParserTests.cpp loadPlugin cases).
 */
#include <catch2/catch_test_macros.hpp>

#include "TestPlugin.h"
#include "../source/ipc/CommandParser.h"
#include "../source/ipc/Protocol.h"
#include "../source/host/PluginManager.h"

#include <atomic>

//==============================================================================
// Helpers (mirror CommandParserTests.cpp)
//==============================================================================

/** Ensure the JUCE MessageManager exists (auto-created in console apps). */
static void ensureMessageManager()
{
    juce::MessageManager::getInstance();
}

/** Flush any pending MessageManager callbacks queued via callAsync. */
static void flushMessageManager (int timeoutMs = 100)
{
    juce::MessageManager::getInstance()->runDispatchLoopUntil (timeoutMs);
}

//==============================================================================
// R1 — name addressing (blacklist entry file name minus .vst3 extension)
//==============================================================================

TEST_CASE ("CommandParser: loadPlugin addresses a blacklisted plugin by name and reports blacklisted",
           "[commandparser][loadPlugin][blacklist]")
{
    ensureMessageManager();

    // ---- Arrange ----
    // The blacklisted plugin is absent from knownPlugins (scan skips it) —
    // only the persistent blacklist holds its path.
    PluginManager pm;
    pm.addToBlacklistLocked ("C:\\fake\\Blacklisted.vst3");

    CommandParser parser;
    parser.setPluginManager (&pm);

    std::atomic<bool> callbackFired { false };
    juce::PluginDescription capturedDesc;
    parser.setLoadPluginCallback ([&] (const juce::PluginDescription& d) {
        callbackFired.store (true);
        capturedDesc = d;
    });

    // ---- Act: address by the file name without its .vst3 extension ----
    auto response = parser.handleCommand (R"({"cmd":"loadPlugin","path":"Blacklisted"})");
    flushMessageManager (200);

    // ---- Assert ----
    // 1. Response: ok + name + blacklist marker.
    REQUIRE (response.contains ("\"ok\":true"));
    REQUIRE (response.contains (R"("name":"Blacklisted")"));
    REQUIRE (response.contains (R"("blacklisted":true)"));

    // 2. The loadPluginCallback received the synthesized description: the
    //    blacklisted path as fileOrIdentifier (so Main.cpp loadPluginByDescription
    //    can isBlacklistedPath() it and route to the child host), the file
    //    name as the display name, and the VST3 format.
    REQUIRE (callbackFired.load());
    REQUIRE (capturedDesc.fileOrIdentifier == "C:\\fake\\Blacklisted.vst3");
    REQUIRE (capturedDesc.name == "Blacklisted");
    REQUIRE (capturedDesc.pluginFormatName == "VST3");
}

//==============================================================================
// R1b — case-insensitive name addressing
//==============================================================================

TEST_CASE ("CommandParser: loadPlugin matches blacklisted plugin name case-insensitively",
           "[commandparser][loadPlugin][blacklist]")
{
    ensureMessageManager();

    // ---- Arrange ----
    PluginManager pm;
    pm.addToBlacklistLocked ("C:\\fake\\Blacklisted.vst3");

    CommandParser parser;
    parser.setPluginManager (&pm);

    std::atomic<bool> callbackFired { false };
    juce::PluginDescription capturedDesc;
    parser.setLoadPluginCallback ([&] (const juce::PluginDescription& d) {
        callbackFired.store (true);
        capturedDesc = d;
    });

    // ---- Act: all-lowercase name ----
    auto response = parser.handleCommand (R"({"cmd":"loadPlugin","path":"blacklisted"})");
    flushMessageManager (200);

    // ---- Assert ----
    REQUIRE (response.contains ("\"ok\":true"));
    REQUIRE (callbackFired.load());
    REQUIRE (capturedDesc.fileOrIdentifier == "C:\\fake\\Blacklisted.vst3");
}

//==============================================================================
// R2 — exact-path addressing
//==============================================================================

TEST_CASE ("CommandParser: loadPlugin addresses a blacklisted plugin by its exact path",
           "[commandparser][loadPlugin][blacklist]")
{
    ensureMessageManager();

    // ---- Arrange ----
    PluginManager pm;
    pm.addToBlacklistLocked ("C:\\fake\\Blacklisted.vst3");

    CommandParser parser;
    parser.setPluginManager (&pm);

    std::atomic<bool> callbackFired { false };
    juce::PluginDescription capturedDesc;
    parser.setLoadPluginCallback ([&] (const juce::PluginDescription& d) {
        callbackFired.store (true);
        capturedDesc = d;
    });

    // ---- Act: the full blacklisted path (with extension) ----
    auto response = parser.handleCommand (R"({"cmd":"loadPlugin","path":"C:\\fake\\Blacklisted.vst3"})");
    flushMessageManager (200);

    // ---- Assert ----
    REQUIRE (response.contains ("\"ok\":true"));
    REQUIRE (response.contains (R"("name":"Blacklisted")"));
    REQUIRE (response.contains (R"("blacklisted":true)"));
    REQUIRE (callbackFired.load());
    REQUIRE (capturedDesc.fileOrIdentifier == "C:\\fake\\Blacklisted.vst3");
}

//==============================================================================
// R3 — unknown plugin still fails with the established error vocabulary
//==============================================================================

TEST_CASE ("CommandParser: loadPlugin still returns plugin not found for an unknown path",
           "[commandparser][loadPlugin][blacklist]")
{
    // ---- Arrange ----
    PluginManager pm;
    pm.addToBlacklistLocked ("C:\\fake\\Blacklisted.vst3");

    CommandParser parser;
    parser.setPluginManager (&pm);

    // ---- Act: neither a known plugin nor a blacklist entry ----
    auto response = parser.handleCommand (R"({"cmd":"loadPlugin","path":"Nonexistent"})");

    // ---- Assert: unchanged error vocabulary ----
    REQUIRE (response.contains ("\"ok\":false"));
    REQUIRE (response.contains ("plugin not found"));
}

//==============================================================================
// R4 — blacklist hit dispatches the callback asynchronously on the message
//      thread (same callAsync pattern as the known-plugin path)
//==============================================================================

TEST_CASE ("CommandParser: blacklisted loadPlugin dispatches callback async on the message thread",
           "[commandparser][loadPlugin][blacklist]")
{
    ensureMessageManager();

    // ---- Arrange ----
    PluginManager pm;
    pm.addToBlacklistLocked ("C:\\fake\\Blacklisted.vst3");

    CommandParser parser;
    parser.setPluginManager (&pm);

    std::atomic<bool> callbackFired { false };
    bool callbackOnMessageThread = false;
    parser.setLoadPluginCallback ([&] (const juce::PluginDescription&) {
        callbackFired.store (true);
        callbackOnMessageThread =
            juce::MessageManager::getInstance()->isThisTheMessageThread();
    });

    // ---- Act: handleCommand must return before the callback runs (callAsync
    // posts to the message queue; it never fires synchronously) ----
    auto response = parser.handleCommand (R"({"cmd":"loadPlugin","path":"Blacklisted"})");
    REQUIRE_FALSE (callbackFired.load());

    flushMessageManager (200);

    // ---- Assert: callback ran on the message thread via the dispatch loop ----
    REQUIRE (callbackFired.load());
    REQUIRE (callbackOnMessageThread);
    REQUIRE (response.contains ("\"ok\":true"));
}
