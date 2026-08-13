#include "PipeServer.h"
#include "Protocol.h"
#include "../utils/CrashLog.h"
#include <windows.h>
#include <sddl.h>

PipeServer::PipeServer() : Thread ("PipeServer") {}

PipeServer::~PipeServer()
{
    shutdown();
}

void PipeServer::setCommandHandler (CommandHandler handler)
{
    commandHandler = std::move (handler);
}

void PipeServer::setControlCommands (std::vector<juce::String> controlCommandNames_)
{
    controlCommandNames = std::move (controlCommandNames_);
}

juce::String PipeServer::getDefaultPipeSddl()
{
    // Current user's SID (TokenUser) — the primary principal allowed in.
    HANDLE token = nullptr;
    if (! ::OpenProcessToken (::GetCurrentProcess(), TOKEN_QUERY, &token))
        return {};

    DWORD size = 0;
    ::GetTokenInformation (token, TokenUser, nullptr, 0, &size);
    std::vector<BYTE> tokenInfo (size);
    const BOOL gotUser = ::GetTokenInformation (token, TokenUser, tokenInfo.data(), size, &size);
    ::CloseHandle (token);

    if (! gotUser)
        return {};

    auto* tokenUser = reinterpret_cast<TOKEN_USER*> (tokenInfo.data());
    LPWSTR userSid = nullptr;
    if (! ::ConvertSidToStringSidW (tokenUser->User.Sid, &userSid))
        return {};

    // D: — a DACL with only these three allow ACEs; every other SID is denied
    // by the default deny-unlisted semantics of an SDDL DACL.
    // <userSid> = current user, BA = BUILTIN\Administrators, SY = SYSTEM.
    const juce::String sddl = "D:(A;;GA;;;" + juce::String (userSid)
                            + ")(A;;GA;;;BA)(A;;GA;;;SY)";
    ::LocalFree (userSid);
    return sddl;
}

bool PipeServer::isControlCommand (const juce::String& commandJson) const
{
    if (controlCommandNames.empty() || commandJson.isEmpty())
        return false;

    const auto parsed = juce::JSON::parse (commandJson);
    const auto* obj = parsed.getDynamicObject();
    if (obj == nullptr)
        return false;

    const auto cmd = obj->getProperty ("cmd").toString();
    for (const auto& name : controlCommandNames)
        if (cmd == name)
            return true;
    return false;
}

void PipeServer::startup()
{
    workerThread = std::thread ([this] { workerLoop(); });
    startThread();
}

void PipeServer::shutdown()
{
    if (isThreadRunning())
    {
        {
            // CancelIoEx completes a pending overlapped ConnectNamedPipe
            // (or a synchronous ReadFile) so CloseHandle returns
            // immediately instead of blocking. Closing the handle also
            // wakes a synchronous ReadFile. This closes the SINGLE pipe
            // instance (issue #35); hPipe aliases it while a client is
            // connected, so it is nulled too.
            std::lock_guard<std::mutex> lock (ioMutex);
            if (hPipeInstance != nullptr)
            {
                ::CancelIoEx ((HANDLE) hPipeInstance, nullptr);
                ::CloseHandle ((HANDLE) hPipeInstance);
                hPipeInstance = nullptr;
            }
            hPipe = nullptr;
        }
        stopThread (5000);
    }
    clientConnected = false;

    // Stop the worker: Main.cpp cancels the session BEFORE shutdown(), so an
    // in-flight command returns promptly and the bounded join below succeeds.
    {
        std::lock_guard<std::mutex> lock (workerMutex);
        workerExit = true;
    }
    workerCv.notify_all();
    if (workerThread.joinable())
        workerThread.join();
}

void PipeServer::emitLine (const juce::String& line)
{
    // Only during a worker-served command: the GUI path never pushes.
    if (! workerBusy.load())
        return;
    writeLine (line, connectionGeneration, true);
}

void PipeServer::writeLine (const juce::String& response, uint64_t generation, bool requireWorkerBusy)
{
    if (requireWorkerBusy && ! workerBusy.load())
        return;

    std::lock_guard<std::mutex> lock (ioMutex);
    if (hPipe == nullptr || generation != connectionGeneration)
        return;

    auto responseStr = response.toRawUTF8();
    const DWORD length = (DWORD) std::strlen (responseStr);
    DWORD bytesWritten = 0;
    const BOOL writeOk = ::WriteFile ((HANDLE) hPipe, responseStr,
                                      length, &bytesWritten, nullptr);

    if (writeOk == FALSE || bytesWritten < length)
    {
        // Write failed or partial: the connection is dead (peer closed,
        // broken pipe). Reset the connection view — the read loop detects the
        // broken pipe on its next poll and reconnects the next client. The
        // single pipe instance itself is NOT closed here (issue #35): it is
        // reused across connections. (Before the fix, WriteFile failures were
        // silently swallowed and the stale response was lost without notice.)
        clientConnected = false;
        hPipe = nullptr;
        ++connectionGeneration;
    }
}

void PipeServer::workerLoop()
{
    for (;;)
    {
        std::pair<juce::String, uint64_t> job;
        {
            std::unique_lock<std::mutex> lock (workerMutex);
            workerCv.wait (lock, [&] { return workerExit || ! workerQueue.empty(); });
            if (workerExit)
            {
                workerBusy.store (false);
                return;
            }
            job = std::move (workerQueue.front());
            workerQueue.pop_front();
        }

        // The handler may block for seconds (measure / playTimeline). The
        // read loop keeps serving control commands meanwhile (issue #3).
        const auto response = commandHandler ? commandHandler (job.first)
                                             : juce::String ("{}");
        writeLine (response, job.second, false);

        {
            std::lock_guard<std::mutex> lock (workerMutex);
            if (workerQueue.empty())
                workerBusy.store (false);
        }
    }
}

void PipeServer::run()
{
    const int bufferSize = 65536;
    std::vector<char> buffer (bufferSize);

    // issue #35 — pipe hardening:
    // 1. ONE pipe instance is created at startup with FILE_FLAG_FIRST_PIPE_INSTANCE
    //    and kept alive for the app lifetime, reused across connections via the
    //    ConnectNamedPipe / DisconnectNamedPipe loop below. A squatter that holds
    //    the well-known name is rejected with ERROR_ACCESS_DENIED — it is never
    //    handed the next client connection (MITM on the AI controller). We log the
    //    failure and retry every second: if the name is freed (attacker exits, a
    //    second PluginLab instance quits), we pick it up. PIPE_UNLIMITED_INSTANCES
    //    stays legal: as the first instance of the name it may claim all instances
    //    it wants, and we only ever create one.
    // 2. Explicit security descriptor: the DACL grants GENERIC_ALL to the current
    //    user, BUILTIN\Administrators and SYSTEM; every other SID is denied. A
    //    secret handshake / token exchange is deliberately NOT added — that is a
    //    client-side protocol change, tracked as a separate decision. On SDDL
    //    failure we log loudly and fall back to the default DACL (no worse than
    //    the pre-hardening state) rather than refusing to serve the AI client.
    const auto sddl = getDefaultPipeSddl();
    SECURITY_ATTRIBUTES securityAttributes = {};
    PSECURITY_DESCRIPTOR securityDescriptor = nullptr;
    if (sddl.isNotEmpty()
        && ::ConvertStringSecurityDescriptorToSecurityDescriptorW (
            sddl.toWideCharPointer(), SDDL_REVISION_1, &securityDescriptor, nullptr))
    {
        securityAttributes.nLength = sizeof (SECURITY_ATTRIBUTES);
        securityAttributes.bInheritHandle = FALSE;
        securityAttributes.lpSecurityDescriptor = securityDescriptor;
    }
    else
    {
        CRASH_LOG_ERR ("PipeServer::run",
                       "failed to build pipe DACL; falling back to the default security descriptor");
    }

    HANDLE pipe = INVALID_HANDLE_VALUE;
    while (! threadShouldExit())
    {
        pipe = ::CreateNamedPipeA (
            Protocol::pipeName,
            PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
            PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
            PIPE_UNLIMITED_INSTANCES,
            bufferSize, bufferSize,
            0, &securityAttributes);

        if (pipe != INVALID_HANDLE_VALUE)
            break;

        // Name already held: another PluginLab instance or an attacker
        // squatting \\.\pipe\PluginLab. Never silently fall back to unlimited
        // instances (that would hand connections to the squatter).
        lastPipeCreateError.store (::GetLastError());
        CRASH_LOG_ERR ("PipeServer::CreateNamedPipeA",
                       "pipe name \\\\.\\pipe\\PluginLab already in use (error "
                       + juce::String ((int) lastPipeCreateError.load()) + ")");
        wait (1000);
    }

    if (securityDescriptor != nullptr)
        ::LocalFree (securityDescriptor);

    if (pipe == INVALID_HANDLE_VALUE)
        return;   // threadShouldExit() during acquisition — nothing to clean up

    lastPipeCreateError.store (0);
    pipeAcquired.store (true);
    {
        std::lock_guard<std::mutex> lock (ioMutex);
        hPipeInstance = pipe;
    }

    // Accept loop: connect one client, serve it, reset the instance, repeat.
    while (! threadShouldExit())
    {
        // Assign before ConnectNamedPipe so shutdown() can CancelIoEx/CloseHandle it.
        {
            std::lock_guard<std::mutex> lock (ioMutex);
            hPipe = pipe;
            ++connectionGeneration;
        }

        // Overlapped connect: unlike the synchronous ConnectNamedPipe, it can be
        // interrupted by shutdown(), so window close stays responsive.
        OVERLAPPED ov = {};
        ov.hEvent = ::CreateEventW (nullptr, TRUE, FALSE, nullptr);

        BOOL connected = ::ConnectNamedPipe (pipe, &ov);
        DWORD connectError = ::GetLastError();
        if (! connected && connectError == ERROR_IO_PENDING)
        {
            // Wait for the connection, polling shutdown()'s exit flag so a
            // connect can never outlive shutdown (a fresh pipe created in the
            // shutdown race window gets abandoned within 50ms).
            while (! threadShouldExit())
            {
                if (::WaitForSingleObject (ov.hEvent, 50) == WAIT_OBJECT_0)
                    break;
            }

            if (! threadShouldExit())
            {
                DWORD bytes = 0;
                connected = ::GetOverlappedResult (pipe, &ov, &bytes, FALSE);
                connectError = ::GetLastError();
            }
        }

        if (ov.hEvent != nullptr)
            ::CloseHandle (ov.hEvent);

        if (! connected && connectError != ERROR_PIPE_CONNECTED)
        {
            // Connection failed or was cancelled by shutdown() — reset the
            // connection view and loop back. The instance handle is owned for
            // the app lifetime (single-instance, issue #35): run() never
            // closes it here; shutdown() does, and the loop condition below
            // then exits.
            std::lock_guard<std::mutex> lock (ioMutex);
            hPipe = nullptr;
            ++connectionGeneration;
            continue;
        }

        clientConnected = true;

        // Poll-based read loop: PeekNamedPipe (non-blocking) instead of a
        // blocking ReadFile. A pending synchronous ReadFile on the same
        // message-mode handle is BROKEN by a concurrent WriteFile from the
        // worker / emitLine thread (observed: the pending read fails and the
        // loop mistakes it for a disconnect, ERROR_PIPE_NOT_CONNECTED on the
        // client). Polling leaves no pending read, so concurrent writes are
        // safe — the same pattern ChildProcessCoordinator's reader uses.
        while (! threadShouldExit())
        {
            DWORD available = 0, totalBytes = 0;
            if (! ::PeekNamedPipe (pipe, nullptr, 0, nullptr, &available, &totalBytes))
            {
                // In message mode, PeekNamedPipe fails with ERROR_MORE_DATA
                // when the current message is larger than the (null) buffer —
                // an oversized message, not a disconnect. totalBytes is still
                // reported on that failure.
                if (::GetLastError() != ERROR_MORE_DATA)
                    break;   // client disconnected
                available = totalBytes;
            }

            if (available == 0)
            {
                wait (10);
                continue;
            }

            DWORD bytesRead = 0;
            BOOL readOk = ::ReadFile (pipe, buffer.data(), bufferSize - 1, &bytesRead, nullptr);

            if (! readOk && ::GetLastError() == ERROR_MORE_DATA)
            {
                // Oversized message (> 65535 bytes): drain the remainder so
                // the connection stays usable, then answer with an error.
                // Before the fix this path had no branch — the read loop
                // treated MORE_DATA as a disconnect and dropped the client
                // (issue #35).
                for (;;)
                {
                    DWORD drained = 0;
                    if (::ReadFile (pipe, buffer.data(), bufferSize - 1, &drained, nullptr) != FALSE)
                        break;   // whole message consumed
                    if (::GetLastError() != ERROR_MORE_DATA)
                        break;   // hard error — the loop below exits
                }
                writeLine (R"({"ok":false,"error":"command too large"})", connectionGeneration, false);
                continue;
            }

            if (! readOk || bytesRead == 0)
                break;   // client disconnected

            buffer[bytesRead] = '\0';
            juce::String command (buffer.data(), (int) bytesRead);
            command = command.trim();

            if (! commandHandler || command.isEmpty())
                continue;

            if (isControlCommand (command))
            {
                // Control commands (stop / status snapshots) are served
                // inline so they stay reachable while a long command runs.
                const auto response = commandHandler (command);
                writeLine (response, connectionGeneration, false);
            }
            else
            {
                // Everything else queues on the worker: all plugin/session
                // access is serialized there (never concurrent), and the read
                // loop stays free for control commands (issue #3).
                workerBusy.store (true);
                {
                    std::lock_guard<std::mutex> lock (workerMutex);
                    workerQueue.emplace_back (command, connectionGeneration);
                }
                workerCv.notify_one();
            }
        }

        clientConnected = false;
        {
            // Detach the connection view; the single instance stays alive for
            // the next client (single-instance loop, issue #35). Writers see
            // hPipe == nullptr again; the generation bump invalidates stale
            // queued responses.
            std::lock_guard<std::mutex> lock (ioMutex);
            hPipe = nullptr;
            ++connectionGeneration;
        }
        // Reset the instance for the next ConnectNamedPipe. Harmless if
        // shutdown() already closed the handle.
        ::DisconnectNamedPipe (pipe);
    }
}
