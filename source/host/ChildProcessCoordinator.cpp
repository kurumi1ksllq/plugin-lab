#include "ChildProcessCoordinator.h"

#include <windows.h>

#include <string>
#include <vector>

namespace
{
    constexpr int kReadChunkBytes = 64 * 1024;   // per PeekNamedPipe/ReadFile chunk
    constexpr int kExitGraceMs = 1000;           // wait for full termination before GetExitCodeProcess
    constexpr int kStopGraceMs = 2000;           // graceful {"cmd":"stop"} wait before kill
    constexpr int kWatchdogPollMs = 50;          // reader loop idle poll
    constexpr int kMaxLineBytes = 1024 * 1024;   // drop pathological unterminated lines
    constexpr int kMaxLastLogLines = 8;          // ring buffer for crash detail
    constexpr int kMaxLogSummaryChars = 512;     // cap the crash detail payload
    constexpr int kMaxQueuedLines = 1024;        // bounded response queue (drop oldest)
    constexpr int kRestartJoinWaitMs = 250;      // reader-exit grace for restart() (see joinFinishedReader)
}  // namespace

//==============================================================================
// Lifecycle
//==============================================================================

PluginHostChildCoordinator::PluginHostChildCoordinator (juce::String childExePath_,
                                                  int heartbeatTimeoutMs_)
    : childExePath (std::move (childExePath_)),
      heartbeatTimeoutMs (heartbeatTimeoutMs_)
{}

PluginHostChildCoordinator::~PluginHostChildCoordinator()
{
    // Emergency teardown — deliberately NOT a crash event: no onCrash fires.
    // stopRequested is set FIRST so the reader thread (and both crash-report
    // paths) observe the teardown before touching onCrash.
    stopRequested.store (true);

    if (processRunning.load())
    {
        ::TerminateProcess ((HANDLE) processHandle, 1);
        processRunning.store (false);
    }

    closeHandles();

    // The reader loop never blocks in a syscall (PeekNamedPipe-based), so it
    // observes stopRequested within ~kWatchdogPollMs and exits on its own.
    // Join only when the thread has provably finished (readerDone); otherwise
    // abandon it per the project iron rule — never join a hung thread, use
    // unique_ptr::release() (same as Main.cpp's scan/load threads). With the
    // PeekNamedPipe loop the release() branch is nearly dead code; when it
    // does run, the abandoned thread's remaining member access is limited to
    // the stopRequested/readerDone atomics, and both crash-report paths
    // re-check stopRequested before touching onCrash.
    if (readerThread != nullptr)
    {
        if (readerThread->joinable() && readerDone.load())
            readerThread->join();
        else
            readerThread.release();
    }
}

bool PluginHostChildCoordinator::start()
{
    if (processRunning.load())
        return false;

    if (! joinFinishedReader())
        return false;

    return spawnChild();
}

bool PluginHostChildCoordinator::restart()
{
    if (processRunning.load())
        return false;

    if (! joinFinishedReader())
        return false;

    // Defensive: after a crash the handles were already closed by the crash
    // report, after a stop() by stop() itself — this only catches a path
    // that left them open (idempotent, mutex-guarded).
    closeHandles();

    return spawnChild();
}

//==============================================================================
// Spawn core (shared by start() and restart())
//==============================================================================

bool PluginHostChildCoordinator::spawnChild()
{
    // -- Create the two anonymous pipes; both ends inheritable for now. ------
    SECURITY_ATTRIBUTES sa {};
    sa.nLength = sizeof (SECURITY_ATTRIBUTES);
    sa.bInheritHandle = TRUE;

    HANDLE childStdinRd = nullptr, childStdinWr = nullptr;
    HANDLE childStdoutRd = nullptr, childStdoutWr = nullptr;

    if (::CreatePipe (&childStdinRd, &childStdinWr, &sa, 0) == FALSE)
        return false;

    if (::CreatePipe (&childStdoutRd, &childStdoutWr, &sa, 0) == FALSE)
    {
        ::CloseHandle (childStdinRd);
        ::CloseHandle (childStdinWr);
        return false;
    }

    // Parent-side ends must NOT be inheritable (the child would never see EOF
    // on its stdin / its stdout would never break when we close our copies).
    ::SetHandleInformation (childStdinWr, HANDLE_FLAG_INHERIT, 0);
    ::SetHandleInformation (childStdoutRd, HANDLE_FLAG_INHERIT, 0);

    // -- Spawn the child with stdin/stdout/stderr redirected. ---------------
    STARTUPINFOW si {};
    si.cb = sizeof (si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = childStdinRd;
    si.hStdOutput = childStdoutWr;
    si.hStdError = childStdoutWr;   // merge stderr -> reader captures child error text

    juce::String cmdLine = "\"" + childExePath + "\"";
    std::vector<wchar_t> cmdBuffer ((size_t) (cmdLine.length() + 1) * 2);
    cmdLine.copyToUTF16 (cmdBuffer.data(), cmdBuffer.size());

    PROCESS_INFORMATION pi {};
    const BOOL ok = ::CreateProcessW (nullptr, cmdBuffer.data(), nullptr, nullptr,
                                      TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);

    // The child-side ends live only in the child now — close our copies.
    ::CloseHandle (childStdinRd);
    ::CloseHandle (childStdoutWr);

    if (ok == FALSE)
    {
        ::CloseHandle (childStdinWr);
        ::CloseHandle (childStdoutRd);
        return false;
    }

    ::CloseHandle (pi.hThread);
    processHandle = pi.hProcess;
    stdinWriteHandle = childStdinWr;
    stdoutReadHandle = childStdoutRd;

    processRunning.store (true);
    crashReported.store (false);
    stopRequested.store (false);   // restart after stop() must not inherit the flag
    readerDone.store (false);
    lastHeartbeatMs.store (juce::Time::getMillisecondCounter());   // staleness clock starts at spawn

    readerThread = std::make_unique<std::thread> (&PluginHostChildCoordinator::readerLoop, this);
    return true;
}

//==============================================================================
// Reader join (start()/restart() preamble)
//==============================================================================

bool PluginHostChildCoordinator::joinFinishedReader()
{
    if (readerThread == nullptr)
        return true;

    if (! readerThread->joinable())
    {
        readerThread.reset();
        return true;
    }

    // onCrash fires on the reader thread — never join the calling thread
    // (a restart() from inside the callback would deadlock; it fails instead
    // and the host dispatches the restart elsewhere).
    if (readerThread->get_id() == std::this_thread::get_id())
        return false;

    if (! readerDone.load())
    {
        // The crash was reported (onCrash fired) but the reader has not yet
        // exited; it is guaranteed to exit promptly (non-blocking
        // PeekNamedPipe loop), so wait briefly. A genuinely hung reader must
        // never be joined (project iron rule) — fail instead.
        const auto deadline = std::chrono::steady_clock::now()
                            + std::chrono::milliseconds (kRestartJoinWaitMs);
        while (! readerDone.load() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for (std::chrono::milliseconds (5));

        if (! readerDone.load())
            return false;
    }

    readerThread->join();
    readerThread.reset();
    return true;
}

bool PluginHostChildCoordinator::isRunning() const
{
    return processRunning.load();
}

void PluginHostChildCoordinator::setOnCrash (CrashCallback callback)
{
    onCrash = std::move (callback);
}

//==============================================================================
// Protocol I/O
//==============================================================================

bool PluginHostChildCoordinator::sendLine (const juce::String& jsonLine)
{
    if (! processRunning.load())
        return false;

    // WriteFile on a pipe is synchronous and unbuffered at the OS level —
    // nothing to flush; the mutex only serializes concurrent senders.
    std::lock_guard<std::mutex> lock (stdinLock);

    const juce::String fullLine = jsonLine + "\n";
    DWORD written = 0;
    const BOOL ok = ::WriteFile ((HANDLE) stdinWriteHandle,
                                 fullLine.toRawUTF8(),
                                 (DWORD) fullLine.getNumBytesAsUTF8(),
                                 &written, nullptr);
    return ok != FALSE && (int) written == fullLine.getNumBytesAsUTF8();
}

juce::String PluginHostChildCoordinator::popLine (int timeoutMs)
{
    const uint32 deadline = juce::Time::getMillisecondCounter()
                          + (uint32) (timeoutMs > 0 ? timeoutMs : 0);

    for (;;)
    {
        {
            std::lock_guard<std::mutex> lock (lineLock);
            if (! lineQueue.empty())
            {
                const juce::String line = lineQueue.front();
                lineQueue.pop_front();
                return line;
            }
        }

        if (timeoutMs >= 0 && juce::Time::getMillisecondCounter() >= deadline)
            return {};

        std::this_thread::sleep_for (std::chrono::milliseconds (10));
    }
}

//==============================================================================
// Shutdown
//==============================================================================

void PluginHostChildCoordinator::stop()
{
    if (! processRunning.load())
        return;

    sendLine (R"({"cmd":"stop"})");

    if (::WaitForSingleObject ((HANDLE) processHandle, kStopGraceMs) == WAIT_TIMEOUT)
    {
        // Deliberate kill: set stopRequested BEFORE TerminateProcess so the
        // reader never misreports the kill as a crash.
        stopRequested.store (true);
        ::TerminateProcess ((HANDLE) processHandle, 1);
    }
    else
    {
        // Child already gone (gracefully or on its own) — no crash follows,
        // but late pipe-close must not report anything either.
        stopRequested.store (true);
    }

    ::WaitForSingleObject ((HANDLE) processHandle, kExitGraceMs);
    processRunning.store (false);
    closeHandles();

    // Reader observes stopRequested within ~kWatchdogPollMs and exits —
    // joining is safe and never blocks meaningfully. Reset afterwards so the
    // destructor sees a clean (joined) state.
    if (readerThread != nullptr && readerThread->joinable())
    {
        readerThread->join();
        readerThread.reset();
    }
}

//==============================================================================
// Reader thread: stdout lines + heartbeat watchdog (single one-shot thread)
//==============================================================================

void PluginHostChildCoordinator::readerLoop()
{
    char readBuffer[kReadChunkBytes];
    std::string pendingBytes;   // byte-level accumulator (handles partial reads)

    while (! stopRequested.load() && processRunning.load())
    {
        DWORD available = 0;

        // Non-blocking: returns immediately with 0 when no data. Never stuck
        // in a syscall -> stop()/destructor can always reach the loop.
        if (::PeekNamedPipe ((HANDLE) stdoutReadHandle, readBuffer,
                             sizeof (readBuffer), nullptr, &available, nullptr) == FALSE)
        {
            handlePipeClosed();   // ERROR_BROKEN_PIPE: child exited / closed stdout
            break;
        }

        if (available > 0)
        {
            const DWORD toRead = jmin (available, (DWORD) sizeof (readBuffer));
            DWORD bytesRead = 0;

            if (::ReadFile ((HANDLE) stdoutReadHandle, readBuffer, toRead,
                            &bytesRead, nullptr) == FALSE)
            {
                handlePipeClosed();
                break;
            }

            if (bytesRead > 0)
                consumeBytes (readBuffer, (int) bytesRead, pendingBytes);
        }
        else
        {
            checkHeartbeat();
            std::this_thread::sleep_for (std::chrono::milliseconds (kWatchdogPollMs));
        }
    }

    readerDone.store (true);
}

void PluginHostChildCoordinator::consumeBytes (const char* data, int len,
                                            std::string& pendingBytes)
{
    for (int i = 0; i < len; ++i)
    {
        if (data[i] == '\n')
        {
            if (! pendingBytes.empty() && pendingBytes.back() == '\r')
                pendingBytes.pop_back();   // tolerate CRLF

            if (! pendingBytes.empty())
                onLine (juce::String (juce::CharPointer_UTF8 (pendingBytes.c_str())));

            pendingBytes.clear();
        }
        else if (pendingBytes.size() < (size_t) kMaxLineBytes)
        {
            pendingBytes += data[i];
        }
    }
}

void PluginHostChildCoordinator::onLine (const juce::String& line)
{
    if (line.isEmpty())
        return;

    // ANY received line proves liveness (progress lines, responses, errors).
    lastHeartbeatMs.store (juce::Time::getMillisecondCounter());

    {
        std::lock_guard<std::mutex> lock (lineLock);

        lineQueue.push_back (line);
        if (lineQueue.size() > (size_t) kMaxQueuedLines)
            lineQueue.pop_front();

        lastLogLines.push_back (line);
        if (lastLogLines.size() > (size_t) kMaxLastLogLines)
            lastLogLines.pop_front();
    }
}

//==============================================================================
// Watchdog + crash reporting
//==============================================================================

void PluginHostChildCoordinator::checkHeartbeat()
{
    if (heartbeatTimeoutMs <= 0)
        return;

    // uint32 subtraction wraps safely (Time::getMillisecondCounter wraps).
    const uint32 nowMs = juce::Time::getMillisecondCounter();
    if (nowMs - lastHeartbeatMs.load() <= (uint32) heartbeatTimeoutMs)
        return;

    // Only fire while the process is actually still running — if it already
    // exited, the exit-code path owns the outcome.
    if (::WaitForSingleObject ((HANDLE) processHandle, 0) != WAIT_TIMEOUT)
        return;

    ::TerminateProcess ((HANDLE) processHandle, 1);
    processRunning.store (false);   // the kill is final — reflect it immediately
    markCrashed ("heartbeat timeout: child unresponsive for "
                 + juce::String (heartbeatTimeoutMs) + " ms");
}

void PluginHostChildCoordinator::handlePipeClosed()
{
    processRunning.store (false);

    if (stopRequested.load())
        return;   // deliberate teardown (stop/destructor) — not a crash event

    // Give the process a moment to fully terminate so GetExitCodeProcess
    // returns a real code rather than STILL_ACTIVE (259).
    const DWORD waitResult = ::WaitForSingleObject ((HANDLE) processHandle, kExitGraceMs);

    DWORD exitCode = STILL_ACTIVE;
    if (::GetExitCodeProcess ((HANDLE) processHandle, &exitCode) == FALSE)
        exitCode = STILL_ACTIVE;

    if (waitResult != WAIT_OBJECT_0 || exitCode == STILL_ACTIVE)
    {
        // Output pipe closed but the process is still alive — protocol
        // violation (a well-behaved child dies with its stdout).
        ::TerminateProcess ((HANDLE) processHandle, 1);
        markCrashed ("child closed its output without exiting");
    }
    else if (exitCode != 0)
    {
        // Crash condition #2: non-zero exit code (design question 4).
        markCrashed ("child exited with code " + juce::String (exitCode)
                     + " - " + lastLogSummary());
    }
    // exitCode == 0 -> graceful exit, no crash callback.
}

bool PluginHostChildCoordinator::markCrashed (const juce::String& detail)
{
    // stopRequested: teardown in progress (deliberate kill must not report).
    // crashReported.exchange: exactly-once — the watchdog and the exit-code
    // path race to report the same death; the first wins.
    if (stopRequested.load() || crashReported.exchange (true))
        return false;

    crashCountValue.fetch_add (1);   // cumulative; the caller decides when to reset its baseline

    // The crash is final — the child is gone and its pipes are broken. Close
    // the handles here (not only in stop()/destructor) so repeated
    // crash-restart cycles cannot leak kernel handles. Gated behind the
    // exactly-once report so a stop()/destructor teardown that merely races
    // the crash path can never close handles out from under stop().
    closeHandles();

    if (onCrash)
        onCrash (detail);

    return true;
}

juce::String PluginHostChildCoordinator::lastLogSummary() const
{
    std::lock_guard<std::mutex> lock (lineLock);

    if (lastLogLines.empty())
        return "no output received";

    juce::String summary;
    int totalChars = 0;

    for (const auto& line : lastLogLines)
    {
        if (totalChars + line.length() > kMaxLogSummaryChars)
            break;

        summary += line + " | ";
        totalChars += line.length() + 3;
    }

    return summary;
}

//==============================================================================
// Handle cleanup
//==============================================================================

void PluginHostChildCoordinator::closeHandles()
{
    // Mutex-guarded: the crash path (reader thread), stop(), restart() and
    // the destructor can all close concurrently — without the lock two
    // threads could CloseHandle the same HANDLE value twice.
    std::lock_guard<std::mutex> lock (handleLock);

    if (stdinWriteHandle != nullptr)
    {
        ::CloseHandle ((HANDLE) stdinWriteHandle);
        stdinWriteHandle = nullptr;
    }

    if (stdoutReadHandle != nullptr)
    {
        ::CloseHandle ((HANDLE) stdoutReadHandle);
        stdoutReadHandle = nullptr;
    }

    if (processHandle != nullptr)
    {
        ::CloseHandle ((HANDLE) processHandle);
        processHandle = nullptr;
    }
}

int PluginHostChildCoordinator::crashCount() const
{
    return crashCountValue.load();
}

//==============================================================================
// Crash-recovery snapshot cache (D3/T5)
//==============================================================================

void PluginHostChildCoordinator::cacheSnapshot (const juce::String& snapshotResponseLine)
{
    std::lock_guard<std::mutex> lock (snapshotLock);

    const auto doc = juce::JSON::parse (snapshotResponseLine);

    if (doc.isArray())
    {
        // Already a bare params array — store verbatim (no double-wrap).
        cachedSnapshot = snapshotResponseLine;
        return;
    }

    if (doc.isObject())
    {
        const auto params = doc["params"];
        if (params.isArray())
        {
            // Extract the params fragment from the full snapshot_params
            // response — the only part restore_params consumes; compact
            // single-line so it splices cleanly into the request.
            cachedSnapshot = juce::JSON::toString (params, true);
            return;
        }
    }

    // Unparseable / no params array → no recoverable snapshot.
    cachedSnapshot = {};
}

juce::String PluginHostChildCoordinator::lastSnapshot() const
{
    std::lock_guard<std::mutex> lock (snapshotLock);
    return cachedSnapshot;
}
