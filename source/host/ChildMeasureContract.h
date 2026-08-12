#pragma once

#include <JuceHeader.h>

/**
 * Frozen contract between the CommandParser routing layer and the
 * out-of-process measurement orchestrator (Block D, D6 — see
 * docs/plan-block-d-out-of-process.md ADR-D-5/6/7 + §子进程 IPC 协议契约).
 *
 * A blacklisted plugin is never loaded in the parent; the orchestrator
 * forwards the measure request to PluginHostChild, collects the result
 * (WAV + metadata), and hands it back to the routing layer as a
 * ChildMeasureResult — the same export shape the in-process path produces
 * (ADR-D-6). Field constants below are the single source of truth shared
 * by host and child to prevent vocabulary drift.
 */

namespace ChildMeasureContract
{
    //==============================================================================
    // Protocol field names (must match the child's ChildProtocol + plan doc
    // contract table exactly — do NOT edit one side only).
    namespace Field
    {
        constexpr auto cmd          = "cmd";
        constexpr auto measure      = "measure";
        constexpr auto type         = "type";
        constexpr auto excitation   = "excitation";
        constexpr auto sampleRate   = "sample_rate";
        constexpr auto blockSize    = "block_size";
        constexpr auto exportPath   = "export_path";
        constexpr auto wavPath      = "wav_path";
        constexpr auto samples      = "samples";
        constexpr auto rate         = "rate";
        constexpr auto name         = "name";
        constexpr auto classId      = "class_id";
        constexpr auto channels     = "channels";
        constexpr auto latency      = "latency_samples";
        constexpr auto params       = "params";
        constexpr auto id           = "id";
        constexpr auto value        = "value";
    }

    //==============================================================================
    /** Measure request forwarded to the child (mirrors the parent measure
     *  command vocabulary: type / excitation / sample_rate / block_size /
     *  export_path / wav_path). */
    struct ChildMeasureRequest
    {
        juce::String type;        // "frequency_response" | "harmonic" (T1; others → ADR-D-7)
        juce::String excitation;  // "sweep" | "mls"
        double sampleRate = 48000.0;
        int blockSize = 512;
        juce::String exportPath;
        juce::String wavPath;
    };

    /** Measure result reported by the child (ADR-D-6 metadata + counts). */
    struct ChildMeasureResult
    {
        int64_t samples = 0;
        double rate = 0.0;
        juce::String exportPath;
        juce::String wavPath;
        juce::String name;
        juce::String classId;
        int channels = 0;
        int latencySamples = 0;
    };

    /** Error result sentinel (result path failed — orchestrator sets
     *  result=false and reports via the callback's return). */
    struct ChildMeasureOutcome
    {
        bool ok = false;
        juce::String error;
        ChildMeasureResult result;
    };

    /** Routing hook the CommandParser invokes for a blacklisted plugin.
     *  Implemented by ChildMeasureOrchestrator (host-side, owns the
     *  coordinator + D3 crash-recovery sequence); nullptr when no child
     *  support is configured (→ explicit error, never host-direct). */
    using Callback = std::function<ChildMeasureOutcome (const ChildMeasureRequest&)>;

}  // namespace ChildMeasureContract
