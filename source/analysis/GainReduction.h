#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <vector>

/**
 * Gain Reduction Analyzer.
 *
 * Computes a gain-reduction-over-time curve (in dB) from a recorded
 * dry (input) / wet (processed output) pair:
 *
 *   GR_dB(t) = 20 * log10 ( RMS_wet[i+latency .. i+latency+W] /
 *                           RMS_dry[i .. i+W] )
 *
 * where the window starts at sample i, hops forward by W samples
 * (W = round(sr * windowSec), non-overlapping windows), and the wet
 * window is shifted by the plugin's latency so that both windows
 * cover the same source samples.
 *
 * The dry buffer is expected to include a silent tail-pad region
 * (SweepRunner::setTailPadSamples) so that plugin tails are captured;
 * windows whose dry RMS falls below a tiny threshold (silence) carry
 * no GR information and are reported as 0 dB instead of NaN/-inf.
 */
class GainReduction
{
public:
    GainReduction() = default;
    ~GainReduction() = default;

    //==============================================================================
    /** A single point on the GR timeline. */
    struct Point
    {
        double timeSec = 0.0;
        double grDB = 0.0;
    };

    /** Result containing the full GR timeline. */
    struct Result
    {
        std::vector<Point> timeline;
        double sampleRate = 0.0;
        int numPoints = 0;
    };

    //==============================================================================
    /** Analyze the GR curve from dry/wet recordings.
     *  @param dry  Dry (input/reference) audio buffer, incl. silent tail-pad
     *  @param wet  Wet (output/processed) audio buffer, incl. plugin tail
     *  @param sr   Sample rate of the recording
     *  @param latencySamples  Plugin latency in samples (dry arrives before
     *                         wet; wet index i+latency aligns with dry index i)
     *  @param windowSec  RMS window length in seconds (default: 5 ms)
     */
    static Result analyze (const juce::AudioBuffer<float>& dry,
                           const juce::AudioBuffer<float>& wet,
                           double sr,
                           int latencySamples,
                           double windowSec = 0.005);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (GainReduction)
};
