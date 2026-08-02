#pragma once

#include <JuceHeader.h>

/**
 * Defines the message protocol between Sisyphus (AI controller)
 * and Plugin Lab via Windows Named Pipe.
 *
 * All messages are single-line JSON (one request, one response per line).
 */

namespace Protocol
{

    //==============================================================================
    /** IPC channel name. */
    constexpr auto pipeName = "\\\\.\\pipe\\PluginLab";

    //==============================================================================
    /** Request commands. */
    namespace Command
    {
        constexpr auto loadPlugin   = "loadPlugin";
        constexpr auto setParam     = "setParam";
        constexpr auto getParams    = "getParams";
        constexpr auto measure      = "measure";
        constexpr auto scan         = "scan";
        constexpr auto stop         = "stop";
        constexpr auto exportCmd    = "exportData";
    }

    //==============================================================================
    /** Response status. */
    namespace Status
    {
        constexpr auto ok     = "ok";
        constexpr auto error  = "error";
        constexpr auto progress = "progress";
    }

    //==============================================================================
    /** Measurement types. */
    namespace MeasureType
    {
        constexpr auto freq        = "frequency_response";
        constexpr auto harmonic    = "harmonic";
        constexpr auto compression = "compression";
    }

    //==============================================================================
    /** Input signal sources. The source determines which signal is generated;
     *  the measurement type remains an analysis field. Non-signal sources are
     *  captured raw (no analysis — that is phase 4). */
    namespace Source
    {
        constexpr auto signal  = "signal";
        constexpr auto file    = "file";
        constexpr auto noise   = "noise";
        constexpr auto dynamic = "dynamic";
    }

    //==============================================================================
    /** Helper to create a response JSON string. */
    inline juce::String makeResponse (bool ok, const juce::String& data = {})
    {
        if (data.isEmpty())
            return R"({"ok":)" + juce::String (ok ? "true" : "false") + "}\n";

        // data should be a JSON fragment like "key": value
        return R"({"ok":)" + juce::String (ok ? "true" : "false") + ", " + data + "}\n";
    }

    /** Helper to create a progress update. */
    inline juce::String makeProgress (float progress, const juce::String& extraData = {})
    {
        juce::String msg = R"({"ok":true,"progress":)" + juce::String (progress, 3);
        if (extraData.isNotEmpty())
            msg += ", " + extraData;
        msg += "}\n";
        return msg;
    }

}  // namespace Protocol
