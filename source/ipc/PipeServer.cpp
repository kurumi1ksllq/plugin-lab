#include "PipeServer.h"
#include <windows.h>

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
            // wakes a synchronous ReadFile.
            std::lock_guard<std::mutex> lock (ioMutex);
            if (hPipe != nullptr)
            {
                ::CancelIoEx ((HANDLE) hPipe, nullptr);
                ::CloseHandle ((HANDLE) hPipe);
                hPipe = nullptr;
            }
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
    DWORD bytesWritten = 0;
    ::WriteFile ((HANDLE) hPipe, responseStr,
                 (DWORD) std::strlen (responseStr), &bytesWritten, nullptr);
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

    while (! threadShouldExit())
    {
        HANDLE pipe = CreateNamedPipeA (
            "\\\\.\\pipe\\PluginLab",
            PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
            PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
            PIPE_UNLIMITED_INSTANCES,
            bufferSize, bufferSize,
            0, nullptr);

        if (pipe == INVALID_HANDLE_VALUE)
        {
            wait (1000);
            continue;
        }

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
            // Connection failed or was cancelled by shutdown() — close exactly
            // once (shutdown() may have already closed the handle) and loop
            // back; the loop condition picks up threadShouldExit().
            std::lock_guard<std::mutex> lock (ioMutex);
            if (hPipe != nullptr)
                ::CloseHandle (pipe);
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
                break;   // client disconnected

            if (available == 0)
            {
                wait (10);
                continue;
            }

            DWORD bytesRead = 0;
            if (! ReadFile (pipe, buffer.data(), bufferSize - 1, &bytesRead, nullptr)
                || bytesRead == 0)
                break;

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
            // Close exactly once: shutdown() may have already closed the
            // handle to wake the synchronous ReadFile above.
            std::lock_guard<std::mutex> lock (ioMutex);
            if (hPipe != nullptr)
            {
                DisconnectNamedPipe (pipe);
                ::CloseHandle (pipe);
                hPipe = nullptr;
            }
            ++connectionGeneration;
        }
    }
}
