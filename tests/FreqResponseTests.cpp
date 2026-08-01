#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "../source/analysis/FreqResponse.h"
#include "../source/signal/SineSweep.h"
#include "../source/utils/FftHelper.h"

#include <juce_dsp/juce_dsp.h>
#include <algorithm>
#include <cmath>
#include <limits>

//==============================================================================
// Helper: generate a full sine sweep buffer (mono, channel 0).
static juce::AudioBuffer<float> generateSweep (double sr,
                                               double startHz,
                                               double endHz,
                                               double durationSec,
                                               double amplitude)
{
    SineSweep sw;
    sw.setFrequencyRange (startHz, endHz);
    sw.setDuration (durationSec);
    sw.setAmplitude (amplitude);
    sw.prepare (sr, 512);

    const int totalSamples = static_cast<int> (sr * durationSec);
    juce::AudioBuffer<float> buf (1, totalSamples);
    buf.clear();
    sw.generate (buf, 0, totalSamples);
    return buf;
}

//==============================================================================
// Helper: apply a cascade of IIR filters to a mono buffer in-place.
static void applyFilters (juce::AudioBuffer<float>& buf,
                          const std::vector<juce::dsp::IIR::Coefficients<float>::Ptr>& coeffs)
{
    const int numSamples = buf.getNumSamples();
    float* data = buf.getWritePointer (0);

    juce::dsp::IIR::Filter<float> filter;
    for (auto& coeff : coeffs)
    {
        filter.coefficients = coeff;
        filter.reset();

        for (int i = 0; i < numSamples; ++i)
            data[i] = filter.processSample (data[i]);
    }
}

//==============================================================================
// Helper: create a delayed copy of a mono buffer.
static juce::AudioBuffer<float> delayCopy (const juce::AudioBuffer<float>& src,
                                           int delaySamples)
{
    const int totalSamples = src.getNumSamples();
    juce::AudioBuffer<float> dst (1, totalSamples);
    dst.clear();
    const float* srcData = src.getReadPointer (0);
    float* dstData = dst.getWritePointer (0);

    for (int i = delaySamples; i < totalSamples; ++i)
        dstData[i] = srcData[i - delaySamples];

    return dst;
}

//==============================================================================
// Helper: find the point in a curve closest to a target frequency.
static FreqResponse::Point findClosest (const std::vector<FreqResponse::Point>& curve,
                                         double targetFreq)
{
    REQUIRE (!curve.empty());
    FreqResponse::Point best = curve[0];
    double bestDist = std::abs (curve[0].frequency - targetFreq);

    for (const auto& p : curve)
    {
        double dist = std::abs (p.frequency - targetFreq);
        if (dist < bestDist)
        {
            bestDist = dist;
            best = p;
        }
    }
    return best;
}

//==============================================================================
// Helper: get all points in a frequency range [lowHz, highHz].
static std::vector<FreqResponse::Point> pointsInRange (
    const std::vector<FreqResponse::Point>& curve,
    double lowHz,
    double highHz)
{
    std::vector<FreqResponse::Point> result;
    for (const auto& p : curve)
        if (p.frequency >= lowHz && p.frequency <= highHz)
            result.push_back (p);
    return result;
}

//==============================================================================
// Helper: compute mean magnitude deviation from 0 dB.
static double meanAbsMagDB (const std::vector<FreqResponse::Point>& points)
{
    if (points.empty()) return 1e9;
    double sum = 0.0;
    for (const auto& p : points)
        sum += std::abs (p.magnitudeDB);
    return sum / static_cast<double> (points.size());
}

//==============================================================================
// Helper: compute mean absolute phase deviation from 0°.
static double meanAbsPhaseDeg (const std::vector<FreqResponse::Point>& points)
{
    if (points.empty()) return 1e9;
    double sum = 0.0;
    for (const auto& p : points)
        sum += std::abs (p.phaseDeg);
    return sum / static_cast<double> (points.size());
}

//==============================================================================
// Test 1: Identity — dry == wet gives flat 0 dB / 0° response.
//==============================================================================

TEST_CASE ("FreqResponse identity: dry==wet gives 0dB/0°", "[freqresponse][h1]")
{
    const double sr = 48000.0;
    const double duration = 5.0;
    const double amplitude = 0.5;

    // Generate one sweep and use it for both dry and wet
    auto dry = generateSweep (sr, 20.0, 20000.0, duration, amplitude);
    juce::AudioBuffer<float> wet (1, dry.getNumSamples());
    wet.clear();
    const float* dryData = dry.getReadPointer (0);
    float* wetData = wet.getWritePointer (0);
    for (int i = 0; i < dry.getNumSamples(); ++i)
        wetData[i] = dryData[i];

    FreqResponse fr;
    auto result = fr.analyze (dry, wet, sr);

    // Raw points must be populated
    REQUIRE (!result.raw.empty());
    REQUIRE (result.sampleRate == Catch::Approx (sr));

    // Check 100 Hz – 10 kHz range
    auto mid = pointsInRange (result.raw, 100.0, 10000.0);
    REQUIRE (mid.size() > 50); // enough data points

    double avgMagErr = meanAbsMagDB (mid);
    double avgPhaseErr = meanAbsPhaseDeg (mid);

    INFO ("Mean |mag| = " << avgMagErr << " dB");
    INFO ("Mean |phase| = " << avgPhaseErr << " deg");
    REQUIRE (avgMagErr < 0.5);
    REQUIRE (avgPhaseErr < 3.0);

    // Smoothed curves must be populated
    REQUIRE (!result.smoothed_1_12.empty());
    REQUIRE (!result.smoothed_1_3.empty());
}

//==============================================================================
// Test 2: Known filter — +6 dB bell @ 1 kHz Q=1 + 12 dB/oct lowpass @ 8 kHz.
//==============================================================================

TEST_CASE ("FreqResponse known bell filter: +6dB@1kHz Q1 recovered", "[freqresponse][h1]")
{
    const double sr = 48000.0;
    const double duration = 5.0;
    const double amplitude = 0.5;

    // Generate dry sweep
    auto dryBuf = generateSweep (sr, 20.0, 20000.0, duration, amplitude);

    // Copy to wet
    juce::AudioBuffer<float> wetBuf (1, dryBuf.getNumSamples());
    wetBuf.clear();
    const float* dryData = dryBuf.getReadPointer (0);
    float* wetData = wetBuf.getWritePointer (0);
    for (int i = 0; i < dryBuf.getNumSamples(); ++i)
        wetData[i] = dryData[i];

    // Build filter cascade:
    // 1. Bell: +6 dB, 1 kHz, Q=1
    // 2. 2nd-order Butterworth lowpass: 12 dB/oct, cutoff 8 kHz
    float gainLinear = juce::Decibels::decibelsToGain (6.0f);

    std::vector<juce::dsp::IIR::Coefficients<float>::Ptr> coeffs;
    coeffs.push_back (juce::dsp::IIR::Coefficients<float>::makePeakFilter (
        sr, 1000.0, 1.0f, gainLinear));
    coeffs.push_back (juce::dsp::IIR::Coefficients<float>::makeLowPass (
        sr, 8000.0, 1.0f / std::sqrt (2.0f)));

    applyFilters (wetBuf, coeffs);

    FreqResponse fr;
    auto result = fr.analyze (dryBuf, wetBuf, sr);

    REQUIRE (!result.raw.empty());

    // Find peak near 1 kHz
    auto vicinity = pointsInRange (result.raw, 900.0, 1100.0);
    REQUIRE (!vicinity.empty());

    FreqResponse::Point peakP = vicinity[0];
    for (const auto& p : vicinity)
        if (p.magnitudeDB > peakP.magnitudeDB)
            peakP = p;

    INFO ("Peak at " << peakP.frequency << " Hz = " << peakP.magnitudeDB << " dB");
    REQUIRE (peakP.magnitudeDB == Catch::Approx (6.0).margin (0.5));
    REQUIRE (peakP.frequency >= 900.0);
    REQUIRE (peakP.frequency <= 1100.0);

    // Verify rolloff above 8k: mag at 10k < mag at 4k by at least 2 dB
    auto p4k = findClosest (result.raw, 4000.0);
    auto p10k = findClosest (result.raw, 10000.0);

    INFO ("mag @ 4k  = " << p4k.magnitudeDB << " dB");
    INFO ("mag @ 10k = " << p10k.magnitudeDB << " dB");
    REQUIRE (p10k.magnitudeDB < p4k.magnitudeDB - 2.0);

    // Phase: just verify it's finite
    for (const auto& p : result.raw)
    {
        REQUIRE (std::isfinite (p.magnitudeDB));
        REQUIRE (std::isfinite (p.phaseDeg));
    }
}

//==============================================================================
// Test 3: Latency compensation — flattens the linear phase ramp.
//==============================================================================

TEST_CASE ("FreqResponse latency compensation flattens phase ramp", "[freqresponse][h1]")
{
    const double sr = 48000.0;
    const double duration = 5.0;
    const int delaySamples = 256;

    auto dryBuf = generateSweep (sr, 20.0, 20000.0, duration, 0.5);
    auto wetBuf = delayCopy (dryBuf, delaySamples);

    // Without compensation: phase should be significantly non-zero
    {
        FreqResponse fr; // default latencySamples = 0
        auto result = fr.analyze (dryBuf, wetBuf, sr);

        auto mid = pointsInRange (result.raw, 1000.0, 10000.0);
        REQUIRE (mid.size() > 30);

        double avgPhaseErr = meanAbsPhaseDeg (mid);
        INFO ("Without compensation: mean |phase| = " << avgPhaseErr << " deg");
        // A delay of 256 samples at 48 kHz should produce measurable phase deviation
        REQUIRE (avgPhaseErr > 15.0);
    }

    // With compensation: phase should be near 0
    {
        FreqResponse fr;
        fr.setLatencySamples (delaySamples);
        auto result = fr.analyze (dryBuf, wetBuf, sr);

        auto mid = pointsInRange (result.raw, 1000.0, 10000.0);
        REQUIRE (mid.size() > 30);

        double avgPhaseErr = meanAbsPhaseDeg (mid);
        INFO ("With compensation: mean |phase| = " << avgPhaseErr << " deg");
        // After compensation, the linear phase ramp should be removed
        REQUIRE (avgPhaseErr < 10.0);
    }

    // Magnitude should be ~0 dB regardless of delay/compensation
    {
        FreqResponse fr;
        fr.setLatencySamples (delaySamples);
        auto result = fr.analyze (dryBuf, wetBuf, sr);

        auto mid = pointsInRange (result.raw, 100.0, 10000.0);
        double avgMagErr = meanAbsMagDB (mid);
        INFO ("With compensation: mean |mag| = " << avgMagErr << " dB");
        REQUIRE (avgMagErr < 0.5);
    }
}

//==============================================================================
// Test 4: Curve sanity — smoothed curves populated, frequency monotonic.
//==============================================================================

TEST_CASE ("FreqResponse smoothed curves populated and frequencies monotonic", "[freqresponse][h1]")
{
    const double sr = 48000.0;

    auto dry = generateSweep (sr, 20.0, 20000.0, 5.0, 0.5);
    juce::AudioBuffer<float> wet (1, dry.getNumSamples());
    wet.clear();
    const float* dryData = dry.getReadPointer (0);
    float* wetData = wet.getWritePointer (0);
    for (int i = 0; i < dry.getNumSamples(); ++i)
        wetData[i] = dryData[i];

    FreqResponse fr;
    auto result = fr.analyze (dry, wet, sr);

    // Smoothed curves must be populated
    REQUIRE (!result.smoothed_1_12.empty());
    REQUIRE (!result.smoothed_1_3.empty());

    // Smooth curve sizes must be <= raw size
    REQUIRE (result.smoothed_1_12.size() <= result.raw.size());
    REQUIRE (result.smoothed_1_3.size() <= result.raw.size());

    // Raw curve frequencies must be monotonically increasing
    for (size_t i = 1; i < result.raw.size(); ++i)
        REQUIRE (result.raw[i].frequency > result.raw[i - 1].frequency);

    // Smoothed curves must have matching frequency bins (same set)
    for (size_t i = 0; i < result.smoothed_1_12.size(); ++i)
        REQUIRE (result.smoothed_1_12[i].frequency == Catch::Approx (result.raw[i].frequency).margin (0.5));
    for (size_t i = 0; i < result.smoothed_1_3.size(); ++i)
        REQUIRE (result.smoothed_1_3[i].frequency == Catch::Approx (result.raw[i].frequency).margin (0.5));

    // All phase values must be finite
    for (const auto& p : result.raw)
        REQUIRE (std::isfinite (p.phaseDeg));
    for (const auto& p : result.smoothed_1_12)
        REQUIRE (std::isfinite (p.phaseDeg));
    for (const auto& p : result.smoothed_1_3)
        REQUIRE (std::isfinite (p.phaseDeg));
}
