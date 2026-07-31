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
        if (hPipe != nullptr)
        {
            CloseHandle ((HANDLE) hPipe);
            hPipe = nullptr;
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
            PIPE_ACCESS_DUPLEX,
            PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
            PIPE_UNLIMITED_INSTANCES,
            bufferSize, bufferSize,
            0, nullptr);

        if (pipe == INVALID_HANDLE_VALUE)
        {
            wait (1000);
            continue;
        }

        hPipe = pipe;

        if (! ConnectNamedPipe (pipe, nullptr))
        {
            if (GetLastError() != ERROR_PIPE_CONNECTED)
            {
                CloseHandle (pipe);
                hPipe = nullptr;
                continue;
            }
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
        hPipe = nullptr;
        DisconnectNamedPipe (pipe);
        CloseHandle (pipe);
    }
}
