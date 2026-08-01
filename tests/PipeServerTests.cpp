/**
 * PipeServer unit tests (TDD: RED phase).
 *
 * Test A (regression): shutdown() must return quickly (< 2.0s) when no client
 *   is connected. Before the fix, the synchronous ConnectNamedPipe blocked
 *   forever and stopThread(5000) hit its full 5s timeout -> the main thread
 *   froze for ~5s when the window was closed.
 * Test B: normal client communication still works after the overlapped
 *   ConnectNamedPipe rework (connect, send command, receive response).
 *
 * Note: these tests create the real named pipe \\.\pipe\PluginLab.
 * No other PluginLab instance may be running while they execute, and the
 * cases must run serially (Catch2 default) because the pipe name is global.
 */
#include <catch2/catch_test_macros.hpp>

#include "../source/ipc/PipeServer.h"
#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>

//==============================================================================
// Test A — regression: shutdown must not block ~5s waiting for a client
//==============================================================================

TEST_CASE ("PipeServer: shutdown returns quickly with no client connected",
           "[pipeserver][shutdown]")
{
    PipeServer server;
    server.setCommandHandler ([] (const juce::String&) { return juce::String ("{}"); });
    server.startup();

    // Give run() time to create the pipe instance and block in
    // ConnectNamedPipe before we hit shutdown().
    std::this_thread::sleep_for (std::chrono::milliseconds (200));

    const auto start = std::chrono::steady_clock::now();
    server.shutdown();
    const auto elapsed = std::chrono::steady_clock::now() - start;

    const double seconds = std::chrono::duration<double> (elapsed).count();

    // Bug: synchronous ConnectNamedPipe can't be woken by CloseHandle, so
    // stopThread(5000) burns the full 5s timeout. Fixed: CancelIoEx wakes
    // the overlapped connect, shutdown returns in milliseconds.
    REQUIRE (seconds < 2.0);
}

//==============================================================================
// Test B — communication still works after the overlapped rework
//==============================================================================

TEST_CASE ("PipeServer: client can connect and receive response",
           "[pipeserver][communication]")
{
    PipeServer server;

    std::atomic<bool> handlerCalled { false };
    juce::String capturedCommand;
    server.setCommandHandler ([&] (const juce::String& command) {
        handlerCalled.store (true);
        capturedCommand = command;
        return juce::String ("world");
    });
    server.startup();

    // Connect as a client. Retry briefly: the server thread needs time to
    // create the pipe instance; OPEN_EXISTING fails with FILE_NOT_FOUND
    // until it exists, then blocks until ConnectNamedPipe accepts us.
    HANDLE client = INVALID_HANDLE_VALUE;
    for (int attempt = 0; attempt < 50 && client == INVALID_HANDLE_VALUE; ++attempt)
    {
        client = ::CreateFileA (
            "\\\\.\\pipe\\PluginLab",
            GENERIC_READ | GENERIC_WRITE,
            0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (client == INVALID_HANDLE_VALUE)
            std::this_thread::sleep_for (std::chrono::milliseconds (20));
    }
    REQUIRE (client != INVALID_HANDLE_VALUE);

    // Match the server's message read mode so ReadFile returns whole messages.
    DWORD pipeMode = PIPE_READMODE_MESSAGE;
    ::SetNamedPipeHandleState (client, &pipeMode, nullptr, nullptr);

    // Send one command message.
    const char* command = "hello";
    DWORD written = 0;
    REQUIRE (::WriteFile (client, command, (DWORD) std::strlen (command),
                          &written, nullptr) != FALSE);

    // Read the response message.
    char buffer[1024] = {};
    DWORD read = 0;
    REQUIRE (::ReadFile (client, buffer, (DWORD) (sizeof (buffer) - 1),
                         &read, nullptr) != FALSE);
    buffer[read] = '\0';

    REQUIRE (std::string (buffer) == "world");
    REQUIRE (handlerCalled.load());
    REQUIRE (capturedCommand == "hello");

    ::CloseHandle (client);
    server.shutdown();
}
