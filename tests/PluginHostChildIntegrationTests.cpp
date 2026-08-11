/**
 * PluginHostChildIntegrationTests (block D1d): integration tests against the
 * REAL out-of-process host child (source/child/PluginHostChild.cpp, D1a+D1b).
 *
 * These complete the D1d acceptance from docs/plan-block-d-out-of-process.md:
 * "spawn 真子进程 → start/load → kill → 断言检测到崩溃 + 宿主存活". They spawn
 * the real PluginHostChild.exe through the coordinator (D1c) and drive it
 * with the frozen stdin/stdout JSON-line protocol. No real plugin is needed:
 * load error paths (empty / nonexistent path) exercise the protocol without
 * a VST3 file. The externally-killed child must be detected as a crash while
 * the test process (the "host") survives — which is the whole point of the
 * out-of-process design.
 *
 * Response shapes asserted here were probed against the built
 * PluginHostChild.exe before writing (start -> {"ok":true,"pid":N,
 * "version":1}, heartbeat -> {"ok":true}, load "" -> "path required", load
 * nonexistent -> "plugin not found", unknown cmd -> "unknown cmd", stop ->
 * {"ok":true} + exit 0).
 */
#include <catch2/catch_test_macros.hpp>

#include "../source/host/ChildProcessCoordinator.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <thread>

#ifndef PLUGIN_HOST_CHILD_EXE
#error "PLUGIN_HOST_CHILD_EXE must be defined by tests/CMakeLists.txt"
#endif

namespace
{
    juce::File realChildExe()
    {
        return juce::File (juce::String (PLUGIN_HOST_CHILD_EXE));
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

    /** Digits directly after the first occurrence of `key` in `line`. */
    juce::String extractDigits (const juce::String& line, const juce::String& key)
    {
        const auto start = line.indexOf (key);
        if (start < 0)
            return {};

        auto end = start + key.length();
        while (end < line.length() && line[end] >= '0' && line[end] <= '9')
            ++end;

        return line.substring (start + key.length(), end);
    }
}  // namespace

//==============================================================================
// I1 — spawn the real child, start roundtrip, clean stop, no crash.
//==============================================================================

TEST_CASE ("PluginHostChildIntegration: spawn/start/stop roundtrip",
           "[childcoordinator][integration][spawn]")
{
    // Arrange
    REQUIRE (realChildExe().existsAsFile());
    PluginHostChildCoordinator coord (realChildExe().getFullPathName());
    std::atomic<bool> crashed { false };
    coord.setOnCrash ([&] (const juce::String&) { crashed.store (true); });

    REQUIRE (coord.start());
    REQUIRE (coord.isRunning());

    // Act — protocol handshake, exactly as the D1d ticket describes.
    REQUIRE (coord.sendLine (R"({"cmd":"start"})"));
    const auto startLine = coord.popLine (3000);

    // Assert — {"ok":true,"pid":N,"version":1}
    REQUIRE (startLine.isNotEmpty());
    REQUIRE (startLine.contains ("\"ok\":true"));
    REQUIRE (startLine.contains ("\"pid\":"));
    REQUIRE (startLine.contains ("\"version\":1"));
    REQUIRE (extractDigits (startLine, "\"pid\":").isNotEmpty());

    // Act — graceful stop.
    REQUIRE (coord.sendLine (R"({"cmd":"stop"})"));
    const auto stopLine = coord.popLine (3000);
    REQUIRE (stopLine.contains ("\"ok\":true"));
    coord.stop();

    // Assert — clean exit, no crash callback, host test process alive.
    REQUIRE_FALSE (crashed.load());
    REQUIRE_FALSE (coord.isRunning());
}

//==============================================================================
// I2 — heartbeat roundtrip.
//==============================================================================

TEST_CASE ("PluginHostChildIntegration: heartbeat roundtrip",
           "[childcoordinator][integration][heartbeat]")
{
    // Arrange
    REQUIRE (realChildExe().existsAsFile());
    PluginHostChildCoordinator coord (realChildExe().getFullPathName());
    std::atomic<bool> crashed { false };
    coord.setOnCrash ([&] (const juce::String&) { crashed.store (true); });

    REQUIRE (coord.start());

    // Act
    REQUIRE (coord.sendLine (R"({"cmd":"heartbeat"})"));
    const auto line = coord.popLine (3000);

    // Assert — child answers {"ok":true}, proving the liveness channel works.
    REQUIRE (line.isNotEmpty());
    REQUIRE (line.contains ("\"ok\":true"));
    REQUIRE_FALSE (crashed.load());

    coord.stop();
    REQUIRE_FALSE (crashed.load());
}

//==============================================================================
// I3 — load error paths (no real plugin needed).
//==============================================================================

TEST_CASE ("PluginHostChildIntegration: load error paths",
           "[childcoordinator][integration][load]")
{
    // Arrange
    REQUIRE (realChildExe().existsAsFile());
    PluginHostChildCoordinator coord (realChildExe().getFullPathName());
    std::atomic<bool> crashed { false };
    coord.setOnCrash ([&] (const juce::String&) { crashed.store (true); });

    REQUIRE (coord.start());

    // Act — empty path: protocol contract "path required".
    REQUIRE (coord.sendLine (R"({"cmd":"load","path":""})"));
    const auto emptyPath = coord.popLine (3000);

// Act — nonexistent path: scanning a missing file finds nothing. The path is
// valid JSON (backslashes escaped) so the child receives exactly
// "C:\nonexistent\x.vst3" — not a JSON-escape-mangled variant.
REQUIRE (coord.sendLine (R"({"cmd":"load","path":"C:\\nonexistent\\x.vst3"})"));
    const auto missingPath = coord.popLine (3000);

    // Assert — error vocabulary matches the real child's implementation
    // (probed: {"ok":false,"error":"path required"} / "plugin not found").
    REQUIRE (emptyPath.contains ("\"ok\":false"));
    REQUIRE (emptyPath.contains ("path required"));
    REQUIRE (missingPath.contains ("\"ok\":false"));
    REQUIRE (missingPath.contains ("plugin not found"));
    REQUIRE_FALSE (crashed.load());   // a failed load is an error, not a crash

    coord.stop();
}

//==============================================================================
// I4 — external kill: taskkill /F the real child -> crash detected, host
//      (this test process) survives.
//==============================================================================

TEST_CASE ("PluginHostChildIntegration: external kill detected as crash",
           "[childcoordinator][integration][crash]")
{
    // Arrange
    REQUIRE (realChildExe().existsAsFile());
    PluginHostChildCoordinator coord (realChildExe().getFullPathName());
    std::atomic<bool> crashed { false };
    juce::String crashDetail;
    coord.setOnCrash ([&] (const juce::String& detail) {
        crashDetail = detail;      // assign BEFORE the flag store (happens-before)
        crashed.store (true);
    });

    REQUIRE (coord.start());

    // Act — handshake, then externally kill the child like a host-killing
    // plugin would (ExitProcess/abort semantics; taskkill /F is our injection
    // of an abrupt process death).
    REQUIRE (coord.sendLine (R"({"cmd":"start"})"));
    const auto startLine = coord.popLine (3000);
    REQUIRE (startLine.isNotEmpty());
    const auto pid = extractDigits (startLine, "\"pid\":").getIntValue();
    REQUIRE (pid > 0);

    const juce::String cmd = "taskkill /F /PID " + juce::String (pid) + " >NUL 2>&1";
    REQUIRE (std::system (cmd.toRawUTF8()) == 0);

    // Assert — crash detected (non-zero exit code from the forced kill), the
    // coordinator knows the child is gone, and this test process is still
    // alive to make the assertion: the host survived the child's death.
    REQUIRE (waitFor (crashed, 8000));
    REQUIRE (crashDetail.contains ("1"));        // taskkill /F termination exit code
    REQUIRE_FALSE (coord.isRunning());
    REQUIRE_FALSE (coord.sendLine (R"({"cmd":"heartbeat"})"));   // dead child rejects I/O
}

//==============================================================================
// I5 — unknown command: protocol vocabulary consistency.
//==============================================================================

TEST_CASE ("PluginHostChildIntegration: unknown command error",
           "[childcoordinator][integration][protocol]")
{
    // Arrange
    REQUIRE (realChildExe().existsAsFile());
    PluginHostChildCoordinator coord (realChildExe().getFullPathName());
    std::atomic<bool> crashed { false };
    coord.setOnCrash ([&] (const juce::String&) { crashed.store (true); });

    REQUIRE (coord.start());

    // Act
    REQUIRE (coord.sendLine (R"({"cmd":"frobnicate"})"));
    const auto line = coord.popLine (3000);

    // Assert — {"ok":false,"error":"unknown cmd"} (same vocabulary as the
    // host-side CommandParser family).
    REQUIRE (line.isNotEmpty());
    REQUIRE (line.contains ("\"ok\":false"));
    REQUIRE (line.contains ("unknown cmd"));
    REQUIRE_FALSE (crashed.load());

    coord.stop();
}
