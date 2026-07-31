#include "CrashLog.h"

#ifdef JUCE_WINDOWS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winsock2.h>
#pragma comment(lib, "ws2_32.lib")
#endif

void CrashLog::write (Level level,
                      const juce::String& operation,
                      const juce::String& detail,
                      const juce::String& file,
                      int line)
{
    juce::String entry;
    entry << "[" << juce::Time::getCurrentTime().toString (true, true) << "] ";

    switch (level)
    {
        case Info:    entry << "INFO ";    break;
        case Warning: entry << "WARN ";    break;
        case Error:   entry << "ERROR ";   break;
    }

    entry << operation;

    if (detail.isNotEmpty())
        entry << " | " << detail;

    if (file.isNotEmpty())
        entry << " | " << file << ":" << line;

    entry << "\n";

#ifdef JUCE_WINDOWS
    auto utf8 = entry.toRawUTF8();

    // 1. Write to file (flushed immediately)
    auto path = juce::File::getSpecialLocation (
        juce::File::SpecialLocationType::tempDirectory)
        .getChildFile ("pluginlab_crashlog.txt")
        .getFullPathName();

    HANDLE h = CreateFileA (path.toRawUTF8(), FILE_APPEND_DATA,
                            FILE_SHARE_READ, nullptr, OPEN_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE)
    {
        DWORD written = 0;
        WriteFile (h, utf8, (DWORD) strlen (utf8), &written, nullptr);
        FlushFileBuffers (h);
        CloseHandle (h);
    }

    // 2. Broadcast via UDP for real-time monitoring
    udpSend (utf8, (int) strlen (utf8));
#else
    auto logFile = juce::File::getSpecialLocation (
        juce::File::SpecialLocationType::tempDirectory)
        .getChildFile ("pluginlab_crashlog.txt");
    logFile.appendText (entry);
#endif

    juce::Logger::outputDebugString (entry.trimEnd());
}

void CrashLog::udpSend (const char* data, int len)
{
#ifdef JUCE_WINDOWS
    static SOCKET sock = []() -> SOCKET {
        WSADATA wsa;
        if (WSAStartup (MAKEWORD (2, 2), &wsa) != 0) return INVALID_SOCKET;
        auto s = socket (AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (s == INVALID_SOCKET) return INVALID_SOCKET;
        u_long nonBlock = 1;
        ioctlsocket (s, FIONBIO, &nonBlock);
        return s;
    }();

    if (sock == INVALID_SOCKET) return;

    sockaddr_in addr {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons (43210);
    addr.sin_addr.s_addr = htonl (INADDR_LOOPBACK);

    sendto (sock, data, len, 0, (sockaddr*) &addr, sizeof (addr));
#endif
}
