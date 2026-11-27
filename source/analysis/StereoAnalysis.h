#pragma once

#include <JuceHeader.h>

/**
 * MidSide (MS) stereo analysis for the stereo_width measurement (issue #104 /
 * ADR-0003, DESIGN.md §3.4).
 *
 * Input is the two recorded runs of the dual excitation:
 *
 *   Run A - mid-only  (L = R = sweep)  -> wet M / dry M gives H_M(f)
 *   Run B - side-only (L = -R = sweep) -> wet S / dry S gives H_S(f)
 *
 * from which the analyser derives the four exported curves:
 *
 *   mid_response   H_M(f)   = wetM / dryM   (MidSide-decoded, Run A)
 *   side_response  H_S(f)   = wetS / dryS   (MidSide-decoded, Run B)
 *   correlation    rho(f)   = |Sxy|^2 / (Sxx * Syy) of the wet L/R pair (Run A)
 *   width_db       width(f) = 20*log10(|M|/|S|) = H_M(f) - H_S(f) in dB
 *
 * rho is 1.0 for a perfectly correlated (mono) output and drops as the plugin
 * decorrelates L/R; width_db rises as the side path is attenuated (narrower)
 * and falls as it is boosted (wider). Both are read per frequency bin, so a
 * frequency-dependent widener is visible.
 *
 * MS decode: M = (L + R) / 2, S = (L - R) / 2. The H1 estimator (Hann-windowed
 * overlap with hop = size/4, H1 = Sxy / Sxx) matches FreqResponse, and the
 * phase post-processing mirrors FreqResponse::applyPhasePost (unwrap + latency
 * ramp removal) - L and R share the plugin latency, so the decoded M/S do too.
 */
class StereoAnalysis
{
public:
    StereoAnalysis() = default;
    ~StereoAnalysis() = default;

    //==============================================================================
    /** One point of an MS response curve. */
    struct Point
    {
        double frequency = 0.0;
        double magnitudeDB = 0.0;
        double phaseDeg = 0.0;
    };

    /** One point of the L/R correlation curve.

        rho is the magnitude-squared coherence |Sxy|^2 / (Sxx*Syy), so it lives
        in [0, 1]: 1.0 = perfectly correlated (mono/untouched), lower = the
        plugin decorrelated the field. It is deliberately unsigned — a polarity
        flip (L = -R) also reads 1.0, because this measure asks "how related",
        not "in which polarity". */

    struct CorrelationPoint
    {
        double frequency = 0.0;
        double rho = 0.0;
    };

    /** One point of the width curve (dB). */
    struct WidthPoint
    {
        double frequency = 0.0;
        double widthDB = 0.0;
    };

    /** Result of a stereo analysis. */
    struct Result
    {
        std::vector<Point> midResponse;
        std::vector<Point> sideResponse;
        std::vector<CorrelationPoint> correlation;
        std::vector<WidthPoint> widthDB;
        double sampleRate = 0.0;

        /** True only when the analysis actually ran on a stereo pair with
         *  enough samples for at least one FFT window. A false flag is an
         *  explicit "not measurable" signal - callers must report an error
         *  instead of exporting an empty (or mono-fallback) stereo_width. */
        bool valid = false;
    };

    //==============================================================================
    /** Analyse the two MidSide excitation runs.
     *
     *  @param midDry   Run A dry recording (mid-only excitation)
     *  @param midWet   Run A wet recording (plugin output)
     *  @param sideDry  Run B dry recording (side-only excitation)
     *  @param sideWet  Run B wet recording (plugin output)
     *  @param sr       Sample rate of both recordings
     *  @param latencySamples  Plugin latency, applied as phase-ramp compensation
     *                  (same convention as FreqResponse::setLatencySamples)
     *
     *  All four buffers must carry at least two channels; a mono buffer leaves
     *  Result::valid false (nothing is decoded, nothing is exported).
     */
    Result analyze (const juce::AudioBuffer<float>& midDry,
                    const juce::AudioBuffer<float>& midWet,
                    const juce::AudioBuffer<float>& sideDry,
                    const juce::AudioBuffer<float>& sideWet,
                    double sr,
                    int latencySamples);

private:
    /** FFT order used by the H1 estimator (16384 points, as FreqResponse). */
    static constexpr int fftOrder = 14;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (StereoAnalysis)
};
