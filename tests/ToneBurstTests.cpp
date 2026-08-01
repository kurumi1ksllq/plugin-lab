#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "../source/signal/ToneBurst.h"

//==============================================================================
// Helper: generate all samples from a ToneBurst into a single buffer and
// return the absolute peak value across all channels.
static float generateAllAndFindPeak (ToneBurst& tb, double sampleRate, int blockSize)
{
    tb.prepare (sampleRate, blockSize);

    const auto totalLen = tb.getTotalLength();
    REQUIRE (totalLen > 0);

    juce::AudioBuffer<float> buffer (1, static_cast<int> (totalLen));
    buffer.clear();

    // generate() fills in chunks of blockSize
    int64_t pos = 0;
    while (pos < totalLen)
    {
        const int chunk = static_cast<int> (std::min<int64_t> (blockSize, totalLen - pos));
        tb.generate (buffer, static_cast<int> (pos), chunk);
        pos += chunk;
    }

    // Find peak
    float peak = 0.0f;
    const float* data = buffer.getReadPointer (0);
    for (int i = 0; i < buffer.getNumSamples(); ++i)
        peak = std::max (peak, std::abs (data[i]));

    return peak;
}

//==============================================================================
TEST_CASE ("ToneBurst setMasterAmplitude scales burst levels without destroying them",
           "[toneburst][master-amplitude]")
{
    ToneBurst tb;
    tb.setFrequency (1000.0);
    tb.setLevels ({ 0.2, 0.5 });
    tb.setMasterAmplitude (0.5);

    const float peak = generateAllAndFindPeak (tb, 48000.0, 512);

    // Peak should be max(0.2, 0.5) * masterAmplitude(0.5) = 0.25
    REQUIRE (peak == Catch::Approx (0.25f).margin (0.01f));

    // getTotalLength must reflect the original 2 bursts, not a collapsed single burst
    const int64_t totalLen = tb.getTotalLength();
    const int64_t expectedLen = static_cast<int64_t> (2 * (2400 + 7200)); // 2 bursts @ 48kHz
    REQUIRE (totalLen == expectedLen);
}

TEST_CASE ("ToneBurst default levels intact without master amplitude",
           "[toneburst][defaults]")
{
    ToneBurst tb;
    tb.setFrequency (1000.0);

    const float peak = generateAllAndFindPeak (tb, 48000.0, 512);

    // Default levels: {0.01, 0.02, 0.05, 0.1, 0.2, 0.3, 0.5, 0.7, 0.9}
    // Peak should be 0.9 (max default level), masterAmplitude defaults to 1.0
    REQUIRE (peak == Catch::Approx (0.9f).margin (0.01f));

    // 9 default bursts
    const int64_t totalLen = tb.getTotalLength();
    const int64_t expectedLen = static_cast<int64_t> (9 * (2400 + 7200));
    REQUIRE (totalLen == expectedLen);
}
