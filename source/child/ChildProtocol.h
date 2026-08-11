#pragma once

#include <JuceHeader.h>

/**
 * stdin/stdout JSON-line protocol between the Plugin Lab parent process and
 * the out-of-process VST3 host child (Block D — see
 * docs/plan-block-d-out-of-process.md §子进程 IPC 协议契约).
 *
 * One request per line, one response per line, same style as the parent's
 * Named Pipe protocol: cmd discriminator + {"ok":...} + snake_case field
 * names + escapeJsonString-escaped strings (Windows backslash paths must be
 * escaped).
 *
 * Self-contained on purpose: the child executable does NOT include
 * source/ipc/Protocol.h — the two response helpers below are copied verbatim
 * from Protocol.h:79-96 (inline), and escapeJsonString is copied verbatim
 * from source/ipc/CommandParser.cpp:19-36.
 */

namespace ChildProtocol
{
    //==============================================================================
    /** Request commands (subset active in D1b; measure lands in D2, the
        D3a parameter snapshot/restore pair below). */
    namespace Command
    {
        constexpr auto start          = "start";
        constexpr auto load           = "load";
        constexpr auto heartbeat      = "heartbeat";
        constexpr auto stop           = "stop";
        constexpr auto measure        = "measure";
        constexpr auto snapshotParams = "snapshot_params";
        constexpr auto restoreParams  = "restore_params";
    }

    //==============================================================================
    /** Helper to create a response JSON string (copied from Protocol.h:79-86). */
    inline juce::String makeResponse (bool ok, const juce::String& data = {})
    {
        if (data.isEmpty())
            return R"({"ok":)" + juce::String (ok ? "true" : "false") + "}\n";

        // data should be a JSON fragment like "key": value
        return R"({"ok":)" + juce::String (ok ? "true" : "false") + ", " + data + "}\n";
    }

    /** Helper to create a progress update (copied from Protocol.h:88-96). */
    inline juce::String makeProgress (float progress, const juce::String& extraData = {})
    {
        juce::String msg = R"({"ok":true,"progress":)" + juce::String (progress, 3);
        if (extraData.isNotEmpty())
            msg += ", " + extraData;
        msg += "}\n";
        return msg;
    }

    /** Escape a string for embedding inside a JSON string literal (copied from
        CommandParser.cpp:19-36). juce::String::quoted() only doubles quote chars —
        backslashes (Windows paths!) stay unescaped, which yields INVALID JSON
        (`\P`, `\C` are illegal escapes). */
    inline juce::String escapeJsonString (const juce::String& s)
    {
        juce::String out;
        out.preallocateBytes (s.getNumBytesAsUTF8() + 16);
        for (auto c : s)
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

}  // namespace ChildProtocol
