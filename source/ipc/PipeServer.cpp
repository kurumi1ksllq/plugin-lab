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

void PipeServer::startup()
{
    startThread();
}

void PipeServer::shutdown()
{
    if (isThreadRunning())
    {
        {
            const juce::ScopedLock sl (lock);
            if (hPipe != nullptr)
            {
                // CancelIoEx completes a pending overlapped ConnectNamedPipe
                // (or a synchronous ReadFile) so CloseHandle returns
                // immediately instead of blocking. Closing the handle also
                // wakes a synchronous ReadFile.
                ::CancelIoEx ((HANDLE) hPipe, nullptr);
                ::CloseHandle ((HANDLE) hPipe);
                hPipe = nullptr;
            }
        }
        stopThread (5000);
    }
    clientConnected = false;
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
            const juce::ScopedLock sl (lock);
            hPipe = pipe;
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
            const juce::ScopedLock sl (lock);
            if (hPipe != nullptr)
                ::CloseHandle (pipe);
            hPipe = nullptr;
            continue;
        }

        clientConnected = true;

        while (! threadShouldExit())
        {
            DWORD bytesRead = 0;
            BOOL readResult = ReadFile (pipe, buffer.data(),
                                        bufferSize - 1, &bytesRead, nullptr);

            if (! readResult || bytesRead == 0)
                break;

            buffer[bytesRead] = '\0';
            juce::String command (buffer.data(), (int) bytesRead);
            command = command.trim();

            if (commandHandler && command.isNotEmpty())
            {
                juce::String response = commandHandler (command);

                DWORD bytesWritten = 0;
                auto responseStr = response.toRawUTF8();
                WriteFile (pipe, responseStr,
                           (DWORD) std::strlen (responseStr),
                           &bytesWritten, nullptr);
            }
        }

        clientConnected = false;
        {
            // Close exactly once: shutdown() may have already closed the
            // handle to wake the synchronous ReadFile above.
            const juce::ScopedLock sl (lock);
            if (hPipe != nullptr)
            {
                DisconnectNamedPipe (pipe);
                ::CloseHandle (pipe);
                hPipe = nullptr;
            }
        }
    }
}
