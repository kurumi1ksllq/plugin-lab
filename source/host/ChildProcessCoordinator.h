#pragma once

#include <JuceHeader.h>

#include <atomic>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

/**
 * Host-side lifecycle manager for the out-of-process plugin host
 * (PluginHostChild, block D).
 *
 * Spawns the child exe with redirected stdin/stdout, drives it with the
 * single-line JSON protocol (docs/plan-block-d-out-of-process.md
 * "子进程 IPC 协议契约"), watches it with a heartbeat watchdog, and reports
 * crashes — non-zero exit code OR heartbeat timeout, per design question 4 —
 * through onCrash. The host wires onCrash to the blacklist
 * (addToBlacklistLocked + saveCache + CRASH_LOG_WARN, PluginManager.h:178 /
 * PluginManager.cpp:384-387 bundle-key normalization) in a later wave; this
 * class ONLY invokes the callback.
 *
 * IMPLEMENTATION DECISION (written evidence): JUCE 9's juce::ChildProcess
 * (modules/juce_core/threads/juce_ChildProcess.h) has NO stdin-write API —
 * it only offers start/isRunning/readProcessOutput/readAllProcessOutput/
 * waitForProcessToFinish/getExitCode/kill. The protocol requires writing
 * JSON lines to the child's stdin, so this class uses raw Win32
 * CreateProcess + anonymous pipes (CreatePipe + STARTF_USESTDHANDLES),
 * matching PipeServer's windows.h-in-.cpp style.
 *
 * NAMING NOTE: the class is PluginHostChildCoordinator (not
 * "ChildProcessCoordinator", as named in the D1c ticket) because JUCE 9
 * itself declares juce::ChildProcessCoordinator
 * (modules/juce_events/interprocess/juce_ConnectedChildProcess.h:152) and
 * the generated JuceHeader.h contains "using namespace juce;" — the ticket
 * name would be ambiguous in every TU. The file names keep the ticket names.
 *
 * THREADING: one dedicated one-shot reader thread (never detached) runs the
 * stdout read loop AND the heartbeat watchdog. It is built on PeekNamedPipe
 * (non-blocking) instead of a blocking ReadFile, so the loop is never stuck
 * in a syscall: stop() can always join it, and the destructor's
 * "never join a hung thread" fallback (release()) is nearly dead code.
 * onCrash fires on this reader thread — the host must dispatch to the
 * message thread if the handler touches the GUI, and must never call
 * restart() from inside the handler (restart() refuses to join the calling
 * thread and returns false; the dispatch applies to it as well).
 *
 * Exactly-once crash reporting: markCrashed() is guarded by a
 * crashReported atomic AND a stopRequested check, so a deliberate stop()
 * kill or destructor teardown never looks like a crash, and a crash is
 * never double-reported (e.g. watchdog fires first, then the exit-code path
 * observes the TerminateProcess exit code). The winning report also closes
 * the child's handles — crash-restart cycles leak nothing — and bumps
 * crashCount().
 */
class PluginHostChildCoordinator
{
public:
    //==============================================================================
    /** Crash callback: invoked AT MOST ONCE per crash with a human-readable
        detail ("heartbeat timeout: ..." or "child exited with code N — last
        output: ..."). Fires on the internal reader thread. */
    using CrashCallback = std::function<void (const juce::String& detail)>;

    explicit PluginHostChildCoordinator (juce::String childExePath,
                                      int heartbeatTimeoutMs = 3000);
    ~PluginHostChildCoordinator();

    //==============================================================================
    /** Spawn the child with redirected stdin/stdout. Returns false on any
        spawn failure (e.g. missing exe path) — clean error, no crash
        callback. Call setOnCrash() before this. Safe to call after a crash
        or after stop(): a finished reader thread is joined (never joined
        while still running, never abandoned), then a fresh child spawns. */
    bool start();

    /** Restart the child after a crash (or after stop()): joins a finished
        reader thread, closes any handles the previous session left open,
        then spawns a fresh child — the heartbeat clock and the
        stopRequested/crashReported flags are reset by the spawn. Returns
        false if the child is still running, if the previous reader thread is
        still alive (only a provably-finished one is joined — never a hung
        one, per the project iron rule), or if the spawn fails.

        NEVER call this from inside the onCrash callback: the callback fires
        on the reader thread, and restart() refuses to join the calling
        thread (returns false). The host dispatches the restart to another
        thread (e.g. the message thread) instead. */
    bool restart();

    /** True while the child process is considered alive. */
    bool isRunning() const;

    /** Write one JSON line (newline appended) to the child's stdin.
        Thread-safe. Returns false if the child is not running or the write
        failed. */
    bool sendLine (const juce::String& jsonLine);

    /** Next complete response line from the child, or empty if none arrives
        within timeoutMs (0 = single immediate check). Thread-safe. */
    juce::String popLine (int timeoutMs);

    /** Graceful shutdown: send {"cmd":"stop"}, wait up to 2 s, then
        TerminateProcess. Joins the reader thread. Safe when not running.
        Must not run concurrently with the destructor. */
    void stop();

    /** Install the crash callback. Call before start() (the reader thread
        may invoke it as soon as the child misbehaves). */
    void setOnCrash (CrashCallback callback);

        /** Number of crashes reported since construction — cumulative, never
            auto-cleared. A caller that wants per-attempt semantics takes a
            delta (crashesSince = crashCount() - baseline). Thread-safe.
            virtual = D6 test seam (ChildMeasureOrchestratorTests exercises
            the orchestrator's crash-loop gate against this count;
            production code never overrides it). */
    virtual int crashCount() const;

    /** Cache the last parameter snapshot for crash recovery (D3/T5). The
        argument is the FULL snapshot_params response line
        ({"ok":true,"params":[...]}) — the params array fragment is extracted
        and stored, so lastSnapshot() is directly spliceable into a
        restore_params request. A bare params array is stored verbatim; any
        unparseable input clears the cache to empty. Thread-safe. */
    void cacheSnapshot (const juce::String& snapshotResponseLine);

    /** The cached params-array fragment (empty if nothing was cached or the
        last cacheSnapshot() failed to parse). Splice into restore_params as
        {"cmd":"restore_params","params":<lastSnapshot()>}. Thread-safe. */
    juce::String lastSnapshot() const;

private:
    //==============================================================================
    void readerLoop();
    void handlePipeClosed();
    void checkHeartbeat();
    bool markCrashed (const juce::String& detail);   // true = crash actually reported
    void consumeBytes (const char* data, int len, std::string& pendingBytes);
    void onLine (const juce::String& line);
    void closeHandles();
    juce::String lastLogSummary() const;

    bool spawnChild();          // CreateProcess + pipe setup + state reset (start/restart core)
    bool joinFinishedReader();  // join a provably-finished reader, or fail cleanly

    //==============================================================================
    juce::String childExePath;
    int heartbeatTimeoutMs = 3000;

    CrashCallback onCrash;

    // Win32 handles kept as void* so windows.h stays out of this header
    // (same pattern as PipeServer.h).
    void* processHandle = nullptr;       // child process (owned, closed here)
    void* stdinWriteHandle = nullptr;    // parent side of child stdin
    void* stdoutReadHandle = nullptr;    // parent side of child stdout

    std::atomic<bool> processRunning { false };
    std::atomic<bool> stopRequested { false };
    std::atomic<bool> crashReported { false };
    std::atomic<bool> readerDone { false };
    std::atomic<uint32> lastHeartbeatMs { 0 };   // wrap-safe via uint32 deltas
    std::atomic<int> crashCountValue { 0 };      // cumulative crash reports (D3b restart)

    mutable std::mutex lineLock;         // guards lineQueue + lastLogLines
    std::deque<juce::String> lineQueue;
    std::deque<juce::String> lastLogLines;

    std::mutex stdinLock;                // serializes sendLine writes
    std::mutex handleLock;               // serializes closeHandles across threads
    mutable std::mutex snapshotLock;     // guards cachedSnapshot (cacheSnapshot/lastSnapshot)
    juce::String cachedSnapshot;         // last snapshot_params params-array fragment (D3 recovery)

    // unique_ptr so the destructor can abandon a not-yet-finished reader via
    // release() (project iron rule: never join a hung thread — same pattern
    // as Main.cpp's scan/load threads).
    std::unique_ptr<std::thread> readerThread;

    //==============================================================================
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginHostChildCoordinator)
};
