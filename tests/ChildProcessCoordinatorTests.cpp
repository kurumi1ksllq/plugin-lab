/**
 * PluginHostChildCoordinator unit tests (block D1c + D3b restart, TDD).
 *
 * These tests spawn the real TestChildProcess.exe console target
 * (tests/TestChildProcess.cpp) — a stdin/stdout JSON-line double — and
 * exercise spawn, protocol roundtrip, crash detection (non-zero exit code),
 * heartbeat timeout, bad-path handling, graceful stop and crash/stop
 * restart cycles (R1-R4: restart(), crashCount()). Real OS objects in
 * tests are precedented (PipeServerTests creates a real named pipe).
 *
 * Protocol contract under test (docs/plan-block-d-out-of-process.md
 * "子进程 IPC 协议契约"): single-line JSON + '\n' both ways; child lines
 * update liveness; crash = non-zero exit code OR heartbeat timeout.
 */
#include <catch2/catch_test_macros.hpp>

#include "../source/host/ChildProcessCoordinator.h"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <thread>

#ifndef TEST_CHILD_EXE
#error "TEST_CHILD_EXE must be defined by tests/CMakeLists.txt"
#endif

namespace
{
    juce::File testChildExe()
    {
        return juce::File (juce::String (TEST_CHILD_EXE));
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

    /** Open kernel handles of this process (approx: threads/CRT add a few).
        Used to prove crash paths close their pipe handles (no leak per
        crash-restart cycle). */
    int handleCount()
    {
        DWORD count = 0;
        if (::GetProcessHandleCount (::GetCurrentProcess(), &count) == FALSE)
            return 0;
        return (int) count;
    }
}  // namespace

//==============================================================================
// S7 — long-running op with periodic progress lines: the watchdog must NOT
//       treat a child that keeps emitting progress as dead (D2 loads a plugin
//       for up to 30 s; the 3 s heartbeat default must not kill it mid-load).
//==============================================================================

TEST_CASE ("ChildProcessCoordinator: periodic progress keeps long op alive",
           "[childcoordinator][heartbeat][progress]")
{
    // Arrange — 3 s heartbeat timeout; child pulses progress every 500 ms
    // for 8 s (well past the 3 s timeout).
    REQUIRE (testChildExe().existsAsFile());
    PluginHostChildCoordinator coord (testChildExe().getFullPathName(), 3000);
    std::atomic<bool> crashed { false };
    coord.setOnCrash ([&] (const juce::String&) { crashed.store (true); });

    REQUIRE (coord.start());
    REQUIRE (coord.sendLine (R"({"cmd":"start"})"));
    REQUIRE (coord.popLine (3000).isNotEmpty());

    // Act — a long operation that keeps emitting progress lines.
    REQUIRE (coord.sendLine (R"({"cmd":"pulse","ms":8000,"interval":500})"));

    // Give the watchdog several heartbeat windows to (wrongly) fire.
    std::this_thread::sleep_for (std::chrono::milliseconds (7000));

    // Assert — still alive: the progress lines refreshed liveness.
    REQUIRE_FALSE (crashed.load());
    REQUIRE (coord.isRunning());

    coord.stop();
    REQUIRE_FALSE (coord.isRunning());
}

//==============================================================================
// S1 — happy path: spawn, send {"cmd":"start"}, receive the response line.
//==============================================================================

TEST_CASE ("ChildProcessCoordinator: spawn child and receive start response",
           "[childcoordinator][spawn]")
{
    // Arrange
    REQUIRE (testChildExe().existsAsFile());
    PluginHostChildCoordinator coord (testChildExe().getFullPathName());
    std::atomic<bool> crashed { false };
    coord.setOnCrash ([&] (const juce::String&) { crashed.store (true); });

    REQUIRE (coord.start());
    REQUIRE (coord.isRunning());

    // Act
    REQUIRE (coord.sendLine (R"({"cmd":"start"})"));
    const auto line = coord.popLine (3000);

    // Assert
    REQUIRE (line.isNotEmpty());
    REQUIRE (line.contains ("\"ok\":true"));
    REQUIRE (line.contains ("\"pid\""));
    REQUIRE (line.contains ("\"version\":1"));
    REQUIRE_FALSE (crashed.load());

    coord.stop();
    REQUIRE_FALSE (coord.isRunning());
}

//==============================================================================
// S2 — crash: child exits with code 3 -> onCrash fired with exit code 3.
//==============================================================================

TEST_CASE ("ChildProcessCoordinator: non-zero exit code fires crash callback",
           "[childcoordinator][crash]")
{
    // Arrange
    REQUIRE (testChildExe().existsAsFile());
    PluginHostChildCoordinator coord (testChildExe().getFullPathName());
    std::atomic<bool> crashed { false };
    juce::String crashDetail;
    coord.setOnCrash ([&] (const juce::String& detail) {
        crashDetail = detail;      // assign BEFORE the flag store (happens-before)
        crashed.store (true);
    });

    REQUIRE (coord.start());

    // Act
    REQUIRE (coord.sendLine (R"({"cmd":"exit","code":3})"));
    REQUIRE (waitFor (crashed, 5000));

    // Assert
    REQUIRE (crashDetail.contains ("3"));
    REQUIRE_FALSE (coord.isRunning());
}

//==============================================================================
// S3 — heartbeat timeout: child sleeps without responding -> watchdog fires
//       onCrash ("heartbeat timeout") at ~heartbeatTimeoutMs.
//==============================================================================

TEST_CASE ("ChildProcessCoordinator: heartbeat timeout fires crash callback",
           "[childcoordinator][heartbeat]")
{
    // Arrange — 3s heartbeat timeout, child sleeps 8s without responding.
    REQUIRE (testChildExe().existsAsFile());
    PluginHostChildCoordinator coord (testChildExe().getFullPathName(), 3000);
    std::atomic<bool> crashed { false };
    juce::String crashDetail;
    coord.setOnCrash ([&] (const juce::String& detail) {
        crashDetail = detail;
        crashed.store (true);
    });

    REQUIRE (coord.start());

    // Act — the child must NOT answer; only the watchdog can end this.
    REQUIRE (coord.sendLine (R"({"cmd":"sleep","ms":8000})"));
    REQUIRE (waitFor (crashed, 8000));

    // Assert — fired by the watchdog, not by an exit-code path.
    REQUIRE (crashDetail.contains ("heartbeat"));
    REQUIRE_FALSE (coord.isRunning());
}

//==============================================================================
// S4 — bad path: nonexistent exe -> start() fails cleanly, no crash callback.
//==============================================================================

TEST_CASE ("ChildProcessCoordinator: nonexistent exe fails cleanly",
           "[childcoordinator][badpath]")
{
    // Arrange
    PluginHostChildCoordinator coord ("C:\\nonexistent\\NoSuchChild.exe");
    std::atomic<bool> crashed { false };
    coord.setOnCrash ([&] (const juce::String&) { crashed.store (true); });

    // Act
    const bool started = coord.start();

    // Assert
    REQUIRE_FALSE (started);
    REQUIRE_FALSE (crashed.load());
    REQUIRE_FALSE (coord.isRunning());
    coord.stop();  // no-op, must not crash or callback
}

//==============================================================================
// S5 — stop: {"cmd":"stop"} -> child exits 0 -> no crash callback.
//==============================================================================

TEST_CASE ("ChildProcessCoordinator: stop leads to clean exit, no crash",
           "[childcoordinator][stop]")
{
    // Arrange
    REQUIRE (testChildExe().existsAsFile());
    PluginHostChildCoordinator coord (testChildExe().getFullPathName());
    std::atomic<bool> crashed { false };
    coord.setOnCrash ([&] (const juce::String&) { crashed.store (true); });

    REQUIRE (coord.start());
    REQUIRE (coord.sendLine (R"({"cmd":"start"})"));
    REQUIRE (coord.popLine (3000).isNotEmpty());   // child alive and responsive

    // Act
    coord.stop();

    // Assert — graceful exit 0 must not look like a crash.
    REQUIRE_FALSE (crashed.load());
    REQUIRE_FALSE (coord.isRunning());
}

//==============================================================================
// S6 — framing: two commands in a row -> two distinct responses, in order.
//==============================================================================

TEST_CASE ("ChildProcessCoordinator: multi-line roundtrip keeps framing",
           "[childcoordinator][framing]")
{
    // Arrange
    REQUIRE (testChildExe().existsAsFile());
    PluginHostChildCoordinator coord (testChildExe().getFullPathName());
    std::atomic<bool> crashed { false };
    coord.setOnCrash ([&] (const juce::String&) { crashed.store (true); });

    REQUIRE (coord.start());

    // Act — two lines back to back, child echoes each.
    REQUIRE (coord.sendLine (R"({"cmd":"echo","text":"first"})"));
    REQUIRE (coord.sendLine (R"({"cmd":"echo","text":"second"})"));
    const auto first = coord.popLine (3000);
    const auto second = coord.popLine (3000);

    // Assert — two distinct lines, in order, framing intact.
    REQUIRE (first.isNotEmpty());
    REQUIRE (second.isNotEmpty());
    REQUIRE (first != second);
    REQUIRE (first.contains ("first"));
    REQUIRE (second.contains ("second"));
    REQUIRE_FALSE (crashed.load());

    coord.stop();
}

//==============================================================================
// R1 — crash (exit code 3) then restart(): exactly one onCrash, host-side
//       coordinator survives, the restarted child works again and the
//       heartbeat clock was reset (no stale-clock watchdog, no terminate).
//==============================================================================

TEST_CASE ("ChildProcessCoordinator: crash then restart resumes working",
           "[childcoordinator][restart][crash]")
{
    // Arrange — short heartbeat (400 ms) so a stale clock would fire fast.
    REQUIRE (testChildExe().existsAsFile());
    PluginHostChildCoordinator coord (testChildExe().getFullPathName(), 400);
    std::atomic<int> crashCalls { 0 };
    std::atomic<bool> crashed { false };
    coord.setOnCrash ([&] (const juce::String&) {
        crashCalls.fetch_add (1);
        crashed.store (true);
    });

    // First life: spawn and verify the pipe works.
    REQUIRE (coord.start());
    REQUIRE (coord.sendLine (R"({"cmd":"start"})"));
    REQUIRE (coord.popLine (3000).isNotEmpty());

    // Act — crash with exit code 3.
    REQUIRE (coord.sendLine (R"({"cmd":"exit","code":3})"));
    REQUIRE (waitFor (crashed, 5000));

    // Assert — exactly one report, host side still alive (no terminate).
    REQUIRE (crashCalls.load() == 1);
    REQUIRE_FALSE (coord.isRunning());

    // Act — restart: new process, fresh pipes, reset heartbeat clock.
    REQUIRE (coord.restart());
    REQUIRE (coord.isRunning());
    REQUIRE (coord.sendLine (R"({"cmd":"start"})"));
    REQUIRE (coord.popLine (3000).isNotEmpty());
    crashed.store (false);   // the first crash is history — watch only the new life

    // Assert — heartbeat healthy after restart: progress pulses every 300 ms
    // keep a 400 ms watchdog at bay. A stale clock (not reset at spawn)
    // would fire onCrash within ~400 ms of the restart.
    REQUIRE (coord.sendLine (R"({"cmd":"pulse","ms":1500,"interval":300})"));
    std::this_thread::sleep_for (std::chrono::milliseconds (1200));
    REQUIRE_FALSE (crashed.load());
    REQUIRE (crashCalls.load() == 1);
    REQUIRE (coord.isRunning());

    coord.stop();
    REQUIRE_FALSE (coord.isRunning());
}

//==============================================================================
// R2 — three consecutive crashes: crashCount() accumulates 1..3 (continuous,
//       not auto-cleared); every restart works.
//==============================================================================

TEST_CASE ("ChildProcessCoordinator: consecutive crashes accumulate crashCount",
           "[childcoordinator][restart][crash]")
{
    // Arrange
    REQUIRE (testChildExe().existsAsFile());
    PluginHostChildCoordinator coord (testChildExe().getFullPathName());
    std::atomic<bool> crashed { false };
    coord.setOnCrash ([&] (const juce::String&) { crashed.store (true); });

    REQUIRE (coord.start());

    // Act — three crash cycles.
    for (int i = 0; i < 3; ++i)
    {
        crashed.store (false);
        REQUIRE (coord.sendLine (R"({"cmd":"exit","code":3})"));
        REQUIRE (waitFor (crashed, 5000));

        // Assert — exactly one report per crash, cumulative counter.
        REQUIRE_FALSE (coord.isRunning());
        REQUIRE (coord.crashCount() == i + 1);

        if (i < 2)
            REQUIRE (coord.restart());   // the third crash stays unrecovered
    }

    REQUIRE (coord.crashCount() == 3);
    coord.stop();   // no-op: nothing running
}

//==============================================================================
// R3 — five crash+restart cycles: never terminates, every restart succeeds,
//       and crash-path handles are closed (no ~3-handle leak per crash).
//==============================================================================

TEST_CASE ("ChildProcessCoordinator: five crash-restart cycles, no terminate, no handle leak",
           "[childcoordinator][restart][crash]")
{
    // Arrange — measure open handles before any spawn; with the leak bug
    // each crash leaves processHandle + stdinWriteHandle + stdoutReadHandle
    // open, i.e. +15 after five cycles (stop() only closes the last one).
    REQUIRE (testChildExe().existsAsFile());
    PluginHostChildCoordinator coord (testChildExe().getFullPathName());
    std::atomic<bool> crashed { false };
    coord.setOnCrash ([&] (const juce::String&) { crashed.store (true); });

    const int handlesBefore = handleCount();

    REQUIRE (coord.start());

    // Act — five full crash + restart cycles.
    for (int i = 0; i < 5; ++i)
    {
        crashed.store (false);
        REQUIRE (coord.sendLine (R"({"cmd":"exit","code":3})"));
        REQUIRE (waitFor (crashed, 5000));
        REQUIRE (coord.restart());
        REQUIRE (coord.isRunning());
    }

    coord.stop();   // closes the fifth (live) session's handles

    // Assert — never terminated, count exact, handles back to baseline
    // (slack 5 separates ~0 from a 12-15 handle leak).
    REQUIRE (coord.crashCount() == 5);
    const int handlesAfter = handleCount();
    REQUIRE (handlesAfter - handlesBefore < 5);
}

//==============================================================================
// R4 — stop() then restart(): stopRequested is reset by the fresh spawn, so
//       the restarted child reports and works again.
//==============================================================================

TEST_CASE ("ChildProcessCoordinator: restart works after stop",
           "[childcoordinator][restart][stop]")
{
    // Arrange
    REQUIRE (testChildExe().existsAsFile());
    PluginHostChildCoordinator coord (testChildExe().getFullPathName());
    std::atomic<bool> crashed { false };
    coord.setOnCrash ([&] (const juce::String&) { crashed.store (true); });

    REQUIRE (coord.start());
    REQUIRE (coord.sendLine (R"({"cmd":"start"})"));
    REQUIRE (coord.popLine (3000).isNotEmpty());

    // Act — graceful stop; a stale stopRequested must not kill the restart.
    coord.stop();
    REQUIRE_FALSE (coord.isRunning());
    REQUIRE_FALSE (crashed.load());

    // Assert — restart spawns a fresh, responsive child.
    REQUIRE (coord.restart());
    REQUIRE (coord.isRunning());
    REQUIRE (coord.sendLine (R"({"cmd":"start"})"));
    REQUIRE (coord.popLine (3000).isNotEmpty());
    REQUIRE_FALSE (crashed.load());

    coord.stop();
}
