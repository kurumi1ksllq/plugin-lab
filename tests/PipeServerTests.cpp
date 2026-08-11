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

namespace
{
    // Connect to the server's real pipe instance. CreateFileA fails while the
    // server is between instances (it creates one instance per connection), so
    // retry — same pattern as Test B, but with a longer window because the
    // reconnect path involves the server first detecting the previous
    // disconnect and returning to its accept loop.
    HANDLE connectClient (int attempts = 200, int sleepMs = 20)
    {
        for (int attempt = 0; attempt < attempts; ++attempt)
        {
            HANDLE client = ::CreateFileA (
                "\\\\.\\pipe\\PluginLab",
                GENERIC_READ | GENERIC_WRITE,
                0, nullptr, OPEN_EXISTING, 0, nullptr);
            if (client != INVALID_HANDLE_VALUE)
            {
                DWORD pipeMode = PIPE_READMODE_MESSAGE;
                ::SetNamedPipeHandleState (client, &pipeMode, nullptr, nullptr);
                return client;
            }
            std::this_thread::sleep_for (std::chrono::milliseconds (sleepMs));
        }
        return INVALID_HANDLE_VALUE;
    }

    // Send one command message and read the one-line response message.
    std::string sendCommand (HANDLE client, const char* command)
    {
        DWORD written = 0;
        ::WriteFile (client, command, (DWORD) std::strlen (command), &written, nullptr);
        char buffer[1024] = {};
        DWORD read = 0;
        ::ReadFile (client, buffer, (DWORD) (sizeof (buffer) - 1), &read, nullptr);
        buffer[read] = '\0';
        return std::string (buffer);
    }
}

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

//==============================================================================
// Test R1 (D4 regression) — the server must detect a client disconnect and
// accept the next connection. Before the fix the inner loop blocked forever in
// the synchronous ReadFile after the client closed its handle (PIPE_WAIT does
// not wake on peer close without pending data), so the next connection could
// never complete ConnectNamedPipe — getScanStatus followed by loadPlugin timed
// out with tools/ipc_client.ps1.
//==============================================================================

TEST_CASE ("PipeServer: reconnects after client disconnect (D4 regression)",
           "[pipeserver][reconnect]")
{
    PipeServer server;

    std::atomic<int> handlerCount { 0 };
    server.setCommandHandler ([&] (const juce::String&) {
        handlerCount.fetch_add (1);
        return juce::String ("world");
    });
    server.startup();

    // Act: first client connects, sends one command, gets a response, then
    // closes its handle — exactly what ipc_client.ps1 does per command.
    HANDLE first = connectClient();
    REQUIRE (first != INVALID_HANDLE_VALUE);
    REQUIRE (sendCommand (first, "hello") == "world");
    ::CloseHandle (first);

    // Act: the server must notice the disconnect and leave the inner loop.
    bool serverSawDisconnect = false;
    for (int i = 0; i < 100 && ! serverSawDisconnect; ++i)
    {
        serverSawDisconnect = ! server.isClientConnected();
        std::this_thread::sleep_for (std::chrono::milliseconds (20));
    }
    REQUIRE (serverSawDisconnect);

    // Assert: a second client can now connect and complete a round trip.
    HANDLE second = connectClient();
    REQUIRE (second != INVALID_HANDLE_VALUE);
    REQUIRE (sendCommand (second, "hello") == "world");
    REQUIRE (handlerCount.load() == 2);

    ::CloseHandle (second);
    server.shutdown();
}

//==============================================================================
// Test R2 — shutdown() must still wake the server thread while a client is
// connected and idle (window-close responsiveness). The poll loop sleeps in
// Thread::wait(50), which shutdown() signals; before the fix the thread could
// be stuck in the synchronous ReadFile and stopThread(5000) hit its timeout.
//==============================================================================

TEST_CASE ("PipeServer: shutdown returns quickly with idle client connected",
           "[pipeserver][shutdown]")
{
    PipeServer server;
    server.setCommandHandler ([] (const juce::String&) { return juce::String ("{}"); });
    server.startup();

    HANDLE client = connectClient();
    REQUIRE (client != INVALID_HANDLE_VALUE);

    // Give run() time to reach the inner poll loop (no message pending).
    std::this_thread::sleep_for (std::chrono::milliseconds (200));

    const auto start = std::chrono::steady_clock::now();
    server.shutdown();
    const auto elapsed = std::chrono::steady_clock::now() - start;
    REQUIRE (std::chrono::duration<double> (elapsed).count() < 2.0);

    ::CloseHandle (client);
}

//==============================================================================
// Test R3 — message framing / response shape regression: several commands on
// one connection each get exactly one exact response (message mode, no byte
// corruption). Guards the JSON line protocol's 1:1 request/response shape.
//==============================================================================

TEST_CASE ("PipeServer: sequential commands round-trip exactly (R3)",
           "[pipeserver][communication]")
{
    PipeServer server;
    server.setCommandHandler ([] (const juce::String& command) {
        return "echo:" + command;
    });
    server.startup();

    HANDLE client = connectClient();
    REQUIRE (client != INVALID_HANDLE_VALUE);

    REQUIRE (sendCommand (client, "cmd1") == "echo:cmd1");
    REQUIRE (sendCommand (client, "cmd2") == "echo:cmd2");
    REQUIRE (sendCommand (client, "cmd3") == "echo:cmd3");

    ::CloseHandle (client);
    server.shutdown();
}
