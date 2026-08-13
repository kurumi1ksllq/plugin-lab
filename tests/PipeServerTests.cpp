/**
 * PipeServer unit tests (TDD: RED phase).
 *
 * Test A (regression): shutdown() must return quickly (< 2.0s) when no client
 *   is connected. Before the fix, the synchronous ConnectNamedPipe blocked
 *   forever and stopThread(5000) hit its full 5s timeout -> the main thread
 *   froze for ~5s when the window was closed.
 * Test B: normal client communication still works after the overlapped
 *   ConnectNamedPipe rework (connect, send command, receive response).
 * Tests S1/S2/S3 (issue #35): oversized messages are drained and answered
 *   instead of dropping the connection; a second server on the same name is
 *   rejected (single startup instance blocks squatting); the pipe DACL grants
 *   the current user + Administrators + SYSTEM only.
 *
 * Note: these tests create the real named pipe \\.\pipe\PluginLab.
 * No other PluginLab instance may be running while they execute, and the
 * cases must run serially (Catch2 default) because the pipe name is global.
 */
#include <catch2/catch_test_macros.hpp>

#include "../source/ipc/PipeServer.h"
#include <windows.h>
#include <sddl.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

// CrashLog recording-stub helpers (defined in CommandParserStubs.cpp)
extern void clearCrashLog();
extern int crashLogErrorCount();
extern bool crashLogContains (const juce::String& substr);

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
// Test C1 (issue #3) — a control command (stop) must be served INLINE while a
// long command runs on the worker. Without this the pipe thread is blocked in
// the long handler and stop is unreachable — the very gap the hosted-measure
// cancel closes. The stop response must arrive BEFORE the long command's
// final response.
//==============================================================================

TEST_CASE ("PipeServer: control command is served while a long command runs (issue #3)",
           "[pipeserver][concurrency]")
{
    PipeServer server;

    std::atomic<bool> longStarted { false };
    std::atomic<bool> releaseLong { false };
    server.setCommandHandler ([&] (const juce::String& command) {
        if (command.contains ("\"cmd\":\"long\""))
        {
            longStarted.store (true);
            while (! releaseLong.load())
                std::this_thread::sleep_for (std::chrono::milliseconds (5));
            return "{\"ok\":true,\"final\":1}\n";
        }
        if (command.contains ("\"cmd\":\"stop\""))
            return "{\"ok\":true,\"cancelled\":true}\n";
        return "{}\n";
    });
    server.setControlCommands ({ "stop" });
    server.startup();

    HANDLE client = connectClient();
    REQUIRE (client != INVALID_HANDLE_VALUE);

    // Send the long command; the worker picks it up.
    DWORD written = 0;
    const char* longCmd = "{\"cmd\":\"long\"}";
    REQUIRE (::WriteFile (client, longCmd, (DWORD) std::strlen (longCmd),
                          &written, nullptr) != FALSE);

    // Wait until the worker is inside the long handler.
    for (int i = 0; i < 200 && ! longStarted.load(); ++i)
        std::this_thread::sleep_for (std::chrono::milliseconds (10));
    REQUIRE (longStarted.load());

    // While the long command runs, the read loop must still serve the
    // control command inline — the stop ack arrives BEFORE the final.
    REQUIRE (sendCommand (client, "{\"cmd\":\"stop\"}") == "{\"ok\":true,\"cancelled\":true}\n");

    // Release the long command and read its final response.
    releaseLong.store (true);
    char buffer[1024] = {};
    DWORD read = 0;
    REQUIRE (::ReadFile (client, buffer, (DWORD) (sizeof (buffer) - 1),
                         &read, nullptr) != FALSE);
    buffer[read] = '\0';
    REQUIRE (std::string (buffer) == "{\"ok\":true,\"final\":1}\n");

    ::CloseHandle (client);
    server.shutdown();
}

//==============================================================================
// Test C2 — a fast (non-control) command sent while the worker is busy is
// QUEUED, not dropped and not run concurrently: its response follows the long
// command's final response in FIFO order.
//==============================================================================

TEST_CASE ("PipeServer: fast command queues behind a running long command",
           "[pipeserver][concurrency]")
{
    PipeServer server;

    std::atomic<bool> longStarted { false };
    std::atomic<bool> releaseLong { false };
    server.setCommandHandler ([&] (const juce::String& command) {
        if (command.contains ("\"cmd\":\"long\""))
        {
            longStarted.store (true);
            while (! releaseLong.load())
                std::this_thread::sleep_for (std::chrono::milliseconds (5));
            return "{\"ok\":true,\"final\":1}\n";
        }
        return "{\"ok\":true,\"fast\":1}\n";
    });
    server.startup();

    HANDLE client = connectClient();
    REQUIRE (client != INVALID_HANDLE_VALUE);

    DWORD written = 0;
    const char* longCmd = "{\"cmd\":\"long\"}";
    REQUIRE (::WriteFile (client, longCmd, (DWORD) std::strlen (longCmd),
                          &written, nullptr) != FALSE);

    for (int i = 0; i < 200 && ! longStarted.load(); ++i)
        std::this_thread::sleep_for (std::chrono::milliseconds (10));
    REQUIRE (longStarted.load());

    // The fast command must not be answered while the long command runs.
    DWORD written2 = 0;
    const char* fastCmd = "{\"cmd\":\"fast\"}";
    REQUIRE (::WriteFile (client, fastCmd, (DWORD) std::strlen (fastCmd),
                          &written2, nullptr) != FALSE);

    // Nothing may be pending: PeekNamedPipe (non-blocking) must see 0 bytes.
    DWORD available = 0, totalBytes = 0;
    BOOL peekOk = ::PeekNamedPipe (client, nullptr, 0, nullptr, &available, &totalBytes);
    REQUIRE (peekOk != FALSE);
    REQUIRE (available == 0);

    // Release: the long final response arrives first, then the queued fast
    // response (FIFO order preserved).
    releaseLong.store (true);
    char buffer[1024] = {};
    DWORD read = 0;
    REQUIRE (::ReadFile (client, buffer, (DWORD) (sizeof (buffer) - 1),
                         &read, nullptr) != FALSE);
    buffer[read] = '\0';
    REQUIRE (std::string (buffer) == "{\"ok\":true,\"final\":1}\n");
    REQUIRE (::ReadFile (client, buffer, (DWORD) (sizeof (buffer) - 1),
                         &read, nullptr) != FALSE);
    buffer[read] = '\0';
    REQUIRE (std::string (buffer) == "{\"ok\":true,\"fast\":1}\n");

    ::CloseHandle (client);
    server.shutdown();
}

//==============================================================================
// Test C3 (issue #2) — emitLine pushes an intermediate progress line to the
// client while the worker serves a long command, BEFORE the final response.
//==============================================================================

TEST_CASE ("PipeServer: emitLine pushes progress during a long command (issue #2)",
           "[pipeserver][concurrency]")
{
    PipeServer server;

    std::atomic<bool> longStarted { false };
    std::atomic<bool> releaseLong { false };
    server.setCommandHandler ([&] (const juce::String& command) {
        if (command.contains ("\"cmd\":\"long\""))
        {
            longStarted.store (true);
            while (! releaseLong.load())
                std::this_thread::sleep_for (std::chrono::milliseconds (5));
            return "{\"ok\":true,\"final\":1}\n";
        }
        return "{}\n";
    });
    server.startup();

    HANDLE client = connectClient();
    REQUIRE (client != INVALID_HANDLE_VALUE);

    DWORD written = 0;
    const char* longCmd = "{\"cmd\":\"long\"}";
    REQUIRE (::WriteFile (client, longCmd, (DWORD) std::strlen (longCmd),
                          &written, nullptr) != FALSE);

    for (int i = 0; i < 200 && ! longStarted.load(); ++i)
        std::this_thread::sleep_for (std::chrono::milliseconds (10));
    REQUIRE (longStarted.load());

    // Push a progress line while the worker is busy; the client must read it
    // before the final response.
    server.emitLine ("{\"ok\":true,\"progress\":0.5,\"event_index\":2,\"event_total\":4}\n");

    char buffer[1024] = {};
    DWORD read = 0;
    REQUIRE (::ReadFile (client, buffer, (DWORD) (sizeof (buffer) - 1),
                         &read, nullptr) != FALSE);
    buffer[read] = '\0';
    REQUIRE (std::string (buffer) == "{\"ok\":true,\"progress\":0.5,\"event_index\":2,\"event_total\":4}\n");

    releaseLong.store (true);
    REQUIRE (::ReadFile (client, buffer, (DWORD) (sizeof (buffer) - 1),
                         &read, nullptr) != FALSE);
    buffer[read] = '\0';
    REQUIRE (std::string (buffer) == "{\"ok\":true,\"final\":1}\n");

    ::CloseHandle (client);
    server.shutdown();
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

//==============================================================================
// Test S1 (issue #35) — an oversized message (larger than the server's
// 65535-byte read cap) must NOT kill the connection: the server answers
// {"ok":false,"error":"command too large"} and the connection stays usable for
// the next command. Before the fix, ReadFile failed with ERROR_MORE_DATA which
// the read loop treated as a disconnect (no branch for it) — the client was
// dropped and its next command could never be served.
//==============================================================================

TEST_CASE ("PipeServer: oversized message gets error response and keeps connection (issue #35)",
           "[pipeserver][communication]")
{
    PipeServer server;
    server.setCommandHandler ([] (const juce::String& command) { return "echo:" + command; });
    server.startup();

    HANDLE client = connectClient();
    REQUIRE (client != INVALID_HANDLE_VALUE);

    // Act: one message of exactly the server's 65536-byte inbound buffer size.
    // Message-mode read caps at bufferSize - 1 (65535), so this message is
    // guaranteed to produce ERROR_MORE_DATA on the server side (one WriteFile
    // = one message; pipe buffers grow beyond the nominal size as needed).
    const size_t bigSize = 65536;
    std::string big (bigSize, 'x');
    DWORD written = 0;
    REQUIRE (::WriteFile (client, big.data(), (DWORD) big.size(), &written, nullptr) != FALSE);

    // Assert: the server answers with the error line and does NOT drop us.
    char buffer[1024] = {};
    DWORD read = 0;
    REQUIRE (::ReadFile (client, buffer, (DWORD) (sizeof (buffer) - 1),
                         &read, nullptr) != FALSE);
    buffer[read] = '\0';
    REQUIRE (std::string (buffer) == R"({"ok":false,"error":"command too large"})");

    // Assert: the connection is still alive — a normal command round-trips.
    REQUIRE (sendCommand (client, "ping") == "echo:ping");

    ::CloseHandle (client);
    server.shutdown();
}

//==============================================================================
// Test S2 (issue #35) — name squatting is blocked: the server creates ONE
// pipe instance at startup with FILE_FLAG_FIRST_PIPE_INSTANCE and keeps it
// alive. A second server on the same name must fail with ERROR_ACCESS_DENIED
// and report the failure (pipeAcquired stays false, crash log records it).
// Before the fix, every server created PIPE_UNLIMITED_INSTANCES without
// FILE_FLAG_FIRST_PIPE_INSTANCE — an attacker holding the name was handed the
// next client connection (MITM on the AI controller).
//==============================================================================

TEST_CASE ("PipeServer: second server on the same name is rejected (issue #35)",
           "[pipeserver][security]")
{
    clearCrashLog();

    PipeServer first;
    first.setCommandHandler ([] (const juce::String&) { return juce::String ("{}"); });
    first.startup();

    // Arrange: the first server owns the name (single instance, kept alive).
    for (int i = 0; i < 100 && ! first.isPipeAcquired(); ++i)
        std::this_thread::sleep_for (std::chrono::milliseconds (20));
    REQUIRE (first.isPipeAcquired());
    REQUIRE (first.getLastPipeCreateError() == 0);

    // Act: a second server tries to create an instance of the same name.
    PipeServer second;
    second.setCommandHandler ([] (const juce::String&) { return juce::String ("{}"); });
    second.startup();

    // Assert: the second server's CreateNamedPipeA fails with
    // ERROR_ACCESS_DENIED — FILE_FLAG_FIRST_PIPE_INSTANCE rejects the
    // squatter instead of silently creating a parallel instance.
    bool sawAccessDenied = false;
    for (int i = 0; i < 100 && ! sawAccessDenied; ++i)
    {
        sawAccessDenied = (second.getLastPipeCreateError()
                           == static_cast<uint32_t> (ERROR_ACCESS_DENIED));
        std::this_thread::sleep_for (std::chrono::milliseconds (20));
    }
    REQUIRE (sawAccessDenied);
    REQUIRE (! second.isPipeAcquired());

    // Assert: the failure is reported through the crash log (clear error).
    REQUIRE (crashLogContains ("PipeServer::CreateNamedPipeA"));

    second.shutdown();
    first.shutdown();
}

//==============================================================================
// Test S3 (issue #35) — the pipe's DACL grants GENERIC_ALL to the current
// user, BUILTIN\Administrators (BA) and SYSTEM (SY), and nothing else
// (deny-unlisted SDDL semantics). Cross-user processes are excluded; a same-
// user process (the AI client, tools/ipc_client.ps1) is admitted. Exercising
// the DACL end-to-end needs a second Windows user, so the test asserts the
// security descriptor that gets attached to the pipe.
//==============================================================================

TEST_CASE ("PipeServer: pipe DACL grants user+admin+system only (issue #35)",
           "[pipeserver][security]")
{
    const auto sddl = PipeServer::getDefaultPipeSddl();

    // Act/Assert: a valid DACL was built with exactly the three allow ACEs.
    REQUIRE (sddl.isNotEmpty());
    REQUIRE (sddl.startsWith ("D:(A;;GA;;;"));
    REQUIRE (sddl.contains ("(A;;GA;;;BA)"));
    REQUIRE (sddl.contains ("(A;;GA;;;SY)"));

    // Assert: the current user's SID is the first principal in the DACL
    // (re-derived independently with the same Win32 calls).
    HANDLE token = nullptr;
    REQUIRE (::OpenProcessToken (::GetCurrentProcess(), TOKEN_QUERY, &token) != FALSE);
    DWORD size = 0;
    ::GetTokenInformation (token, TokenUser, nullptr, 0, &size);
    std::vector<BYTE> tokenInfo (size);
    REQUIRE (::GetTokenInformation (token, TokenUser, tokenInfo.data(), size, &size) != FALSE);
    ::CloseHandle (token);

    auto* tokenUser = reinterpret_cast<TOKEN_USER*> (tokenInfo.data());
    LPWSTR sidStr = nullptr;
    REQUIRE (::ConvertSidToStringSidW (tokenUser->User.Sid, &sidStr) != FALSE);
    const juce::String userSid (sidStr);
    ::LocalFree (sidStr);

    REQUIRE (sddl.contains (userSid));
    REQUIRE (sddl.indexOf (userSid) > 0);   // after the "D:(A;;GA;;;" prefix
}
