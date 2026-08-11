/**
 * ChildMeasureOrchestrator tests (block D, D6): the host-side orchestrator
 * that drives one frequency_response measurement through the child with the
 * D3 crash-recovery sequence (restart → start → load → restore_params →
 * measure) and maps the child's result line onto ChildMeasureResult.
 *
 * The orchestrator is exercised END-TO-END against the real coordinator
 * (PluginHostChildCoordinator) + the TestChildProcess JSON-line double
 * (tests/TestChildProcess.cpp, extended with load / restore_params / measure
 * responses and a received-commands log for sequence assertions) — the same
 * pattern as ChildProcessCoordinatorTests. The measure behavior override
 * (progress lines / silent child / mid-measure crash) travels in a mode file
 * because the orchestrator spawns a FRESH child per run (a set command would
 * die with the previous process).
 *
 * R3 / R7 (the crash-loop gate, design Q4 / D3b) run the REAL coordinator +
 * TestChildProcess in crash_on_measure mode (the child exits 3 on measure —
 * a mid-measure crash): R3 drives three real consecutive crashes then a
 * refused run; R7 proves a successful run RESETS the streak (crash →
 * success → crash ×3 → refused). crashCount is virtual (the D6 test seam,
 * see ChildProcessCoordinator.h) but production never overrides it.
 */
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "../source/host/ChildMeasureOrchestrator.h"
#include "../source/host/ChildProcessCoordinator.h"

#include <atomic>
#include <vector>

#ifndef TEST_CHILD_EXE
#error "TEST_CHILD_EXE must be defined by tests/CMakeLists.txt"
#endif

namespace
{
    juce::File testChildExe()
    {
        return juce::File (juce::String (TEST_CHILD_EXE));
    }

    juce::File tempFile (const juce::String& prefix, const juce::String& suffix)
    {
        return juce::File::getSpecialLocation (juce::File::SpecialLocationType::tempDirectory)
                   .getNonexistentChildFile (prefix, suffix);
    }

    /** The measure-mode override file TestChildProcess reads (see
        tests/TestChildProcess.cpp header comment). */
    juce::File modeFile()
    {
        return juce::File::getSpecialLocation (juce::File::SpecialLocationType::tempDirectory)
                   .getChildFile ("pluginlab_test_child_mode.txt");
    }

    /** One frequency_response request with deterministic paths. */
    ChildMeasureContract::ChildMeasureRequest sweepRequest (const juce::File& exportPath,
                                                            const juce::File& wavPath)
    {
        ChildMeasureContract::ChildMeasureRequest request;
        request.type = "frequency_response";
        request.excitation = "sweep";
        request.sampleRate = 48000.0;
        request.blockSize = 512;
        request.exportPath = exportPath.getFullPathName();
        request.wavPath = wavPath.getFullPathName();
        return request;
    }

    /** Ask the child which commands it received (single-line response). */
    juce::String askReceived (PluginHostChildCoordinator& coord)
    {
        coord.sendLine (R"({"cmd":"received"})");
        return coord.popLine (3000);
    }

    /** Parse the {"ok":true,"received":[...]} response → command list. */
    std::vector<juce::String> parseReceived (const juce::String& line)
    {
        std::vector<juce::String> cmds;
        const auto doc = juce::JSON::parse (line);
        if (! doc.isObject())
            return cmds;
        const auto received = doc["received"];
        if (! received.isArray())
            return cmds;
        cmds.reserve (static_cast<size_t> (received.size()));
        for (int i = 0; i < received.size(); ++i)
            cmds.push_back (received[i].toString());
        return cmds;
    }
}  // namespace

//==============================================================================
// R1 — core: recovery sequence restart→start→load→restore_params→measure
//      reaches the child in order and every result field maps correctly.
//==============================================================================

TEST_CASE ("ChildMeasureOrchestrator: recovery sequence drives child and maps result fields",
           "[childorchestrator][measure][happy]")
{
    // Arrange — real coordinator + TestChildProcess; the pre-crash parameter
    // snapshot is cached so the recovery sequence must splice restore_params.
    REQUIRE (testChildExe().existsAsFile());
    // A crashed/interrupted earlier test may have left a measure-mode
    // override behind — this test expects the child's DEFAULT behavior.
    modeFile().deleteFile();
    PluginHostChildCoordinator coord (testChildExe().getFullPathName());
    std::atomic<bool> crashed { false };
    juce::String crashDetail;
    coord.setOnCrash ([&] (const juce::String& detail) {
        crashDetail = detail;
        crashed.store (true);
    });
    coord.cacheSnapshot (R"({"ok":true,"params":[{"id":"a","value":0.5}]})");
    REQUIRE (coord.lastSnapshot().isNotEmpty());

    ChildMeasureOrchestrator orchestrator (&coord, "C:\\fake\\plugin.vst3");
    const auto exportPath = tempFile ("pluginlab_orch_r1_", ".json");
    const auto wavPath = tempFile ("pluginlab_orch_r1_", ".wav");

    // Act
    const auto outcome = orchestrator.run (sweepRequest (exportPath, wavPath));

    // Assert — success + field mapping (result line → ChildMeasureResult).
    INFO ("outcome.error: " << outcome.error);
    INFO ("crashDetail: " << crashDetail);
    REQUIRE (outcome.ok);
    REQUIRE (outcome.error.isEmpty());
    REQUIRE (outcome.result.samples == 240000);
    REQUIRE (outcome.result.rate == Catch::Approx (48000.0));
    REQUIRE (outcome.result.exportPath == exportPath.getFullPathName());
    REQUIRE (outcome.result.wavPath == wavPath.getFullPathName());
    REQUIRE (outcome.result.name == "FakeChildPlugin");
    REQUIRE (outcome.result.classId == "fake.class");
    REQUIRE (outcome.result.channels == 2);
    REQUIRE (outcome.result.latencySamples == 0);

    // Assert — the sequence reached the child in order (restart is the
    // coordinator-side spawn, proven by isRunning); the result was found
    // despite the child's progress line; no crash was ever reported.
    REQUIRE (parseReceived (askReceived (coord))
             == std::vector<juce::String> ({ "start", "load", "restore_params", "measure" }));
    REQUIRE (coord.isRunning());
    REQUIRE_FALSE (crashed.load());
    REQUIRE (coord.crashCount() == 0);

    coord.stop();
    exportPath.deleteFile();
    wavPath.deleteFile();
}

//==============================================================================
// R2 — ADR-D-7: a non-frequency_response type fails with the not-implemented
//      error and NEVER touches the child (no restart, no spawn, no measure).
//==============================================================================

TEST_CASE ("ChildMeasureOrchestrator: non-frequency types fail without touching the child",
           "[childorchestrator][notimplemented]")
{
    // Arrange
    REQUIRE (testChildExe().existsAsFile());
    PluginHostChildCoordinator coord (testChildExe().getFullPathName());
    ChildMeasureOrchestrator orchestrator (&coord, "C:\\fake\\plugin.vst3");

    ChildMeasureContract::ChildMeasureRequest request;
    request.type = "harmonic";

    // Act
    const auto outcome = orchestrator.run (request);

    // Assert — ADR-D-7 error vocabulary; no child interaction of any kind.
    REQUIRE_FALSE (outcome.ok);
    REQUIRE (outcome.error == "child measurement not implemented for type 'harmonic'");
    REQUIRE (outcome.result.samples == 0);
    REQUIRE_FALSE (coord.isRunning());   // never spawned a child
    REQUIRE (coord.crashCount() == 0);
}

//==============================================================================
// Cancel (issue #3): a user stop during the child's measure wait must return
// {"ok":false,"error":"cancelled"} WITHOUT touching the crash-loop gate or
// crashCount — a deliberate stop is NOT a crash event (D3 semantics).
//==============================================================================

TEST_CASE ("ChildMeasureOrchestrator: cancel during measure returns cancelled, no crash count",
           "[childorchestrator][cancel]")
{
    // Arrange — progress_only: the child never answers the measure result,
    // so run() blocks in waitForLine until the cancel is honoured.
    REQUIRE (testChildExe().existsAsFile());
    modeFile().replaceWithText ("progress_only");

    PluginHostChildCoordinator coord (testChildExe().getFullPathName());
    std::atomic<bool> crashed { false };
    coord.setOnCrash ([&] (const juce::String&) { crashed.store (true); });
    ChildMeasureOrchestrator orchestrator (&coord, "C:\\fake\\plugin.vst3", 5000);
    const auto exportPath = tempFile ("pluginlab_orch_cancel_", ".json");
    const auto wavPath = tempFile ("pluginlab_orch_cancel_", ".wav");

    // A canceller thread fires coordinator->requestCancel() while run() is
    // blocked waiting for the measure result line.
    std::thread canceller ([&]
    {
        juce::Thread::sleep (300);
        coord.requestCancel();
    });

    // Act
    const auto outcome = orchestrator.run (sweepRequest (exportPath, wavPath));
    canceller.join();

    // Assert — deliberate cancel: "cancelled" vocabulary, the child is
    // stopped deliberately (no crash report, no crashCount bump).
    INFO ("outcome.error: " << outcome.error);
    REQUIRE_FALSE (outcome.ok);
    REQUIRE (outcome.error == "cancelled");
    REQUIRE_FALSE (crashed.load());
    REQUIRE (coord.crashCount() == 0);
    REQUIRE_FALSE (coord.isRunning());

    exportPath.deleteFile();
    wavPath.deleteFile();
}

//==============================================================================
// R3 — crash-loop gate (design Q4 / D3b): 连续崩溃 ≥3 次（跨 run 累积，baseline
//      = 上次成功 run 后的 crashCount）→ 第 4 次 run 在碰子进程之前直接拒绝，
//      {"ok":false,"error":"child process crashed (restarting)"}。Runs 1-3 让
//      TestChildProcess 真实崩溃（crash_on_measure → exit 3），run 4 必须被
//      gate 拒绝且不 spawn（spawn 会再崩一次把 crashCount 推到 4）。
//==============================================================================

TEST_CASE ("ChildMeasureOrchestrator: 3 consecutive crashes refuse the 4th run",
           "[childorchestrator][crashlimit]")
{
    // Arrange — the child dies on every measure (exit 3, mid-measure crash).
    REQUIRE (testChildExe().existsAsFile());
    modeFile().replaceWithText ("crash_on_measure");

    PluginHostChildCoordinator coord (testChildExe().getFullPathName());
    std::atomic<bool> crashed { false };
    coord.setOnCrash ([&] (const juce::String&) { crashed.store (true); });
    ChildMeasureOrchestrator orchestrator (&coord, "C:\\fake\\plugin.vst3");
    const auto exportPath = tempFile ("pluginlab_orch_r3_", ".json");
    const auto wavPath = tempFile ("pluginlab_orch_r3_", ".wav");

    // Act — three crashing runs: each crash is detected and counted exactly
    // once, and the D3b vocabulary surfaces (测量中的崩溃 → 该错误串).
    for (int run = 1; run <= 3; ++run)
    {
        const auto outcome = orchestrator.run (sweepRequest (exportPath, wavPath));
        INFO ("run " << run << " error: " << outcome.error);
        REQUIRE_FALSE (outcome.ok);
        REQUIRE (outcome.error == "child process crashed (restarting)");
        REQUIRE (coord.crashCount() == run);
        REQUIRE (crashed.load());
    }

    // Act — the 4th run is refused at the gate BEFORE spawning:
    // crashCount() - crashBaseline == 3 - 0 >= kMaxChildCrashes.
    const auto refused = orchestrator.run (sweepRequest (exportPath, wavPath));

    // Assert — refusal with the D3b vocabulary and NO new child interaction
    // (a spawn would have crashed again and pushed crashCount to 4).
    REQUIRE_FALSE (refused.ok);
    REQUIRE (refused.error == "child process crashed (restarting)");
    REQUIRE (coord.crashCount() == 3);
    REQUIRE_FALSE (coord.isRunning());

    coord.stop();   // no-op — the last child already died
    modeFile().deleteFile();
    exportPath.deleteFile();
    wavPath.deleteFile();
}

//==============================================================================
// R7 — the crash-loop gate counts CONSECUTIVE crashes only: a successful run
//      resets the baseline (crashBaseline = crashCount()), so crashes on
//      either side of a success never accumulate toward the ≥3 refusal.
//      Sequence: crash → SUCCESS → crash ×3 → refused. Without the reset
//      (baseline stuck at 0) run 4 would already be refused (3 - 0 >= 3);
//      with it, three fresh crashes past the reset are required.
//==============================================================================

TEST_CASE ("ChildMeasureOrchestrator: successful run resets the crash streak",
           "[childorchestrator][crashlimit][reset]")
{
    REQUIRE (testChildExe().existsAsFile());

    PluginHostChildCoordinator coord (testChildExe().getFullPathName());
    ChildMeasureOrchestrator orchestrator (&coord, "C:\\fake\\plugin.vst3");
    const auto exportPath = tempFile ("pluginlab_orch_r7_", ".json");
    const auto wavPath = tempFile ("pluginlab_orch_r7_", ".wav");

    // Run 1 — crash: the streak starts (crashCount 1, baseline still 0).
    modeFile().replaceWithText ("crash_on_measure");
    const auto first = orchestrator.run (sweepRequest (exportPath, wavPath));
    REQUIRE_FALSE (first.ok);
    REQUIRE (first.error == "child process crashed (restarting)");
    REQUIRE (coord.crashCount() == 1);

    // Run 2 — SUCCESS: resets the baseline to crashCount() == 1.
    modeFile().deleteFile();
    const auto reset = orchestrator.run (sweepRequest (exportPath, wavPath));
    REQUIRE (reset.ok);
    REQUIRE (coord.crashCount() == 1);

    // Runs 3-5 — crash again: the new streak builds from the reset baseline
    // (crashCount - crashBaseline goes 0 → 1 → 2 across these three runs).
    modeFile().replaceWithText ("crash_on_measure");
    for (int run = 3; run <= 5; ++run)
    {
        const auto outcome = orchestrator.run (sweepRequest (exportPath, wavPath));
        INFO ("run " << run << " error: " << outcome.error);
        REQUIRE_FALSE (outcome.ok);
        REQUIRE (outcome.error == "child process crashed (restarting)");
        REQUIRE (coord.crashCount() == run - 1);   // 2, 3, 4 — one per run
    }

    // Run 6 — refused: 3 crashes since the reset (4 - 1 >= 3), no new spawn.
    const auto refused = orchestrator.run (sweepRequest (exportPath, wavPath));
    REQUIRE_FALSE (refused.ok);
    REQUIRE (refused.error == "child process crashed (restarting)");
    REQUIRE (coord.crashCount() == 4);   // no spawn → no new crash

    coord.stop();
    modeFile().deleteFile();
    exportPath.deleteFile();
    wavPath.deleteFile();
}

//==============================================================================
// R4 — restore_params is spliced into the sequence ONLY when the cached
//      snapshot is non-empty; and the orchestrator is repeatable (a second
//      run stops the live child and succeeds again).
//==============================================================================

TEST_CASE ("ChildMeasureOrchestrator: restore_params sent only when a snapshot is cached",
           "[childorchestrator][restore]")
{
    REQUIRE (testChildExe().existsAsFile());

    // Scenario A — no cached snapshot: restore_params must be skipped.
    {
        modeFile().deleteFile();   // default child behavior, no mode residue
        PluginHostChildCoordinator coord (testChildExe().getFullPathName());
        ChildMeasureOrchestrator orchestrator (&coord, "C:\\fake\\plugin.vst3");
        const auto exportPath = tempFile ("pluginlab_orch_r4a_", ".json");
        const auto wavPath = tempFile ("pluginlab_orch_r4a_", ".wav");

        const auto outcome = orchestrator.run (sweepRequest (exportPath, wavPath));
        REQUIRE (outcome.ok);
        REQUIRE (parseReceived (askReceived (coord))
                 == std::vector<juce::String> ({ "start", "load", "measure" }));

        coord.stop();
        exportPath.deleteFile();
        wavPath.deleteFile();
    }

    // Scenario B — cached snapshot: restore_params is spliced in; and a
    // second run on the SAME orchestrator (child still alive) succeeds
    // again via the stop-if-running preamble.
    {
        PluginHostChildCoordinator coord (testChildExe().getFullPathName());
        coord.cacheSnapshot (R"({"ok":true,"params":[{"id":"a","value":0.5}]})");
        ChildMeasureOrchestrator orchestrator (&coord, "C:\\fake\\plugin.vst3");
        const auto exportPath = tempFile ("pluginlab_orch_r4b_", ".json");
        const auto wavPath = tempFile ("pluginlab_orch_r4b_", ".wav");

        const auto outcome = orchestrator.run (sweepRequest (exportPath, wavPath));
        REQUIRE (outcome.ok);
        REQUIRE (parseReceived (askReceived (coord))
                 == std::vector<juce::String> ({ "start", "load", "restore_params", "measure" }));

        const auto again = orchestrator.run (sweepRequest (exportPath, wavPath));
        REQUIRE (again.ok);
        REQUIRE (coord.crashCount() == 0);   // the deliberate stop never looked like a crash
        REQUIRE (parseReceived (askReceived (coord))
                 == std::vector<juce::String> ({ "start", "load", "restore_params", "measure" }));

        coord.stop();
        exportPath.deleteFile();
        wavPath.deleteFile();
    }
}

//==============================================================================
// R5 — progress lines are skipped (the result is still found) and a child
//      that stays silent (progress only, no result) times out with the
//      generic "child measurement failed" — never a crash report.
//==============================================================================

TEST_CASE ("ChildMeasureOrchestrator: progress lines skipped and silent child times out",
           "[childorchestrator][progress][timeout]")
{
    REQUIRE (testChildExe().existsAsFile());

    // Scenario A — two progress lines then the result: skipped, success.
    {
        modeFile().replaceWithText ("progress_then_result");
        PluginHostChildCoordinator coord (testChildExe().getFullPathName());
        ChildMeasureOrchestrator orchestrator (&coord, "C:\\fake\\plugin.vst3");
        const auto exportPath = tempFile ("pluginlab_orch_r5a_", ".json");
        const auto wavPath = tempFile ("pluginlab_orch_r5a_", ".wav");

        const auto outcome = orchestrator.run (sweepRequest (exportPath, wavPath));
        REQUIRE (outcome.ok);
        REQUIRE (outcome.result.samples == 240000);

        coord.stop();
        exportPath.deleteFile();
        wavPath.deleteFile();
    }

    // Scenario B — progress-only child: no result within the (shortened)
    // budget → timeout error; the child stayed alive the whole time.
    {
        modeFile().replaceWithText ("progress_only");
        PluginHostChildCoordinator coord (testChildExe().getFullPathName());
        std::atomic<bool> crashed { false };
        coord.setOnCrash ([&] (const juce::String&) { crashed.store (true); });
        ChildMeasureOrchestrator orchestrator (&coord, "C:\\fake\\plugin.vst3", 3000);
        const auto exportPath = tempFile ("pluginlab_orch_r5b_", ".json");
        const auto wavPath = tempFile ("pluginlab_orch_r5b_", ".wav");

        const auto outcome = orchestrator.run (sweepRequest (exportPath, wavPath));
        REQUIRE_FALSE (outcome.ok);
        REQUIRE (outcome.error == "child measurement failed");
        REQUIRE_FALSE (crashed.load());          // progress refreshed liveness — no watchdog
        REQUIRE (coord.isRunning());

        coord.stop();
        exportPath.deleteFile();
        wavPath.deleteFile();
    }

    modeFile().deleteFile();
}

//==============================================================================
// R6 — a mid-measure crash (child exits non-zero on measure): the result
//      collection bails out early and reports the D3b crash vocabulary; the
//      host-side test process survives; the coordinator counted exactly one.
//==============================================================================

TEST_CASE ("ChildMeasureOrchestrator: mid-measure crash reports D3 error, host survives",
           "[childorchestrator][crash][measure]")
{
    // Arrange — the child dies on the measure command (exit code 3).
    REQUIRE (testChildExe().existsAsFile());
    modeFile().replaceWithText ("crash_on_measure");

    PluginHostChildCoordinator coord (testChildExe().getFullPathName());
    std::atomic<bool> crashed { false };
    coord.setOnCrash ([&] (const juce::String&) { crashed.store (true); });
    ChildMeasureOrchestrator orchestrator (&coord, "C:\\fake\\plugin.vst3", 3000);
    const auto exportPath = tempFile ("pluginlab_orch_r6_", ".json");
    const auto wavPath = tempFile ("pluginlab_orch_r6_", ".wav");

    // Act
    const auto outcome = orchestrator.run (sweepRequest (exportPath, wavPath));

    // Assert — D3b: 测量中的崩溃 → "child process crashed (restarting)",
    // exactly one report, child gone, THIS process alive.
    REQUIRE_FALSE (outcome.ok);
    REQUIRE (outcome.error == "child process crashed (restarting)");
    REQUIRE (coord.crashCount() == 1);
    REQUIRE (crashed.load());
    REQUIRE_FALSE (coord.isRunning());

    coord.stop();   // no-op — the child is already gone
    modeFile().deleteFile();
    exportPath.deleteFile();
    wavPath.deleteFile();
}
