/**
 * ChildProcessRestartTests (block D, D3b-3): end-to-end crash → restart →
 * resume-measurement integration against the REAL out-of-process host child
 * (PluginHostChild.exe, D1a+D1b) and the REAL crash-injection VST3 fixture
 * (SuicidePlugin, tests/SuicidePlugin — ADR-D-2).
 *
 * Flow under test (docs/plan-block-d-out-of-process.md D3b ticket + design
 * Q4): the child loads a host-killer plugin and dies mid-measure; the host
 * coordinator detects the crash (onCrash fires exactly once), the host-side
 * test process survives, restart() re-spawns the child, and a re-load +
 * re-measure succeeds (续测最终成功). R5-R7 add the D3 recovery half (T5):
 * the host caches the pre-crash snapshot_params response via cacheSnapshot()
 * and restores it through restore_params after the restart, so the parameter
 * state survives the crash.
 *
 * CRASH INJECTION (option C — zero production changes, no set_param command
 * needed): the crash is armed through the EXISTING restore_params command:
 *
 *   {"cmd":"restore_params","params":[{"id":"<crash_mode id>","value":0.5}]}
 *
 * → the child's handleRestoreParams calls setValueNotifyingHost(0.5) on the
 * hosted parameter → SuicidePlugin::CrashModeParameter::setValue maps it to
 * mode 1 → the NEXT processBlock (the first block of measure) calls
 * ExitProcess(1), which is immune to /EHa catch(...) (the whole point of the
 * fixture, ADR-D-2). The mode is re-read from an atomic every block, so
 * arming anytime works.
 *
 * PARAMETER ID NOTE: the id in the protocol is the VST3-hosted HASHeD ParamID
 * (a decimal string — juce_VST3PluginFormatImpl getParameterID returns
 * String(cachedInfo.id)), NOT the literal "crash_mode". The deterministic
 * mapping is juce::VST3ClientExtensions::convertJuceParameterId
 * (juce_VST3ClientExtensions.h:207), the same round-trip the D3b-2 fixture
 * verified. The JUCE VST3 wrapper auto-appends a Bypass parameter with the
 * legacy id 0x62797073 ('byps') — R4 guards that the snapshot/restore
 * roundtrip never flips it to true.
 *
 * R3 (load vs measure crash): the child protocol cannot arm a crash BEFORE
 * load (restore_params requires a loaded instance), and no processBlock runs
 * during load — so a load-phase crash is NOT injectable with the current
 * plugin/protocol. R3 therefore documents the boundary testably: arming
 * without measuring never kills; the crash belongs to measure's first block.
 * Load-phase crash injection is recorded as a D6 decision item in the team
 * report.
 *
 * Threading (coordinator contract): onCrash fires on the coordinator's reader
 * thread and restart() refuses to join the calling thread (returns false).
 * Every test therefore polls a std::atomic crash flag from the TEST thread
 * and calls restart() from the test thread — never from inside onCrash.
 *
 * If SUICIDE_PLUGIN / PLUGIN_HOST_CHILD_EXE is absent the tests SKIP (Catch2
 * SKIP semantics), never a fake substitute (tests/AGENTS.md real-plugin
 * exception; the SuicidePlugin is a registered build-time fixture).
 */
#include <catch2/catch_test_macros.hpp>

#include "../source/host/ChildProcessCoordinator.h"
#include "../source/child/ChildProtocol.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <thread>
#include <vector>

#ifndef PLUGIN_HOST_CHILD_EXE
#error "PLUGIN_HOST_CHILD_EXE must be defined by tests/CMakeLists.txt"
#endif

#ifndef SUICIDE_PLUGIN
#error "SUICIDE_PLUGIN must be defined by tests/CMakeLists.txt"
#endif

namespace
{
    //==============================================================================
    juce::File realChildExe()
    {
        return juce::File (juce::String (PLUGIN_HOST_CHILD_EXE));
    }

    juce::File suicidePluginFile()
    {
        return juce::File (juce::String (SUICIDE_PLUGIN));
    }

    juce::File tempFile (const juce::String& prefix, const juce::String& suffix)
    {
        return juce::File::getSpecialLocation (juce::File::SpecialLocationType::tempDirectory)
                   .getNonexistentChildFile (prefix, suffix);
    }

    /** Poll `flag` until it turns true or timeoutMs elapses. */
    bool waitFor (const std::atomic<bool>& flag, int timeoutMs)
    {
        const auto deadline = std::chrono::steady_clock::now()
                            + std::chrono::milliseconds (timeoutMs);
        while (! flag.load() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for (std::chrono::milliseconds (10));
        return flag.load();
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

    //==============================================================================
    // Protocol helpers (contract: docs/plan-block-d-out-of-process.md
    // "子进程 IPC 协议契约").

    /** The VST3-hosted (hashed) ParamID of the SuicidePlugin "crash_mode"
     *  parameter, as a decimal string — the id the protocol uses
     *  (juce_VST3ClientExtensions.h:207, deterministic round-trip verified by
     *  the D3b-2 fixture). */
    juce::String crashModeId()
    {
        return juce::String (juce::VST3ClientExtensions::convertJuceParameterId ("crash_mode", true));
    }

    /** start handshake + load the SuicidePlugin; false on any failure. */
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

    /** restore_params for a single stable-id entry → response line. */
    juce::String sendRestore (PluginHostChildCoordinator& coord,
                              const juce::String& id, double value)
    {
        const juce::String req = R"({"cmd":"restore_params","params":[{"id":")"
                               + ChildProtocol::escapeJsonString (id)
                               + R"(","value":)" + juce::String (value, 6) + "}]}";
        if (! coord.sendLine (req))
            return {};
        return popUntil (coord, "\"ok\"", 5000);
    }

    /** One snapshot entry: stable id + normalized value. */
    struct ParamEntry
    {
        juce::String id;
        double value = 0.0;
    };

    /** snapshot_params → response line. */
    juce::String sendSnapshot (PluginHostChildCoordinator& coord)
    {
        if (! coord.sendLine (R"({"cmd":"snapshot_params"})"))
            return {};
        return popUntil (coord, "\"params\":", 5000);
    }

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
                return {};
            const auto id = item["id"].toString();
            const auto valueVar = item["value"];
            if (id.isEmpty() || ! valueVar.isDouble())
                return {};
            entries.push_back ({ id, static_cast<double> (valueVar) });
        }
        return entries;
    }

    /** restore_params with a full snapshot list verbatim → response line. */
    juce::String sendRestoreList (PluginHostChildCoordinator& coord,
                                  const std::vector<ParamEntry>& entries)
    {
        juce::String req = R"({"cmd":"restore_params","params":[)";
        for (size_t i = 0; i < entries.size(); ++i)
        {
            if (i > 0)
                req += ",";
            req += R"({"id":")" + ChildProtocol::escapeJsonString (entries[i].id)
                 + R"(","value":)" + juce::String (entries[i].value, 6) + "}";
        }
        req += "]}";
        if (! coord.sendLine (req))
            return {};
        return popUntil (coord, "\"ok\"", 5000);
    }

    /** Send a measure request (fire-and-forget — the armed child dies on its
     *  first processBlock, so no response line is expected on the crash
     *  path). Returns sendLine success. */
    bool sendMeasure (PluginHostChildCoordinator& coord,
                      const juce::File& exportPath, const juce::File& wavPath)
    {
        juce::DynamicObject req;
        req.setProperty ("cmd", "measure");
        req.setProperty ("type", "frequency_response");
        req.setProperty ("excitation", "mls");   // short: 16383 samples ≈ 0.35 s
        req.setProperty ("sample_rate", 48000);
        req.setProperty ("block_size", 512);
        req.setProperty ("export_path", exportPath.getFullPathName());
        req.setProperty ("wav_path", wavPath.getFullPathName());
        const auto reqVar = juce::var (new juce::DynamicObject (req));
        return coord.sendLine (juce::JSON::toString (reqVar, true));
    }
}  // namespace

//==============================================================================
// R1 — core acceptance: spawn real child → load SuicidePlugin → arm
//      crash_mode=0.5 via restore_params → measure → ExitProcess crash →
//      onCrash exactly once + child dead + host-side test process alive →
//      restart() → re-load + disarm → measure succeeds (续测最终成功).
//==============================================================================

TEST_CASE ("ChildProcessRestart: crash then restart resumes measurement",
           "[childrestart][crash][restart]")
{
    // Preconditions — real crash plugin + real child exe (no fake substitutes).
    // A .vst3 is a DIRECTORY, so exists() (not existsAsFile()) is the check.
    const auto pluginFile = suicidePluginFile();
    if (! pluginFile.exists())
        SKIP ("SUICIDE_PLUGIN not present: " + pluginFile.getFullPathName());
    if (! realChildExe().existsAsFile())
        SKIP ("PLUGIN_HOST_CHILD_EXE not present: " + realChildExe().getFullPathName());

    PluginHostChildCoordinator coord (realChildExe().getFullPathName());
    std::atomic<bool> crashed { false };
    juce::String crashDetail;
    coord.setOnCrash ([&] (const juce::String& detail) {
        crashDetail = detail;      // assign BEFORE the flag store (happens-before)
        crashed.store (true);
    });

    // ── First life: spawn + load + arm the crash (option C) ──
    REQUIRE (coord.start());
    REQUIRE (startAndLoad (coord, pluginFile));
    REQUIRE (sendRestore (coord, crashModeId(), 0.5).contains ("\"ok\":true"));

    const auto exportPath = tempFile ("pluginlab_restart_r1_", ".json");
    const auto wavPath = tempFile ("pluginlab_restart_r1_", ".wav");

    // ── Act: measure → first processBlock calls ExitProcess(1) ──
    REQUIRE (sendMeasure (coord, exportPath, wavPath));
    REQUIRE (waitFor (crashed, 10000));

    // ── Assert — exactly one crash, child gone, THIS process survived ──
    REQUIRE (coord.crashCount() == 1);
    REQUIRE_FALSE (coord.isRunning());
    REQUIRE (crashDetail.contains ("code 1"));   // exit-code path, not heartbeat

    // ── Restart from the TEST thread (never from inside onCrash) ──
    REQUIRE (coord.restart());
    REQUIRE (coord.isRunning());

    // ── Resume: re-load + disarm (crash_mode=0) + measure succeeds ──
    REQUIRE (startAndLoad (coord, pluginFile));
    REQUIRE (sendRestore (coord, crashModeId(), 0.0).contains ("\"ok\":true"));
    crashed.store (false);
    REQUIRE (sendMeasure (coord, exportPath, wavPath));

    const auto resumeLine = popUntil (coord, "\"samples\"", 60000);
    REQUIRE (resumeLine.isNotEmpty());
    REQUIRE (resumeLine.contains ("\"ok\":true"));
    REQUIRE_FALSE (crashed.load());              // no false crash on the fresh child
    REQUIRE (coord.crashCount() == 1);           // still exactly one — no double count
    REQUIRE (coord.isRunning());

    coord.stop();
    exportPath.deleteFile();
    wavPath.deleteFile();
}

//==============================================================================
// R2 — three consecutive crashes: crashCount() reaches 3 (the D6 upper-layer
//      maps ≥3 to {"ok":false,"error":"child process crashed (restarting)"}).
//==============================================================================

TEST_CASE ("ChildProcessRestart: three consecutive crashes reach crashCount 3",
           "[childrestart][crash][limit]")
{
    const auto pluginFile = suicidePluginFile();
    if (! pluginFile.exists())
        SKIP ("SUICIDE_PLUGIN not present: " + pluginFile.getFullPathName());
    if (! realChildExe().existsAsFile())
        SKIP ("PLUGIN_HOST_CHILD_EXE not present: " + realChildExe().getFullPathName());

    PluginHostChildCoordinator coord (realChildExe().getFullPathName());
    std::atomic<bool> crashed { false };
    coord.setOnCrash ([&] (const juce::String&) { crashed.store (true); });

    const auto exportPath = tempFile ("pluginlab_restart_r2_", ".json");
    const auto wavPath = tempFile ("pluginlab_restart_r2_", ".wav");

    REQUIRE (coord.start());

    // Act — three full crash cycles: load → arm → measure → crash → restart.
    for (int i = 0; i < 3; ++i)
    {
        crashed.store (false);
        REQUIRE (startAndLoad (coord, pluginFile));
        REQUIRE (sendRestore (coord, crashModeId(), 0.5).contains ("\"ok\":true"));
        REQUIRE (sendMeasure (coord, exportPath, wavPath));
        REQUIRE (waitFor (crashed, 10000));

        // Assert — exactly one report per crash, cumulative counter.
        REQUIRE_FALSE (coord.isRunning());
        REQUIRE (coord.crashCount() == i + 1);

        if (i < 2)
            REQUIRE (coord.restart());   // the third crash stays unrecovered
    }

    // Assert — crashCount()==3 is the coordinator-level limit signal; the
    // D6 routing layer maps it to the error response.
    REQUIRE (coord.crashCount() == 3);

    coord.stop();   // no-op: nothing running
    exportPath.deleteFile();
    wavPath.deleteFile();
}

//==============================================================================
// R3 — load-phase vs measure-phase crash: arming (restore_params) alone never
//      kills — the crash fires on measure's first processBlock. Load-phase
//      injection is infeasible with the current protocol (restore_params
//      needs a loaded instance; no processBlock runs during load) — recorded
//      as a D6 decision item in the team report.
//==============================================================================

TEST_CASE ("ChildProcessRestart: crash fires on measure processing, not on arming",
           "[childrestart][crash][loadvsmeasure]")
{
    const auto pluginFile = suicidePluginFile();
    if (! pluginFile.exists())
        SKIP ("SUICIDE_PLUGIN not present: " + pluginFile.getFullPathName());
    if (! realChildExe().existsAsFile())
        SKIP ("PLUGIN_HOST_CHILD_EXE not present: " + realChildExe().getFullPathName());

    PluginHostChildCoordinator coord (realChildExe().getFullPathName());
    std::atomic<bool> crashed { false };
    juce::String crashDetail;
    coord.setOnCrash ([&] (const juce::String& detail) {
        crashDetail = detail;
        crashed.store (true);
    });

    const auto exportPath = tempFile ("pluginlab_restart_r3_", ".json");
    const auto wavPath = tempFile ("pluginlab_restart_r3_", ".wav");

    REQUIRE (coord.start());
    REQUIRE (startAndLoad (coord, pluginFile));

    // Act — arm right after load, WITHOUT measuring. The armed mode is only
    // read by processBlock, which only measure triggers — so the child must
    // stay alive and quiet (800 ms is well inside the 3 s heartbeat window).
    REQUIRE (sendRestore (coord, crashModeId(), 0.5).contains ("\"ok\":true"));
    std::this_thread::sleep_for (std::chrono::milliseconds (800));

    // Assert — arming alone never kills.
    REQUIRE_FALSE (crashed.load());
    REQUIRE (coord.isRunning());

    // Act — the same armed state kills on the first measure block.
    crashed.store (false);
    REQUIRE (sendMeasure (coord, exportPath, wavPath));
    REQUIRE (waitFor (crashed, 10000));

    // Assert — measure-phase crash, exit-code path, exactly once.
    REQUIRE (crashDetail.contains ("code 1"));
    REQUIRE (coord.crashCount() == 1);
    REQUIRE_FALSE (coord.isRunning());

    coord.stop();
    exportPath.deleteFile();
    wavPath.deleteFile();
}

//==============================================================================
// R4 — Bypass caution: the snapshot/restore roundtrip restores the
//      wrapper-provided Bypass parameter ("byps", legacy id 0x62797073)
//      correctly — it is never accidentally flipped true while arming /
//      disarming crash_mode.
//==============================================================================

TEST_CASE ("ChildProcessRestart: snapshot roundtrip restores Bypass correctly",
           "[childrestart][restore][bypass]")
{
    const auto pluginFile = suicidePluginFile();
    if (! pluginFile.exists())
        SKIP ("SUICIDE_PLUGIN not present: " + pluginFile.getFullPathName());
    if (! realChildExe().existsAsFile())
        SKIP ("PLUGIN_HOST_CHILD_EXE not present: " + realChildExe().getFullPathName());

    PluginHostChildCoordinator coord (realChildExe().getFullPathName());
    std::atomic<bool> crashed { false };
    coord.setOnCrash ([&] (const juce::String&) { crashed.store (true); });

    REQUIRE (coord.start());
    REQUIRE (startAndLoad (coord, pluginFile));

    // Find the Bypass entry: the VST3 wrapper adds AudioParameterBool
    // ("byps", ...) with the legacy ParamID 0x62797073 ('byps').
    const juce::String bypassId (juce::String ((juce::uint32) 0x62797073));

    // Baseline snapshot: crash_mode(0) + Bypass(0).
    const auto baseline = parseParams (sendSnapshot (coord));
    REQUIRE_FALSE (baseline.empty());

    const auto bypsIn = [&] (const std::vector<ParamEntry>& entries) -> double
    {
        for (const auto& entry : entries)
            if (entry.id == bypassId)
                return entry.value;
        return -1.0;   // missing → fail loudly
    };

    // Act — arm crash_mode=0.5, snapshot again: only crash_mode changed.
    REQUIRE (sendRestore (coord, crashModeId(), 0.5).contains ("\"ok\":true"));
    const auto armed = parseParams (sendSnapshot (coord));
    REQUIRE_FALSE (armed.empty());

    double crashModeBaseline = -1.0;
    double crashModeArmed = -1.0;
    for (const auto& entry : baseline)
        if (entry.id == crashModeId())
            crashModeBaseline = entry.value;
    for (const auto& entry : armed)
        if (entry.id == crashModeId())
            crashModeArmed = entry.value;
    REQUIRE (std::abs (crashModeArmed - 0.5) < 1e-3);          // arming took effect
    REQUIRE (std::abs (crashModeBaseline - 0.0) < 1e-3);       // baseline was passthrough
    REQUIRE (std::abs (bypsIn (armed) - 0.0) < 1e-3);          // Bypass NOT flipped true

    // Act — restore the baseline snapshot verbatim, snapshot again.
    REQUIRE (sendRestoreList (coord, baseline).contains ("\"ok\":true"));
    const auto after = parseParams (sendSnapshot (coord));

    // Assert — every parameter returned to its baseline value; Bypass stays 0.
    REQUIRE (after.size() == baseline.size());
    for (size_t i = 0; i < baseline.size(); ++i)
    {
        INFO ("param id=" << after[i].id
              << " baseline=" << baseline[i].value
              << " after=" << after[i].value);
        REQUIRE (after[i].id == baseline[i].id);
        REQUIRE (std::abs (after[i].value - baseline[i].value) < 1e-3);
    }
    REQUIRE (std::abs (bypsIn (after) - 0.0) < 1e-3);
    REQUIRE_FALSE (crashed.load());                            // never armed a crash

    coord.stop();
}

//==============================================================================
// R5 — D3 recovery chain (core): host caches the pre-crash snapshot_params
//      response via cacheSnapshot(), the child crashes, restart() re-spawns,
//      the D6-style driver restores the CACHED snapshot through restore_params,
//      a re-snapshot equals the pre-crash state (params survive the crash),
//      and a disarmed measure succeeds (续测).
//==============================================================================

TEST_CASE ("ChildProcessRestart: cached snapshot restores params across crash",
           "[childrestart][restore][snapshot][crash]")
{
    const auto pluginFile = suicidePluginFile();
    if (! pluginFile.exists())
        SKIP ("SUICIDE_PLUGIN not present: " + pluginFile.getFullPathName());
    if (! realChildExe().existsAsFile())
        SKIP ("PLUGIN_HOST_CHILD_EXE not present: " + realChildExe().getFullPathName());

    PluginHostChildCoordinator coord (realChildExe().getFullPathName());
    std::atomic<bool> crashed { false };
    juce::String crashDetail;
    coord.setOnCrash ([&] (const juce::String& detail) {
        crashDetail = detail;
        crashed.store (true);
    });

    const auto exportPath = tempFile ("pluginlab_restart_r5_", ".json");
    const auto wavPath = tempFile ("pluginlab_restart_r5_", ".wav");

    // ── First life: spawn + load, then CACHE the pre-crash state ──
    // (crash_mode=0 + Bypass=false by default — the D6 driver would cache the
    // last snapshot before each command, exactly like this.)
    REQUIRE (coord.start());
    REQUIRE (startAndLoad (coord, pluginFile));

    const auto rawSnapshotLine = sendSnapshot (coord);
    REQUIRE_FALSE (rawSnapshotLine.isEmpty());
    coord.cacheSnapshot (rawSnapshotLine);   // host-side recovery state
    REQUIRE (coord.lastSnapshot().isNotEmpty());
    const auto preCrash = parseParams (rawSnapshotLine);
    REQUIRE_FALSE (preCrash.empty());

    // ── Arm + crash (measure's first processBlock calls ExitProcess(1)) ──
    REQUIRE (sendRestore (coord, crashModeId(), 0.5).contains ("\"ok\":true"));
    REQUIRE (sendMeasure (coord, exportPath, wavPath));
    REQUIRE (waitFor (crashed, 10000));
    REQUIRE (coord.crashCount() == 1);
    REQUIRE_FALSE (coord.isRunning());
    REQUIRE (crashDetail.contains ("code 1"));

    // ── Restart + re-load ──
    REQUIRE (coord.restart());
    REQUIRE (coord.isRunning());
    REQUIRE (startAndLoad (coord, pluginFile));

    // ── Auto-restore sequence (D6 routing layer driver — the coordinator
    //    stays pure: restart() re-spawns only; restore_params must follow a
    //    successful load, which only the caller can sequence). ──
    REQUIRE (coord.lastSnapshot().isNotEmpty());   // survived the crash host-side
    const juce::String restoreReq = R"({"cmd":"restore_params","params":)"
                                  + coord.lastSnapshot() + "}";
    REQUIRE (coord.sendLine (restoreReq));
    REQUIRE (popUntil (coord, "\"ok\":true", 5000).isNotEmpty());

    // ── Assert — post-restore snapshot equals the pre-crash one (params
    //    preserved across the crash; Bypass back to false). ──
    const auto postCrash = parseParams (sendSnapshot (coord));
    REQUIRE (postCrash.size() == preCrash.size());
    for (size_t i = 0; i < preCrash.size(); ++i)
    {
        INFO ("param id=" << postCrash[i].id
              << " pre-crash=" << preCrash[i].value
              << " post-restore=" << postCrash[i].value);
        REQUIRE (postCrash[i].id == preCrash[i].id);
        REQUIRE (std::abs (postCrash[i].value - preCrash[i].value) < 1e-3);
    }

    // ── Resume: disarm + measure succeeds (续测最终成功) ──
    REQUIRE (sendRestore (coord, crashModeId(), 0.0).contains ("\"ok\":true"));
    crashed.store (false);
    REQUIRE (sendMeasure (coord, exportPath, wavPath));
    const auto resumeLine = popUntil (coord, "\"samples\"", 60000);
    REQUIRE (resumeLine.isNotEmpty());
    REQUIRE (resumeLine.contains ("\"ok\":true"));
    REQUIRE_FALSE (crashed.load());              // no false crash on the fresh child
    REQUIRE (coord.crashCount() == 1);           // still exactly one

    coord.stop();
    exportPath.deleteFile();
    wavPath.deleteFile();
}

//==============================================================================
// R6 — Bypass guard through the cache/restore path: the wrapper-attached
//      Bypass parameter (legacy id 0x62797073) stays false across the crash
//      and the cached-snapshot restore (a naive restore must never bypass).
//==============================================================================

TEST_CASE ("ChildProcessRestart: Bypass stays false through crash and restore",
           "[childrestart][restore][bypass][crash]")
{
    const auto pluginFile = suicidePluginFile();
    if (! pluginFile.exists())
        SKIP ("SUICIDE_PLUGIN not present: " + pluginFile.getFullPathName());
    if (! realChildExe().existsAsFile())
        SKIP ("PLUGIN_HOST_CHILD_EXE not present: " + realChildExe().getFullPathName());

    PluginHostChildCoordinator coord (realChildExe().getFullPathName());
    std::atomic<bool> crashed { false };
    coord.setOnCrash ([&] (const juce::String&) { crashed.store (true); });

    const auto exportPath = tempFile ("pluginlab_restart_r6_", ".json");
    const auto wavPath = tempFile ("pluginlab_restart_r6_", ".wav");
    const juce::String bypassId (juce::String ((juce::uint32) 0x62797073));

    REQUIRE (coord.start());
    REQUIRE (startAndLoad (coord, pluginFile));

    // Cache the pre-crash state (Bypass=false by default), then arm + crash.
    const auto rawSnapshotLine = sendSnapshot (coord);
    coord.cacheSnapshot (rawSnapshotLine);
    REQUIRE (sendRestore (coord, crashModeId(), 0.5).contains ("\"ok\":true"));
    REQUIRE (sendMeasure (coord, exportPath, wavPath));
    REQUIRE (waitFor (crashed, 10000));
    REQUIRE (coord.crashCount() == 1);

    // Restart + re-load + restore the CACHED snapshot.
    REQUIRE (coord.restart());
    REQUIRE (coord.isRunning());
    REQUIRE (startAndLoad (coord, pluginFile));
    crashed.store (false);   // the crash is history — watch only the post-restore window
    REQUIRE (coord.sendLine (R"({"cmd":"restore_params","params":)"
                             + coord.lastSnapshot() + "}"));
    REQUIRE (popUntil (coord, "\"ok\":true", 5000).isNotEmpty());

    // Assert — Bypass explicitly false, crash_mode back to 0 (passthrough).
    const auto after = parseParams (sendSnapshot (coord));
    REQUIRE_FALSE (after.empty());
    double bypassValue = -1.0;
    double crashModeValue = -1.0;
    for (const auto& entry : after)
    {
        if (entry.id == bypassId)
            bypassValue = entry.value;
        if (entry.id == crashModeId())
            crashModeValue = entry.value;
    }
    REQUIRE (std::abs (bypassValue - 0.0) < 1e-3);            // Bypass NOT flipped
    REQUIRE (std::abs (crashModeValue - 0.0) < 1e-3);          // passthrough restored
    REQUIRE_FALSE (crashed.load());                            // restore never armed

    coord.stop();
    exportPath.deleteFile();
    wavPath.deleteFile();
}

//==============================================================================
// R7 — cacheSnapshot()/lastSnapshot() pure unit roundtrip (no child/plugin):
//      full response line → params fragment extracted; bare array → verbatim;
//      unparseable → empty; initial state empty.
//==============================================================================

TEST_CASE ("ChildProcessRestart: cacheSnapshot/lastSnapshot roundtrip",
           "[childrestart][snapshot][cache]")
{
    // No child or plugin needed — pure coordinator API.
    PluginHostChildCoordinator coord ("C:\\nonexistent\\NoSuchChild.exe");

    // Empty semantics — nothing cached yet.
    REQUIRE (coord.lastSnapshot().isEmpty());

    // Full snapshot_params response line → params array fragment extracted
    // (a parseable array, directly spliceable into restore_params). The
    // parse-back assertions are format-independent (JUCE single-line JSON
    // may space after colons) and verify the restore contract itself.
    coord.cacheSnapshot (R"({"ok":true,"params":[{"id":"a","value":0.500000},{"id":"byps","value":0.000000}]})");
    const auto parsed = juce::JSON::parse (coord.lastSnapshot());
    REQUIRE (parsed.isArray());
    REQUIRE (parsed.size() == 2);
    REQUIRE (parsed[0]["id"].toString() == "a");
    REQUIRE (std::abs (static_cast<double> (parsed[0]["value"]) - 0.5) < 1e-6);
    REQUIRE (parsed[1]["id"].toString() == "byps");
    REQUIRE (std::abs (static_cast<double> (parsed[1]["value"]) - 0.0) < 1e-6);
    REQUIRE_FALSE (coord.lastSnapshot().contains ("\"ok\""));   // wrapper stripped

    // A bare params array is stored verbatim (no double-wrap).
    coord.cacheSnapshot (R"([{"id":"x","value":0.1}])");
    const auto parsedBare = juce::JSON::parse (coord.lastSnapshot());
    REQUIRE (parsedBare.isArray());
    REQUIRE (parsedBare.size() == 1);
    REQUIRE (parsedBare[0]["id"].toString() == "x");

    // Unparseable input → cache cleared to empty (no recoverable snapshot).
    coord.cacheSnapshot ("garbage{{{not json");
    REQUIRE (coord.lastSnapshot().isEmpty());

    // Re-cache after a failed parse works normally.
    coord.cacheSnapshot (R"({"ok":true,"params":[{"id":"y","value":1.0}]})");
    REQUIRE (juce::JSON::parse (coord.lastSnapshot()).isArray());
}
