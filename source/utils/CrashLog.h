#pragma once

#include <JuceHeader.h>

/**
 * Crash-safe logger with REAL-TIME streaming.
 *
 * Every log entry is:
 *  1. Written to file with FlushFileBuffers (crash-safe)
 *  2. Broadcast via UDP to 127.0.0.1:43210 (real-time monitoring)
 *  3. Sent to OutputDebugString
 *
 * To monitor in real-time (PowerShell):
 *   $udp = [System.Net.Sockets.UdpClient]::new(43210)
 *   while($true) { $udp.Receive([ref]$null) | %{ [Text.Encoding]::UTF8.GetString($_) } }
 *
 * Log file: %%TEMP%%/pluginlab_crashlog.txt
 */
class CrashLog
{
public:
    enum Level { Info, Warning, Error };

    static void write (Level level,
                       const juce::String& operation,
                       const juce::String& detail = {},
                       const juce::String& file = {},
                       int line = 0);

private:
    CrashLog() = delete;

    static void udpSend (const char* data, int len);
};

#define CRASH_LOG_INFO(op, detail) \
    CrashLog::write (CrashLog::Info, op, detail, __FILE__, __LINE__)

#define CRASH_LOG_WARN(op, detail) \
    CrashLog::write (CrashLog::Warning, op, detail, __FILE__, __LINE__)

#define CRASH_LOG_ERR(op, detail) \
    CrashLog::write (CrashLog::Error, op, detail, __FILE__, __LINE__)
