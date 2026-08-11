/**
 * ChildProtocolTests (block D, D3a): parameter snapshot/restore roundtrip
 * against the REAL out-of-process host child (PluginHostChild.exe, D1a+D1b).
 *
 * This file covers the D3a protocol half — the restart half (coordinator
 * re-spawns the child and re-applies the snapshot) lands in D3b. New commands
 * per docs/plan-block-d-out-of-process.md §子进程 IPC 协议契约 (D3a ticket):
 *
 *   snapshot_params → {"ok":true,"params":[{"id":...,"value":...},...]}
 *     — full parameter state, STABLE-id keyed (getParameterID(), ADR-D-1:
 *     NOT the name-keyed captureParameterSnapshot, which has no re-inject
 *     path). value is normalized 0..1.
 *   restore_params  {"params":[{id,value},...]} → {"ok":true}
 *     — stable-id lookup (mirror of ParameterTimeline::findParam), unknown
 *     ids skipped, values normalized 0..1.
 *
 * Tests spawn the real child through PluginHostChildCoordinator (D1c) and
 * drive the frozen stdin/stdout JSON-line protocol, exactly like
 * PluginHostChildIntegrationTests / ChildHostParityTests. R1/R2/R3 load the
 * real magic.CURVE VST3 (CHILD_PARITY_PLUGIN) — a registered real-plugin
 * exception (tests/AGENTS.md); a missing plugin SKIPs (Catch2 SKIP
 * semantics), never a fake substitute.
 */
#include <catch2/catch_test_macros.hpp>

#include "../source/host/ChildProcessCoordinator.h"
#include "../source/child/ChildProtocol.h"

#include <atomic>
#include <cmath>
#include <vector>

#ifndef PLUGIN_HOST_CHILD_EXE
#error "PLUGIN_HOST_CHILD_EXE must be defined by tests/CMakeLists.txt"
#endif

#ifndef CHILD_PARITY_PLUGIN
#error "CHILD_PARITY_PLUGIN must be defined by tests/CMakeLists.txt"
#endif

namespace
{
    //==============================================================================
    juce::File realChildExe()
    {
        return juce::File (juce::String (PLUGIN_HOST_CHILD_EXE));
    }

    juce::File realPluginFile()
    {
        return juce::File (juce::String (CHILD_PARITY_PLUGIN));
    }

    /** Pop lines until one contains `needle` (progress lines pass through
     *  silently — same pattern as ChildHostParityTests::popUntil). */
    juce::String popUntil (PluginHostChildCoordinator& coord, const juce::String& needle,
                           int timeoutMs)
    {
        const auto deadline = juce::Time::getMillisecondCounter()
                            + static_cast<juce::uint32> (timeoutMs);
        while (juce::Time::getMillisecondCounter() < deadline)
        {
            const auto line = coord.popLine (200);
            if (line.isEmpty())
                continue;
            if (line.contains (needle))
                return line;
        }
        return {};
    }

    /** start handshake + load the real plugin; false on any failure. */
    bool startAndLoad (PluginHostChildCoordinator& coord, const juce::File& pluginFile)
    {
        if (! coord.sendLine (R"({"cmd":"start"})"))
            return false;
        if (popUntil (coord, "\"pid\":", 5000).isEmpty())
            return false;

        juce::DynamicObject req;
        req.setProperty ("cmd", "load");
        req.setProperty ("path", pluginFile.getFullPathName());
        const auto reqVar = juce::var (new juce::DynamicObject (req));
        if (! coord.sendLine (juce::JSON::toString (reqVar, true)))
            return false;
        const auto loadLine = popUntil (coord, "\"name\"", 40000);
        return loadLine.isNotEmpty() && loadLine.contains ("\"ok\":true");
    }

    /** One snapshot entry: stable id + normalized value. */
    struct ParamEntry
    {
        juce::String id;
        double value = 0.0;
    };

    /** Parse a snapshot_params response line → entries. Empty on any parse
     *  failure (including an {"ok":false} error line). */
    std::vector<ParamEntry> parseParams (const juce::String& line)
    {
        std::vector<ParamEntry> entries;
        const auto doc = juce::JSON::parse (line);
        if (! doc.isObject())
            return entries;
        const auto params = doc["params"];
        if (! params.isArray())
            return entries;
        entries.reserve (static_cast<size_t> (params.size()));
        for (int i = 0; i < params.size(); ++i)
        {
            const auto item = params[i];
            if (! item.isObject())
                return {};                       // malformed entry → fail loudly
            const auto id = item["id"].toString();
            const auto valueVar = item["value"];
            if (id.isEmpty() || ! valueVar.isDouble())
                return {};                       // id/value fields must be valid
            entries.push_back ({ id, static_cast<double> (valueVar) });
        }
        return entries;
    }

    /** Send snapshot_params; returns the response line. */
    juce::String sendSnapshot (PluginHostChildCoordinator& coord)
    {
        if (! coord.sendLine (R"({"cmd":"snapshot_params"})"))
            return {};
        return popUntil (coord, "\"params\":", 5000);
    }

    /** Build + send restore_params; extraEntries (a JSON object fragment,
     *  e.g. an unknown-id entry) is appended after the known entries.
     *  Returns the response line. */
    juce::String sendRestore (PluginHostChildCoordinator& coord,
                              const std::vector<ParamEntry>& entries,
                              const juce::String& extraEntries = {})
    {
        juce::String req = R"({"cmd":"restore_params","params":[)";
        for (size_t i = 0; i < entries.size(); ++i)
        {
            if (i > 0)
                req += ",";
            req += R"({"id":")" + ChildProtocol::escapeJsonString (entries[i].id)
                 + R"(","value":)" + juce::String (entries[i].value, 6) + "}";
        }
        if (extraEntries.isNotEmpty())
        {
            if (! entries.empty())
                req += ",";
            req += extraEntries;
        }
        req += "]}";
        if (! coord.sendLine (req))
            return {};
        return popUntil (coord, "\"ok\"", 5000);
    }
}  // namespace

//==============================================================================
// R1 — snapshot_params after a real load: non-empty, stable-id keyed,
//      id/value fields legal (ADR-D-1).
//==============================================================================

TEST_CASE ("ChildProtocol: snapshot_params enumerates stable-id keyed params after load",
           "[childprotocol][snapshot]")
{
    // Arrange — real plugin + real child (no fake substitutes, tests/AGENTS.md).
    const auto pluginFile = realPluginFile();
    if (! pluginFile.exists())
        SKIP ("CHILD_PARITY_PLUGIN not present: " + pluginFile.getFullPathName());
    if (! realChildExe().existsAsFile())
        SKIP ("PLUGIN_HOST_CHILD_EXE not present: " + realChildExe().getFullPathName());

    PluginHostChildCoordinator coord (realChildExe().getFullPathName());
    std::atomic<bool> crashed { false };
    coord.setOnCrash ([&] (const juce::String&) { crashed.store (true); });
    REQUIRE (coord.start());
    REQUIRE (startAndLoad (coord, pluginFile));

    // Act — snapshot the full parameter set.
    const auto snapshotLine = sendSnapshot (coord);
    const auto params = parseParams (snapshotLine);

    // Assert — {"ok":true,"params":[...]}, non-empty, every entry has a
    // non-empty stable id and a legal normalized value in 0..1, ids unique.
    REQUIRE_FALSE (crashed.load());
    REQUIRE (snapshotLine.isNotEmpty());
    REQUIRE (snapshotLine.contains ("\"ok\":true"));
    REQUIRE_FALSE (params.empty());

    juce::StringArray seenIds;
    for (const auto& entry : params)
    {
        INFO ("param id=" << entry.id << " value=" << entry.value);
        REQUIRE_FALSE (entry.id.isEmpty());
        REQUIRE (entry.value >= 0.0);
        REQUIRE (entry.value <= 1.0);
        REQUIRE_FALSE (seenIds.contains (entry.id));
        seenIds.add (entry.id);
    }

    coord.stop();
}

//==============================================================================
// R2 — snapshot → restore_params → snapshot: the two snapshots are identical
//      (parameter state survives the restore roundtrip).
//==============================================================================

TEST_CASE ("ChildProtocol: snapshot-to-restore-to-snapshot roundtrip preserves every value",
           "[childprotocol][restore][snapshot]")
{
    // Arrange
    const auto pluginFile = realPluginFile();
    if (! pluginFile.exists())
        SKIP ("CHILD_PARITY_PLUGIN not present: " + pluginFile.getFullPathName());
    if (! realChildExe().existsAsFile())
        SKIP ("PLUGIN_HOST_CHILD_EXE not present: " + realChildExe().getFullPathName());

    PluginHostChildCoordinator coord (realChildExe().getFullPathName());
    std::atomic<bool> crashed { false };
    coord.setOnCrash ([&] (const juce::String&) { crashed.store (true); });
    REQUIRE (coord.start());
    REQUIRE (startAndLoad (coord, pluginFile));

    // Act — baseline snapshot, restore it verbatim, snapshot again.
    const auto baseline = parseParams (sendSnapshot (coord));
    REQUIRE_FALSE (baseline.empty());

    const auto restoreLine = sendRestore (coord, baseline);
    REQUIRE (restoreLine.contains ("\"ok\":true"));

    const auto after = parseParams (sendSnapshot (coord));

    // Assert — same parameter set, same values. The margin absorbs
    // plugin-side normalized↔value float conversion noise (the restore path
    // re-injects the 6-decimal snapshot through setValueNotifyingHost).
    REQUIRE_FALSE (crashed.load());
    REQUIRE (after.size() == baseline.size());
    for (size_t i = 0; i < baseline.size(); ++i)
    {
        INFO ("param id=" << after[i].id
              << " baseline=" << baseline[i].value
              << " after=" << after[i].value);
        REQUIRE (after[i].id == baseline[i].id);
        REQUIRE (std::abs (after[i].value - baseline[i].value) < 1e-3);
    }

    coord.stop();
}

//==============================================================================
// R3 — restore_params with an empty list / unknown ids: {"ok":true}, no
//      crash, child stays alive (unknown ids are skipped per contract).
//==============================================================================

TEST_CASE ("ChildProtocol: restore_params tolerates empty list and unknown ids",
           "[childprotocol][restore]")
{
    // Arrange
    const auto pluginFile = realPluginFile();
    if (! pluginFile.exists())
        SKIP ("CHILD_PARITY_PLUGIN not present: " + pluginFile.getFullPathName());
    if (! realChildExe().existsAsFile())
        SKIP ("PLUGIN_HOST_CHILD_EXE not present: " + realChildExe().getFullPathName());

    PluginHostChildCoordinator coord (realChildExe().getFullPathName());
    std::atomic<bool> crashed { false };
    coord.setOnCrash ([&] (const juce::String&) { crashed.store (true); });
    REQUIRE (coord.start());
    REQUIRE (startAndLoad (coord, pluginFile));

    // Act — empty list, then a list containing only an unknown id.
    const auto emptyLine = sendRestore (coord, {});
    REQUIRE (emptyLine.isNotEmpty());
    REQUIRE (emptyLine.contains ("\"ok\":true"));

    const auto unknownLine = sendRestore (coord, {},
                                          R"({"id":"no.such.param","value":0.5})");
    REQUIRE (unknownLine.isNotEmpty());
    REQUIRE (unknownLine.contains ("\"ok\":true"));

    // Assert — no crash; a subsequent heartbeat roundtrip proves the child
    // is still alive (restore is a no-op for unknown ids, never a failure).
    REQUIRE_FALSE (crashed.load());
    REQUIRE (coord.sendLine (R"({"cmd":"heartbeat"})"));
    const auto heartbeatLine = popUntil (coord, "\"ok\":true", 3000);
    REQUIRE (heartbeatLine.isNotEmpty());
    REQUIRE (heartbeatLine.contains ("\"ok\":true"));

    coord.stop();
}
