/**
 * TestChildProcess — a minimal stdin/stdout JSON-line double for the
 * ChildProcessCoordinator tests (block D1c).
 *
 * It mirrors the frozen child-process protocol subset that the coordinator
 * tests need (docs/plan-block-d-out-of-process.md "子进程 IPC 协议契约"):
 *
 *   {"cmd":"start"}          -> {"ok":true,"pid":<pid>,"version":1}
 *   {"cmd":"load",...}       -> {"ok":true,"name":"FakeChildPlugin"} (D6:
 *                               the orchestrator's load handshake)
 *   {"cmd":"restore_params",...} -> {"ok":true} (D6: restore handshake)
 *   {"cmd":"measure",...}    -> {"ok":true,"progress":0.1} followed by the
 *                               result line {"ok":true,"samples":240000,
 *                               "rate":48000.0,"export_path":...,"wav_path":
 *                               ...,"name":"FakeChildPlugin","class_id":
 *                               "fake.class","channels":2,
 *                               "latency_samples":0} (D6: orchestrator result
 *                               collection; export_path/wav_path echo the
 *                               request values)
 *   {"cmd":"received"}       -> {"ok":true,"received":["start","load",...]}
 *                               — the list of commands processed so far (D6:
 *                               lets tests assert the exact call sequence;
 *                               the query itself is not logged)
 *   {"cmd":"echo",...}       -> {"ok":true,"echo":"<escaped original line>"}
 *   {"cmd":"exit","code":N}  -> exit with code N (crash-detection test)
 *   {"cmd":"sleep","ms":N}   -> sleep N ms WITHOUT responding (heartbeat-
 *                               timeout test; the host watchdog kills us)
 *   {"cmd":"pulse","ms":N,"interval":M} -> emit {"ok":true,"progress":...}
 *                               every M ms for N ms, then return to the read
 *                               loop (long-running-op-with-heartbeat test:
 *                               the host must NOT treat us as dead)
 *   {"cmd":"stop"}           -> exit 0 (graceful stop test)
 *   stdin EOF                -> exit 0 (host killed us / closed the pipe)
 *   unknown command          -> echo line (tests always get a response)
 *
 * MEASURE MODE OVERRIDE: the orchestrator (D6) spawns a FRESH child on every
 * run, so a "set mode" command could never reach the measuring process. The
 * mode instead lives in a file the test writes before run():
 *
 *   %TEMP%\pluginlab_test_child_mode.txt  ->  "" (default)          progress
 *                                             line + result line
 *                                             "progress_then_result" two
 *                                             progress lines + result line
 *                                             "progress_only"       progress
 *                                             lines forever (host times out)
 *                                             "crash_on_measure"    exit 3 on
 *                                             measure (mid-measure crash)
 *
 * The file is re-read on every measure command, so tests can switch modes
 * between runs without re-spawning.
 *
 * Plain add_executable target on purpose: it only uses Win32 console I/O
 * (no JUCE), so it builds fast and needs no JUCE message loop.
 */
#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace
{
    //--------------------------------------------------------------------------
    // Tiny JSON field extraction for the protocol above (no JSON parser
    // needed in a test double). Whitespace after ':' is tolerated — JUCE's
    // JSON::toString emits `"key": value`, the host may send compact JSON.

    std::string extractString (const std::string& line, const std::string& key)
    {
        const std::string needle = "\"" + key + "\":";
        const auto start = line.find (needle);
        if (start == std::string::npos)
            return {};

        auto valueStart = start + needle.size();
        while (valueStart < line.size() && line[valueStart] == ' ')
            ++valueStart;
        if (valueStart >= line.size() || line[valueStart] != '"')
            return {};

        const auto textStart = ++valueStart;
        const auto end = line.find ('"', textStart);
        if (end == std::string::npos)
            return {};

        return line.substr (textStart, end - textStart);
    }

    std::string extractInt (const std::string& line, const std::string& key)
    {
        const std::string needle = "\"" + key + "\":";
        const auto start = line.find (needle);
        if (start == std::string::npos)
            return {};

        auto pos = start + needle.size();
        while (pos < line.size() && line[pos] == ' ')
            ++pos;

        const auto valueStart = pos;
        while (pos < line.size() && line[pos] >= '0' && line[pos] <= '9')
            ++pos;

        if (pos == valueStart)
            return {};
        return line.substr (valueStart, pos - valueStart);
    }

    //--------------------------------------------------------------------------
    // JSON string escaping for the echo value (backslashes + quotes are the
    // only characters our test lines contain).

    std::string escapeJson (const std::string& s)
    {
        std::string out;
        out.reserve (s.size() + 8);

        for (const char c : s)
        {
            switch (c)
            {
                case '\\': out += "\\\\"; break;
                case '"':  out += "\\\""; break;
                case '\n': out += "\\n";  break;
                case '\r': out += "\\r";  break;
                case '\t': out += "\\t";  break;
                default:   out += c;      break;
            }
        }

        return out;
    }

    //--------------------------------------------------------------------------

    void writeLine (HANDLE hOut, const std::string& line)
    {
        const std::string full = line + "\n";
        DWORD written = 0;
        ::WriteFile (hOut, full.data(), (DWORD) full.size(), &written, nullptr);
    }

    //--------------------------------------------------------------------------
    // Measure-mode override file (see the header comment — the orchestrator
    // restarts the child per run, so the mode travels in a file, not in a
    // set command that would die with the previous process).

    /** Undo the JSON string escapes the raw request line still contains
        (\\ → \, \" → " etc.) — mirrors the real child's juce::JSON::parse
        unescaping so the response round-trips the TRUE value (double-escaping
        would corrupt Windows paths). */
    std::string unescapeJson (const std::string& s)
    {
        std::string out;
        out.reserve (s.size());
        for (size_t i = 0; i < s.size(); ++i)
        {
            if (s[i] == '\\' && i + 1 < s.size())
            {
                switch (s[i + 1])
                {
                    case '\\': out += '\\'; ++i; break;
                    case '"':  out += '"';  ++i; break;
                    case 'n':  out += '\n'; ++i; break;
                    case 'r':  out += '\r'; ++i; break;
                    case 't':  out += '\t'; ++i; break;
                    default:   out += s[i]; break;
                }
            }
            else
            {
                out += s[i];
            }
        }
        return out;
    }

    std::string readModeFile()
    {
        char buffer[MAX_PATH] = {};
        const DWORD len = ::GetTempPathA (MAX_PATH, buffer);
        std::string modePath (buffer, static_cast<size_t> (len));
        modePath += "pluginlab_test_child_mode.txt";

        std::string content;
        FILE* f = nullptr;
        if (::fopen_s (&f, modePath.c_str(), "r") != 0 || f == nullptr)
            return {};

        char chunk[128];
        size_t n = 0;
        while ((n = ::fread (chunk, 1, sizeof (chunk), f)) > 0)
            content.append (chunk, n);
        ::fclose (f);

        while (! content.empty() && (content.back() == '\n' || content.back() == '\r'))
            content.pop_back();
        return content;
    }

    /** The D6 measure result line (export_path/wav_path echo the request —
        unescaped first, like the real child's JSON parse → escape round-trip;
        the other fields are fixed deterministic metadata). */
    std::string measureResultLine (const std::string& requestLine)
    {
        return "{\"ok\":true,\"samples\":240000,\"rate\":48000.0,"
               "\"export_path\":\"" + escapeJson (unescapeJson (extractString (requestLine, "export_path"))) + "\","
               "\"wav_path\":\"" + escapeJson (unescapeJson (extractString (requestLine, "wav_path"))) + "\","
               "\"name\":\"FakeChildPlugin\",\"class_id\":\"fake.class\","
               "\"channels\":2,\"latency_samples\":0}";
    }
}  // namespace

int main()
{
    const HANDLE hIn = ::GetStdHandle (STD_INPUT_HANDLE);
    const HANDLE hOut = ::GetStdHandle (STD_OUTPUT_HANDLE);

    std::vector<std::string> receivedCmds;   // D6: sequence-assertion log
    std::string pending;
    char buffer[1024];

    for (;;)
    {
        DWORD bytesRead = 0;
        if (::ReadFile (hIn, buffer, sizeof (buffer), &bytesRead, nullptr) == FALSE || bytesRead == 0)
            return 0;  // stdin closed — the host went away

        for (DWORD i = 0; i < bytesRead; ++i)
        {
            if (buffer[i] == '\n')
            {
                if (! pending.empty() && pending.back() == '\r')
                    pending.pop_back();

                const std::string line = pending;
                pending.clear();

                const std::string cmd = extractString (line, "cmd");
                if (cmd != "received")
                    receivedCmds.push_back (cmd);

                if (cmd == "start")
                {
                    writeLine (hOut, "{\"ok\":true,\"pid\":"
                                     + std::to_string (::GetCurrentProcessId())
                                     + ",\"version\":1}");
                }
                else if (cmd == "load")
                {
                    writeLine (hOut, "{\"ok\":true,\"name\":\"FakeChildPlugin\"}");
                }
                else if (cmd == "restore_params")
                {
                    writeLine (hOut, "{\"ok\":true}");
                }
                else if (cmd == "measure")
                {
                    const std::string mode = readModeFile();

                    if (mode == "crash_on_measure")
                        return 3;   // mid-measure crash (D6 orchestrator test)

                    if (mode == "progress_only")
                    {
                        // Progress forever (liveness kept up) but never a
                        // result line — the host must time out, not crash us.
                        for (int pulseCount = 0; pulseCount < 50; ++pulseCount)
                        {
                            writeLine (hOut, "{\"ok\":true,\"progress\":0.5}");
                            ::Sleep (200);
                        }
                        continue;   // back to the read loop after ~10 s
                    }

                    if (mode == "progress_then_result")
                    {
                        writeLine (hOut, "{\"ok\":true,\"progress\":0.1}");
                        writeLine (hOut, "{\"ok\":true,\"progress\":0.9}");
                    }
                    else
                    {
                        writeLine (hOut, "{\"ok\":true,\"progress\":0.1}");
                    }

                    writeLine (hOut, measureResultLine (line));
                }
                else if (cmd == "received")
                {
                    std::string arr = "[";
                    for (size_t entry = 0; entry < receivedCmds.size(); ++entry)
                    {
                        if (entry > 0)
                            arr += ",";
                        arr += "\"" + escapeJson (receivedCmds[entry]) + "\"";
                    }
                    arr += "]";
                    writeLine (hOut, "{\"ok\":true,\"received\":" + arr + "}");
                }
                else if (cmd == "echo")
                {
                    writeLine (hOut, "{\"ok\":true,\"echo\":\"" + escapeJson (line) + "\"}");
                }
                else if (cmd == "exit")
                {
                    return std::atoi (extractInt (line, "code").c_str());
                }
                else if (cmd == "sleep")
                {
                    // Deliberately no response: lets the host watchdog fire.
                    ::Sleep ((DWORD) std::atoi (extractInt (line, "ms").c_str()));
                }
                else if (cmd == "pulse")
                {
                    // Long-running operation that keeps the host alive with
                    // periodic progress lines (the heartbeat contract).
                    const auto totalMs = (DWORD) std::atoi (extractInt (line, "ms").c_str());
                    const auto intervalMs = (DWORD) std::atoi (extractInt (line, "interval").c_str());
                    const auto deadline = ::GetTickCount() + totalMs;

                    while (::GetTickCount() < deadline)
                    {
                        writeLine (hOut, "{\"ok\":true,\"progress\":0.5}");
                        ::Sleep (intervalMs);
                    }
                }
                else if (cmd == "stop")
                {
                    return 0;
                }
                else
                {
                    writeLine (hOut, "{\"ok\":true,\"echo\":\"" + escapeJson (line) + "\"}");
                }
            }
            else
            {
                pending += buffer[i];
            }
        }
    }
}
