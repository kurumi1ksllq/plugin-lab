// tests/SequentialToneTests.cpp — single-tone-per-segment THD excitation
// (issue #38: THD must use single-tone excitation per DESIGN.md §3.2).
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include "../source/signal/SequentialTone.h"
#include <cmath>
#include <vector>

namespace
{
//==============================================================================
// Helper: generate the full buffer (length comes from getTotalLength()).
static juce::AudioBuffer<float> generateFull (SequentialTone& st)
{
    const int totalSamples = static_cast<int> (st.getTotalLength());
    juce::AudioBuffer<float> buf (1, totalSamples);
    buf.clear();
    st.generate (buf, 0, totalSamples);
    return buf;
}

//==============================================================================
// Helper: dominant frequency of a windowed region via rising zero crossings.
// For a clean sine starting at phase 0 this equals the sine's frequency.
static double dominantFreq (const juce::AudioBuffer<float>& buf,
                            int start, int len, double sr)
{
    int rising = 0;
    for (int i = 1; i < len; ++i)
    {
        const float prev = buf.getSample (0, start + i - 1);
        const float cur  = buf.getSample (0, start + i);
        if (prev <= 0.0f && cur > 0.0f)
            ++rising;
    }
    return static_cast<double> (rising) * sr / static_cast<double> (len);
}

static double segmentPeak (const juce::AudioBuffer<float>& buf, int start, int len)
{
    double peak = 0.0;
    for (int i = 0; i < len; ++i)
        peak = std::max (peak, std::abs ((double) buf.getSample (0, start + i)));
    return peak;
}
} // namespace

//==============================================================================
TEST_CASE ("SequentialTone: total length is N x (segment + gap) x sample rate",
           "[sequentialtone]")
{
    SequentialTone st;
    st.setFrequencies ({ 100.0, 200.0, 400.0 });
    st.setSegmentDuration (0.5);
    st.setAmplitude (0.4);
    st.prepare (48000.0, 512);
    REQUIRE (st.getTotalLength() == static_cast<int64_t> (3 * 0.5 * 48000.0));

    st.setGapSeconds (0.25);
    st.prepare (48000.0, 512);
    REQUIRE (st.getTotalLength() == static_cast<int64_t> (3 * 0.75 * 48000.0));
}

TEST_CASE ("SequentialTone: each segment contains only its own frequency",
           "[sequentialtone]")
{
    constexpr double kSr = 48000.0;
    SequentialTone st;
    st.setFrequencies ({ 100.0, 200.0, 400.0 });
    st.setSegmentDuration (0.5);
    st.setAmplitude (0.4);
    st.prepare (kSr, 512);
    const auto buf = generateFull (st);

    const int segLen = static_cast<int> (0.5 * kSr);
    const std::vector<double> expected = { 100.0, 200.0, 400.0 };
    for (size_t i = 0; i < expected.size(); ++i)
    {
        const double f = dominantFreq (buf, static_cast<int> (i * segLen), segLen, kSr);
        INFO ("segment " << i << " dominant freq = " << f);
        REQUIRE (std::abs (f - expected[i]) < expected[i] * 0.02);
        // The segment carries the configured amplitude — a sine whose peak
        // never quite hits the exact amplitude (sampling) is fine.
        const double peak = segmentPeak (buf, static_cast<int> (i * segLen), segLen);
        REQUIRE (peak > 0.4 * 0.99);
        REQUIRE (peak < 0.4 * 1.01);
    }
}

TEST_CASE ("SequentialTone: amplitude scales the output", "[sequentialtone]")
{
    constexpr double kSr = 48000.0;

    SequentialTone a;
    a.setFrequencies ({ 100.0 });
    a.setSegmentDuration (0.1);
    a.setAmplitude (0.25);
    a.prepare (kSr, 512);
    const auto bufA = generateFull (a);

    SequentialTone b;
    b.setFrequencies ({ 100.0 });
    b.setSegmentDuration (0.1);
    b.setAmplitude (0.5);
    b.prepare (kSr, 512);
    const auto bufB = generateFull (b);

    const double peakA = segmentPeak (bufA, 0, bufA.getNumSamples());
    const double peakB = segmentPeak (bufB, 0, bufB.getNumSamples());
    REQUIRE (peakA == Catch::Approx (0.25).margin (0.01));
    REQUIRE (peakB == Catch::Approx (0.5).margin (0.01));
    REQUIRE (peakB / peakA == Catch::Approx (2.0).margin (1e-3));
}

TEST_CASE ("SequentialTone: gap samples are silent", "[sequentialtone]")
{
    constexpr double kSr = 48000.0;
    SequentialTone st;
    st.setFrequencies ({ 100.0 });
    st.setSegmentDuration (0.1);
    st.setGapSeconds (0.05);
    st.setAmplitude (0.4);
    st.prepare (kSr, 512);
    const auto buf = generateFull (st);

    const int toneLen = static_cast<int> (0.1 * kSr);
    for (int i = toneLen; i < static_cast<int> (0.15 * kSr); ++i)
        REQUIRE (buf.getSample (0, i) == 0.0f);
    // The tone itself is NOT silent (sample 0 is exactly 0 — phase starts at
    // 0 — so probe a mid-tone sample instead).
    REQUIRE (buf.getSample (0, 100) != 0.0f);
}

TEST_CASE ("SequentialTone: reset restarts from the beginning", "[sequentialtone]")
{
    SequentialTone st;
    st.setFrequencies ({ 100.0, 200.0 });
    st.setSegmentDuration (0.1);
    st.setAmplitude (0.4);
    st.prepare (48000.0, 512);

    const auto full = generateFull (st);
    REQUIRE (full.getNumSamples() > 500);

    juce::AudioBuffer<float> head (1, 500);
    head.clear();
    st.reset();
    st.generate (head, 0, 500);

    for (int i = 0; i < 500; ++i)
        REQUIRE (head.getSample (0, i) == full.getSample (0, i));   // bit-identical
}

TEST_CASE ("SequentialTone: two runs produce identical output", "[sequentialtone]")
{
    SequentialTone a;
    a.setFrequencies ({ 100.0, 200.0, 400.0 });
    a.setSegmentDuration (0.1);
    a.setAmplitude (0.4);
    a.prepare (48000.0, 512);
    const auto bufA = generateFull (a);

    SequentialTone b;
    b.setFrequencies ({ 100.0, 200.0, 400.0 });
    b.setSegmentDuration (0.1);
    b.setAmplitude (0.4);
    b.prepare (48000.0, 512);
    const auto bufB = generateFull (b);

    REQUIRE (bufA.getNumSamples() == bufB.getNumSamples());
    for (int i = 0; i < bufA.getNumSamples(); ++i)
        REQUIRE (bufA.getSample (0, i) == bufB.getSample (0, i));   // bit-identical
}
