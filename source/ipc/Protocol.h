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
        constexpr auto exportWav    = "exportWav";
        constexpr auto getScanStatus = "getScanStatus";   // 计划步骤 5：插件扫描状态快照
        constexpr auto dataset      = "dataset";          // 批量采集：默认 4 类型 + 可选 scan/compression_family
        constexpr auto recordTimeline = "recordTimeline"; // 参数自动化录制（B2，非阻塞事件录制）
        constexpr auto stopTimeline   = "stopTimeline";   // 停止录制并导出 timeline JSON
        constexpr auto playTimeline   = "playTimeline";   // 播放 timeline（自动化 + 音频采集）
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
        constexpr auto grTimeline  = "gr_timeline";
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
    /** Frequency-response excitation. */
    namespace Excitation
    {
        constexpr auto sweep = "sweep";   // default (backward compatible)
        constexpr auto mls   = "mls";
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
