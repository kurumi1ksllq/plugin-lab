#pragma once

#include <JuceHeader.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

/**
 * Windows named-pipe server (\\\\.\\pipe\\PluginLab, JSON-line protocol).
 *
 * CONCURRENCY MODEL (issue #2 / #3 rework):
 * - One read loop on the pipe thread. Commands split into two tiers:
 *   - CONTROL commands (stop / getScanStatus — set via setControlCommands):
 *     served INLINE on the pipe thread while a long command runs. They must
 *     be non-blocking and touch only thread-safe state (atomic flags /
 *     internally-locked snapshots) — never the plugin or the session.
 *   - EVERYTHING else: queued FIFO onto a single worker thread. All commands
 *     that touch the plugin/session are therefore serialized — the worker
 *     never runs two handlers concurrently, and control commands never
 *     contend for plugin state. This is what makes "stop during a hosted
 *     (child-process) measure" reachable: the read loop keeps reading while
 *     the measure runs on the worker.
 * - All WriteFile calls (inline control response, worker final response,
 *   emitLine progress) go through writeLine() under one ioMutex. The worker
 *   captures the connection generation at queue time and writes only if it
 *   still matches — a response never lands on a stale/reused pipe handle.
 * - shutdown(): Main.cpp cancels the session BEFORE calling this, so the
 *   worker's in-flight command returns promptly and joins cleanly.
 *
 * SECURITY MODEL (issue #35 hardening):
 * - ONE pipe instance is created at startup with FILE_FLAG_FIRST_PIPE_INSTANCE
 *   and kept alive for the app lifetime, reused across connections via
 *   ConnectNamedPipe / DisconnectNamedPipe (the documented single-instance
 *   server loop). A squatter holding the well-known name is rejected with
 *   ERROR_ACCESS_DENIED — it is never handed the next client connection
 *   (no MITM on the AI controller). isPipeAcquired() / getLastPipeCreateError()
 *   report the acquisition state; the app logs the failure and keeps retrying.
 * - The instance carries an explicit DACL granting GENERIC_ALL to the current
 *   user, BUILTIN\Administrators and SYSTEM only (getDefaultPipeSddl()).
 *   A secret handshake / token exchange is deliberately NOT implemented here:
 *   that is a client-side protocol change, tracked as a separate decision.
 * - An oversized message (> 65535 bytes) is drained and answered with
 *   {"ok":false,"error":"command too large"} instead of dropping the
 *   connection. writeLine() closes the connection on write failure / partial
 *   write instead of silently swallowing it.
 */
class PipeServer : private juce::Thread
{
public:
    PipeServer();
    ~PipeServer() override;

    //==============================================================================
    /** Start the pipe server (pipe thread + worker thread). */
    void startup();

    /** Stop the worker (bounded join) and the pipe thread; close the single
     *  pipe instance. Safe to call when not started. */
    void shutdown();

    /** Set the command handler callback.
     *  Callback receives the JSON command string and must return a JSON response.
     *  Runs on the worker thread (non-control) or the pipe thread (control). */
    using CommandHandler = std::function<juce::String (const juce::String& command)>;
    void setCommandHandler (CommandHandler handler);

    /** Control commands served INLINE on the pipe thread while the worker is
     *  busy (issue #3). Each entry is a "cmd" JSON field value (e.g. "stop").
     *  They must be non-blocking and thread-safe to run concurrently with a
     *  worker-served command. */
    void setControlCommands (std::vector<juce::String> controlCommandNames);

    /** Push an intermediate response line to the connected client while a
     *  worker-served command runs (issue #2 playback progress). Thread-safe.
     *  No-op unless the worker is currently busy (the GUI path never pushes)
     *  and a client is connected. */
    void emitLine (const juce::String& line);

    /** Returns true if a client is connected. */
    bool isClientConnected() const { return clientConnected; }

    /** True once the single pipe instance was created and the well-known name
     *  is owned (issue #35). Stays false while the name is squatted — see
     *  getLastPipeCreateError(). */
    bool isPipeAcquired() const { return pipeAcquired; }

    /** Last CreateNamedPipeA error code (0 once acquired; updated while the
     *  acquisition retry loop runs). ERROR_ACCESS_DENIED means the name is
     *  already held by another process — squatting is blocked. */
    uint32_t getLastPipeCreateError() const { return lastPipeCreateError; }

    /** SDDL DACL for the pipe instance: grants GENERIC_ALL to the current
     *  user, BUILTIN\Administrators and SYSTEM; denies everyone else
     *  (issue #35). Empty when the current user's SID cannot be resolved. */
    static juce::String getDefaultPipeSddl();

private:
    void run() override;
    void workerLoop();

    /** True if the JSON command's "cmd" field is in the control set. */
    bool isControlCommand (const juce::String& commandJson) const;

    /** Write one response line to the current connection pipe, under
     *  ioMutex. Skips the write when the connection generation changed
     *  (worker final response, emitLine) or no client is connected. On a
     *  failed / partial write the connection is reset (the read loop
     *  reconnects the next client; the single instance survives). */
    void writeLine (const juce::String& response, uint64_t generation, bool requireWorkerBusy);

    //==============================================================================
    CommandHandler commandHandler;
    std::vector<juce::String> controlCommandNames;

    // Pipe state — guarded by ioMutex; the pipe thread owns the transitions,
    // every writer (inline / worker / emitLine) serializes on it.
    mutable std::mutex ioMutex;
    void* hPipe = nullptr;         // current connection pipe (== hPipeInstance while a client is connected, else null)
    void* hPipeInstance = nullptr; // the single pipe instance (app-lifetime, owned; closed by shutdown())
    uint64_t connectionGeneration = 0;  // bumped on every connection transition
    std::atomic<bool> clientConnected { false };

    // Single-instance acquisition state (issue #35) — written by the pipe
    // thread, read via isPipeAcquired() / getLastPipeCreateError().
    std::atomic<bool> pipeAcquired { false };
    std::atomic<uint32_t> lastPipeCreateError { 0 };

    // Worker — one FIFO queue, one thread, no concurrent handlers.
    std::thread workerThread;
    std::mutex workerMutex;
    std::condition_variable workerCv;
    std::deque<std::pair<juce::String, uint64_t>> workerQueue;  // {command, generation}
    bool workerExit = false;
    std::atomic<bool> workerBusy { false };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PipeServer)
};
