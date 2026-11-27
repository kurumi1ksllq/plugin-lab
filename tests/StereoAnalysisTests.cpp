/**
 * StereoAnalysis tests (issue #104 / ADR-0003, DESIGN.md §3.4).
 *
 * The analyser is exercised through its public API only: synthetic L/R pairs
 * go in, the four MS curves come out. Every fixture is a closed-form signal so
 * the expected decode is exact:
 *
 *   - mid-only input  (L = R = s)  -> M = s, S = 0
 *   - side-only input (L = -R = s) -> M = 0, S = s
 *   - L = R passthrough            -> rho = 1, width_db = 0 dB reference
 *
 * The four curves are only compared in bands where the excitation actually has
 * energy (a swept sine is not full-band at any instant), which is why the
 * helpers below read a narrow window around a probe frequency.
 */
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "../source/analysis/StereoAnalysis.h"

#include <cmath>
#include <vector>

namespace
{
constexpr double kSampleRate = 48000.0;

/** Fill a stereo buffer with the MidSide excitation pair:
 *    mid  -> L = R = s
 *    side -> L = s, R = -s
 *  s is a steady sine at freqHz (a steady tone keeps the H1 estimate exact at
 *  its own bin, which is all these tests assert). */
void fillExcitation (juce::AudioBuffer<float>& buffer, bool side, double freqHz,
                     int numSamples, double amplitude = 0.5)
{
    buffer.setSize (2, numSamples, false, true, false);

    for (int i = 0; i < numSamples; ++i)
    {
        const double s = amplitude
                         * std::sin (2.0 * juce::MathConstants<double>::pi
                                     * freqHz * static_cast<double> (i) / kSampleRate);
        buffer.setSample (0, i, static_cast<float> (s));
        buffer.setSample (1, i, static_cast<float> (side ? -s : s));
    }
}

/** Run a passthrough-style plugin behaviour on the wet copy: apply the given
 *  per-sample L/R transform. */
template <typename Fn>
void transform (juce::AudioBuffer<float>& wet, Fn&& fn)
{
    for (int i = 0; i < wet.getNumSamples(); ++i)
    {
        const double l = wet.getSample (0, i);
        const double r = wet.getSample (1, i);
        double outL = l, outR = r;
        fn (l, r, outL, outR);
        wet.setSample (0, i, static_cast<float> (outL));
        wet.setSample (1, i, static_cast<float> (outR));
    }
}

/** Value of a response curve at the analysed bin closest to freqHz. Returns
 *  false when no point is close enough (excitation had no energy there). */
template <typename PointVec, typename Get>
bool valueNear (const PointVec& points, double freqHz, Get get, double& out)
{
    double bestDist = 1.0e9;

    for (const auto& p : points)
    {
        const double dist = std::abs (p.frequency - freqHz);
        if (dist < bestDist)
        {
            bestDist = dist;
            out = get (p);
        }
    }

    // One FFT bin at 48 kHz / 16384 is ~2.93 Hz; allow a few bins of slack.
    return bestDist <= 10.0;
}
}  // namespace
//==============================================================================
// MS decode: mid-only excitation -> M response = plugin mid path,
// S response = plugin mid->side leakage (0 for a passthrough plugin).

TEST_CASE ("StereoAnalysis: passthrough decodes both MS responses at 0 dB",
           "[stereo][ms-decode]")
{
    constexpr int kNumSamples = 16384 * 2;
    constexpr double kProbeHz = 1000.0;

    juce::AudioBuffer<float> midDry, midWet, sideDry, sideWet;
    fillExcitation (midDry, false, kProbeHz, kNumSamples);
    midWet.makeCopyOf (midDry);          // passthrough
    fillExcitation (sideDry, true, kProbeHz, kNumSamples);
    sideWet.makeCopyOf (sideDry);        // passthrough

    StereoAnalysis analyser;
    const auto result = analyser.analyze (midDry, midWet, sideDry, sideWet, kSampleRate, 0);

    REQUIRE (result.valid);
    REQUIRE_FALSE (result.midResponse.empty());

    // M = (L + R)/2 = the excitation itself -> 0 dB mid response.
    double midDB = 0.0;
    REQUIRE (valueNear (result.midResponse, kProbeHz,
                        [] (const StereoAnalysis::Point& p) { return p.magnitudeDB; }, midDB));
    REQUIRE (midDB == Catch::Approx (0.0).margin (0.1));

    // H_S comes from Run B (side-only excitation -> decoded S): a passthrough
    // plugin leaves it at 0 dB, so the MS pair is balanced and width_db = 0.
    double sideDB = 0.0;
    REQUIRE (valueNear (result.sideResponse, kProbeHz,
                        [] (const StereoAnalysis::Point& p) { return p.magnitudeDB; }, sideDB));
    REQUIRE (sideDB == Catch::Approx (0.0).margin (0.1));

    double widthDB = 0.0;
    REQUIRE (valueNear (result.widthDB, kProbeHz,
                        [] (const StereoAnalysis::WidthPoint& p) { return p.widthDB; }, widthDB));
    REQUIRE (widthDB == Catch::Approx (0.0).margin (0.1));
}

//==============================================================================
// Correlation: L = R output -> rho = 1 at every analysed bin (mono input is
// perfectly correlated).

TEST_CASE ("StereoAnalysis: L = R output has correlation 1", "[stereo][correlation]")
{
    constexpr int kNumSamples = 16384 * 2;
    constexpr double kProbeHz = 1000.0;

    juce::AudioBuffer<float> midDry, midWet, sideDry, sideWet;
    fillExcitation (midDry, false, kProbeHz, kNumSamples);
    midWet.makeCopyOf (midDry);
    fillExcitation (sideDry, true, kProbeHz, kNumSamples);
    sideWet.makeCopyOf (sideDry);

    StereoAnalysis analyser;
    const auto result = analyser.analyze (midDry, midWet, sideDry, sideWet, kSampleRate, 0);

    REQUIRE (result.valid);
    REQUIRE_FALSE (result.correlation.empty());

    double rho = 0.0;
    REQUIRE (valueNear (result.correlation, kProbeHz,
                        [] (const StereoAnalysis::CorrelationPoint& p) { return p.rho; }, rho));
    REQUIRE (rho == Catch::Approx (1.0).margin (1.0e-3));
}

//==============================================================================
// Width: attenuating the side path (narrower) raises width_db by exactly the
// applied side gain in dB.

TEST_CASE ("StereoAnalysis: side attenuation raises width_db by the side gain",
           "[stereo][width]")
{
    constexpr int kNumSamples = 16384 * 2;
    constexpr double kProbeHz = 1000.0;
    constexpr double kSideGain = 0.5;    // -6.02 dB of side

    juce::AudioBuffer<float> midDry, midWet, sideDry, sideWet;
    fillExcitation (midDry, false, kProbeHz, kNumSamples);
    midWet.makeCopyOf (midDry);          // mid path untouched
    fillExcitation (sideDry, true, kProbeHz, kNumSamples);
    sideWet.makeCopyOf (sideDry);

    // sideAttenuation behaviour: S *= kSideGain, re-encoded as L = M + S,
    // R = M - S (the same transform TestStereoPlugin applies).
    transform (sideWet, [kSideGain] (double l, double r, double& outL, double& outR)
    {
        const double mid  = 0.5 * (l + r);
        const double side = 0.5 * (l - r) * kSideGain;
        outL = mid + side;
        outR = mid - side;
    });

    StereoAnalysis analyser;
    const auto result = analyser.analyze (midDry, midWet, sideDry, sideWet, kSampleRate, 0);

    REQUIRE (result.valid);

    double widthDB = 0.0;
    REQUIRE (valueNear (result.widthDB, kProbeHz,
                        [] (const StereoAnalysis::WidthPoint& p) { return p.widthDB; }, widthDB));

    // width_db = 20*log10(|M|/|S|) = 0 - (-6.02) = +6.02 dB.
    REQUIRE (widthDB == Catch::Approx (-20.0 * std::log10 (kSideGain)).margin (0.2));
}

//==============================================================================
// Width reference: an untouched side path gives width_db = 0 dB.

TEST_CASE ("StereoAnalysis: untouched side path has width_db 0", "[stereo][width]")
{
    constexpr int kNumSamples = 16384 * 2;
    constexpr double kProbeHz = 1000.0;

    juce::AudioBuffer<float> midDry, midWet, sideDry, sideWet;
    fillExcitation (midDry, false, kProbeHz, kNumSamples);
    midWet.makeCopyOf (midDry);
    fillExcitation (sideDry, true, kProbeHz, kNumSamples);
    sideWet.makeCopyOf (sideDry);

    StereoAnalysis analyser;
    const auto result = analyser.analyze (midDry, midWet, sideDry, sideWet, kSampleRate, 0);

    REQUIRE (result.valid);

    double widthDB = 0.0;
    REQUIRE (valueNear (result.widthDB, kProbeHz,
                        [] (const StereoAnalysis::WidthPoint& p) { return p.widthDB; }, widthDB));
    REQUIRE (widthDB == Catch::Approx (0.0).margin (0.1));
}

//==============================================================================
// Widening: boosting the side path (sideGain > 1) lowers width_db.

TEST_CASE ("StereoAnalysis: side boost lowers width_db", "[stereo][width]")
{
    constexpr int kNumSamples = 16384 * 2;
    constexpr double kProbeHz = 1000.0;
    constexpr double kSideGain = 2.0;    // +6.02 dB of side

    juce::AudioBuffer<float> midDry, midWet, sideDry, sideWet;
    fillExcitation (midDry, false, kProbeHz, kNumSamples);
    midWet.makeCopyOf (midDry);
    fillExcitation (sideDry, true, kProbeHz, kNumSamples);
    sideWet.makeCopyOf (sideDry);

    transform (sideWet, [kSideGain] (double l, double r, double& outL, double& outR)
    {
        const double mid  = 0.5 * (l + r);
        const double side = 0.5 * (l - r) * kSideGain;
        outL = mid + side;
        outR = mid - side;
    });

    StereoAnalysis analyser;
    const auto result = analyser.analyze (midDry, midWet, sideDry, sideWet, kSampleRate, 0);

    REQUIRE (result.valid);

    double widthDB = 0.0;
    REQUIRE (valueNear (result.widthDB, kProbeHz,
                        [] (const StereoAnalysis::WidthPoint& p) { return p.widthDB; }, widthDB));
    REQUIRE (widthDB == Catch::Approx (-20.0 * std::log10 (kSideGain)).margin (0.2));
}

//==============================================================================
// Edge case: a mono capture is an explicit "not measurable" — no curves, and
// valid == false so the caller reports an error instead of exporting mono data.

TEST_CASE ("StereoAnalysis: mono buffers leave the result invalid", "[stereo][edge]")
{
    constexpr int kNumSamples = 16384 * 2;

    juce::AudioBuffer<float> monoDry (1, kNumSamples);
    juce::AudioBuffer<float> monoWet (1, kNumSamples);
    monoDry.clear();
    monoWet.clear();

    StereoAnalysis analyser;
    const auto result = analyser.analyze (monoDry, monoWet, monoDry, monoWet, kSampleRate, 0);

    REQUIRE_FALSE (result.valid);
    REQUIRE (result.midResponse.empty());
    REQUIRE (result.sideResponse.empty());
    REQUIRE (result.correlation.empty());
    REQUIRE (result.widthDB.empty());
}

//==============================================================================
// Edge case: a recording shorter than one FFT window is not analysable.

TEST_CASE ("StereoAnalysis: too-short recording leaves the result invalid", "[stereo][edge]")
{
    constexpr int kShortSamples = 512;

    juce::AudioBuffer<float> dry, wet;
    fillExcitation (dry, false, 1000.0, kShortSamples);
    wet.makeCopyOf (dry);

    StereoAnalysis analyser;
    const auto result = analyser.analyze (dry, wet, dry, wet, kSampleRate, 0);

    REQUIRE_FALSE (result.valid);
    REQUIRE (result.widthDB.empty());
}

//==============================================================================
// Polarity flip: inverting R (R = -R) swaps M and S (M = (L - R)/2, S = (L +
// R)/2), so the mid response turns into the side response. rho is
// magnitude-squared coherence, so the anti-correlated output pair stays at
// rho = 1 - the flip shows up as M/S swap, not as a correlation drop.

TEST_CASE ("StereoAnalysis: right-channel polarity flip swaps the MS responses",
           "[stereo][correlation]")
{
    constexpr int kNumSamples = 16384 * 2;
    constexpr double kProbeHz = 1000.0;

    juce::AudioBuffer<float> midDry, midWet, sideDry, sideWet;
    fillExcitation (midDry, false, kProbeHz, kNumSamples);
    midWet.makeCopyOf (midDry);
    fillExcitation (sideDry, true, kProbeHz, kNumSamples);
    sideWet.makeCopyOf (sideDry);

    // polarityFlip behaviour on the mid-only wet pair: R = -R.
    transform (midWet, [] (double l, double r, double& outL, double& outR)
    {
        outL = l;
        outR = -r;
    });

    StereoAnalysis analyser;
    const auto result = analyser.analyze (midDry, midWet, sideDry, sideWet, kSampleRate, 0);

    REQUIRE (result.valid);

    // L = s, R = -s: perfectly anti-correlated, so |rho| = 1 (the
    // magnitude-squared coherence cannot distinguish the sign).
    double rho = 0.0;
    REQUIRE (valueNear (result.correlation, kProbeHz,
                        [] (const StereoAnalysis::CorrelationPoint& p) { return p.rho; }, rho));
    REQUIRE (rho == Catch::Approx (1.0).margin (1.0e-3));

    // The flip moves the mid-only excitation into the side response: the
    // decoded M is now silent while S carries the signal, so the flipped
    // measurement reports the original M level on the side curve.
    double sideDB = 0.0;
    REQUIRE (valueNear (result.sideResponse, kProbeHz,
                        [] (const StereoAnalysis::Point& p) { return p.magnitudeDB; }, sideDB));
    REQUIRE (sideDB == Catch::Approx (0.0).margin (0.1));
}