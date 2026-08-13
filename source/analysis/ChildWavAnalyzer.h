// ChildWavAnalyzer.h (namespace-style deep module; static functions)
#pragma once
#include <JuceHeader.h>

/**
 * Host-side analysis entry for out-of-process measurements (ADR-D-6 / D2b):
 * blacklisted plugins are never loaded in the host, so the child process
 * measures and ships back a WAV mirror + metadata (name / class_id /
 * latency_samples) over IPC. This entry turns that WAV into the same export
 * JSON the in-process path (buildExportContext + Export::freqResponseToJSON)
 * produces — two paths coexist, D6 routes between them.
 */
namespace ChildWavAnalyzer
{
    /** Analyze a child-measured sweep/MLS WAV and return the export JSON.
     *  @param wavPath      2*numChannels-channel 24-bit WAV ([dry, wet] layout)
     *  @param numChannels  plugin channel count (dry and wet share it)
     *  @param sampleRate   measurement sample rate (from the measure request)
     *  @param blockSize    measurement block size (from the measure request)
     *  @param excitation   "sweep" or "mls" (mirrors Protocol::Excitation)
     *  @param mlsLength    MLS sequence length (only used when excitation == "mls")
     *  @param pluginName / classId / latencySamples — plugin metadata reported
     *                      by the child in its measure result (ADR-D-6)
     *  @return the frequency_response export JSON, or an empty string when the
     *          WAV cannot be read (WavCaptureReader logged the failure). */
    juce::String analyzeChildFrequencyResponse (const juce::File& wavPath,
                                                int numChannels,
                                                double sampleRate,
                                                int blockSize,
                                                const juce::String& excitation,
                                                int mlsLength,
                                                const juce::String& pluginName,
                                                const juce::String& classId,
                                                int latencySamples);

    /** Analyze a child-measured SequentialTone WAV and return the export JSON
     *  (T1 — same ADR-D-6/D2b pattern as analyzeChildFrequencyResponse).
     *  @param wavPath      2*numChannels-channel 24-bit WAV ([dry, wet] layout)
     *  @param numChannels  plugin channel count (dry and wet share it)
     *  @param sampleRate   measurement sample rate (from the measure request)
     *  @param blockSize    measurement block size (from the measure request)
     *  @param pluginName / classId / latencySamples — plugin metadata reported
     *                      by the child in its measure result (ADR-D-6)
     *  @return the harmonic_analysis export JSON, or an empty string when the
     *          WAV cannot be read (WavCaptureReader logged the failure). */
    juce::String analyzeChildHarmonic (const juce::File& wavPath,
                                       int numChannels,
                                       double sampleRate,
                                       int blockSize,
                                       const juce::String& pluginName,
                                       const juce::String& classId,
                                       int latencySamples);

    /** Analyze a child-measured ToneBurst WAV and return the export JSON
     *  (T2 — same ADR-D-6/D2b pattern as analyzeChildHarmonic).
     *  @param wavPath      2*numChannels-channel 24-bit WAV ([dry, wet] layout)
     *  @param numChannels  plugin channel count (dry and wet share it)
     *  @param sampleRate   measurement sample rate (from the measure request)
     *  @param blockSize    measurement block size (from the measure request)
     *  @param pluginName / classId / latencySamples — plugin metadata reported
     *                      by the child in its measure result (ADR-D-6)
     *  @return the compression_curve export JSON, or an empty string when the
     *          WAV cannot be read (WavCaptureReader logged the failure). */
    juce::String analyzeChildCompression (const juce::File& wavPath,
                                           int numChannels,
                                           double sampleRate,
                                           int blockSize,
                                           const juce::String& pluginName,
                                           const juce::String& classId,
                                           int latencySamples);

    /** Analyze a child-measured dynamic-source dry/wet WAV and return the
     *  gr_timeline export JSON (T4.4 — same ADR-D-6/D2b pattern as
     *  analyzeChildCompression).
     *  @param wavPath      2*numChannels-channel 24-bit WAV ([dry, wet] layout)
     *  @param numChannels  plugin channel count (dry and wet share it)
     *  @param sampleRate   measurement sample rate (from the measure request)
     *  @param blockSize    measurement block size (from the measure request)
     *  @param pluginName / classId / latencySamples — plugin metadata reported
     *                      by the child in its measure result (ADR-D-6)
     *  @return the gr_timeline export JSON, or an empty string when the
     *          WAV cannot be read (WavCaptureReader logged the failure). */
    juce::String analyzeChildGrTimeline (const juce::File& wavPath,
                                         int numChannels,
                                         double sampleRate,
                                         int blockSize,
                                         const juce::String& pluginName,
                                         const juce::String& classId,
                                         int latencySamples);
}
