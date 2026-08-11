#pragma once

#include <JuceHeader.h>

#include <atomic>
#include <condition_variable>
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
 */
class PipeServer : private juce::Thread
{
public:
    PipeServer();
    ~PipeServer() override;

    //==============================================================================
    /** Start the pipe server (pipe thread + worker thread). */
    void startup();

    /** Stop the worker (bounded join) and the pipe thread; close any live
     *  pipe. Safe to call when not started. */
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

private:
    void run() override;
    void workerLoop();

    /** True if the JSON command's "cmd" field is in the control set. */
    bool isControlCommand (const juce::String& commandJson) const;

    /** Write one response line to the current connection pipe, under
     *  ioMutex. Skips the write when the connection generation changed
     *  (worker final response, emitLine) or no client is connected. */
    void writeLine (const juce::String& response, uint64_t generation, bool requireWorkerBusy);

    //==============================================================================
    CommandHandler commandHandler;
    std::vector<juce::String> controlCommandNames;

    // Pipe state — guarded by ioMutex; the pipe thread owns the transitions,
    // every writer (inline / worker / emitLine) serializes on it.
    mutable std::mutex ioMutex;
    void* hPipe = nullptr;              // current connection pipe (or null)
    uint64_t connectionGeneration = 0;  // bumped on every connection transition
    std::atomic<bool> clientConnected { false };

    // Worker — one FIFO queue, one thread, no concurrent handlers.
    std::thread workerThread;
    std::mutex workerMutex;
    std::condition_variable workerCv;
    std::deque<std::pair<juce::String, uint64_t>> workerQueue;  // {command, generation}
    bool workerExit = false;
    std::atomic<bool> workerBusy { false };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PipeServer)
};
