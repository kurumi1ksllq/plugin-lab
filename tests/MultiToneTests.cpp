#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include "../source/signal/MultiTone.h"
#include <cmath>

//==============================================================================
// Helper: generate the full multitone buffer into a mono buffer.
// (Buffer length comes from getTotalLength(), so no sample rate is needed.)
static juce::AudioBuffer<float> generateTone (MultiTone& mt)
{
    const int totalSamples = static_cast<int> (mt.getTotalLength());
    juce::AudioBuffer<float> buf (1, totalSamples);
    buf.clear();
    mt.generate (buf, 0, totalSamples);
    return buf;
}

//==============================================================================
// Helper: crest factor = peak / RMS (linear).
static double crestFactor (const juce::AudioBuffer<float>& buf)
{
    const float* d = buf.getReadPointer (0);
    const int n = buf.getNumSamples();
    double peak = 0.0, sumSq = 0.0;
    for (int i = 0; i < n; ++i)
    {
        peak = std::max (peak, std::abs ((double) d[i]));
        sumSq += (double) d[i] * d[i];
    }
    const double rms = std::sqrt (sumSq / n);
    return peak / std::max (rms, 1e-12);
}

//==============================================================================
TEST_CASE ("MultiTone: random phase seed lowers crest factor vs zero phase",
           "[multitone][phase]")
{
    // NOTE: powers-of-2 frequencies (100/200/.../12800, the generator default)
    // never phase-align, so their zero-phase crest factor is already low and
    // random phases would *raise* it — not a meaningful demonstration. A set
    // of mutually commensurate tones that CAN align shows the real drop:
    // zero-phase CF ≈ 4.0 vs random-phase CF ≈ 2.9 (plan E2 verified).
    MultiTone mt;
    mt.setFrequencies ({ 100.0, 500.0, 900.0, 1300.0, 1700.0, 2100.0, 2500.0, 2900.0 });
    mt.setDuration (1.0);
    mt.setAmplitude (0.3);

    mt.setRandomPhaseSeed (0);          // legacy all-zero phase
    mt.prepare (48000.0, 512);
    const auto zeroBuf = generateTone (mt);
    const double zeroCF = crestFactor (zeroBuf);

    mt.setRandomPhaseSeed (42);
    mt.prepare (48000.0, 512);
    const auto randBuf = generateTone (mt);
    const double randCF = crestFactor (randBuf);

    INFO ("zero-phase CF = " << zeroCF << ", random-phase CF = " << randCF);
    REQUIRE (randCF < zeroCF);      // crest factor dropped
    REQUIRE (randCF < 4.0);         // 8-tone random-phase CF ≈ 3.2-3.6
}

TEST_CASE ("MultiTone: same seed reproduces identical waveform", "[multitone][phase]")
{
    MultiTone mt;
    mt.setFrequencies ({ 100.0, 1000.0, 5000.0 });
    mt.setDuration (0.1);
    mt.setAmplitude (0.3);

    juce::AudioBuffer<float> a, b;
    mt.setRandomPhaseSeed (7); mt.prepare (48000.0, 512); a = generateTone (mt);
    mt.setRandomPhaseSeed (7); mt.prepare (48000.0, 512); b = generateTone (mt);

    REQUIRE (a.getNumSamples() == b.getNumSamples());
    for (int i = 0; i < a.getNumSamples(); ++i)
        REQUIRE (a.getSample (0, i) == b.getSample (0, i));   // bit-identical
}

TEST_CASE ("MultiTone: different seeds produce different waveforms", "[multitone][phase]")
{
    MultiTone mt;
    mt.setFrequencies ({ 100.0, 1000.0, 5000.0 });
    mt.setDuration (0.1);
    mt.setAmplitude (0.3);

    juce::AudioBuffer<float> a, b;
    mt.setRandomPhaseSeed (7);  mt.prepare (48000.0, 512); a = generateTone (mt);
    mt.setRandomPhaseSeed (99); mt.prepare (48000.0, 512); b = generateTone (mt);

    bool differs = false;
    for (int i = 0; i < a.getNumSamples(); ++i)
        if (a.getSample (0, i) != b.getSample (0, i)) { differs = true; break; }
    REQUIRE (differs);
}

TEST_CASE ("MultiTone: seed 0 keeps the legacy zero-phase waveform", "[multitone][phase]")
{
    // Without setRandomPhaseSeed (default = 0), sample values equal sin-sum.
    MultiTone mt;
    mt.setFrequencies ({ 100.0, 300.0, 500.0 });
    mt.setDuration (0.1);
    mt.setAmplitude (0.3);
    mt.prepare (48000.0, 512);
    const auto buf = generateTone (mt);

    // Spot-check against the closed form sin(2πft) with no phase offset:
    const double sr = 48000.0;
    for (int i = 0; i < 2000; i += 100)
    {
        double expected = 0.0;
        for (double f : { 100.0, 300.0, 500.0 })
            expected += std::sin (2.0 * juce::MathConstants<double>::pi * f * i / sr);
        expected *= 0.3 / 3.0;
        REQUIRE (buf.getSample (0, i) == Catch::Approx (expected).margin (1e-6));
    }
}
