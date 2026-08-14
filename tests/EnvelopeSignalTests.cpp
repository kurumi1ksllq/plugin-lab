#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "../source/signal/EnvelopeSignal.h"
#include "../source/signal/SineSweep.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>

// CrashLog recording helpers (stubbed in CommandParserStubs.cpp)
extern void clearCrashLog();
extern bool crashLogWarnContains (const juce::String& substr);

//==============================================================================
// Test-local carrier: fills every sample with a constant 1.0 so the envelope
// value equals the generated output exactly. Has a finite nominal length so
// EnvelopeSignal can derive the ADSR release timing from it.
class ConstantCarrier final : public SignalGenerator
{
public:
    explicit ConstantCarrier (int64_t lengthSamples)
        : length (lengthSamples)
    {
    }

    void generate (juce::AudioBuffer<float>& buffer,
                   int startSample,
                   int numSamples) override
    {
        for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
            std::fill (buffer.getWritePointer (ch) + startSample,
                       buffer.getWritePointer (ch) + startSample + numSamples,
                       1.0f);
    }

    int64_t getTotalLength() const override { return length; }
    void reset() override { currentSample = 0.0; }

private:
    int64_t length;
};

//==============================================================================
// Test-local carrier: reports an indefinite length (-1) so EnvelopeSignal
// takes the CRASH_LOG_WARN fallback path in getTotalLength (issue #44).
class IndefiniteCarrier final : public SignalGenerator
{
public:
    void generate (juce::AudioBuffer<float>& buffer,
                   int startSample,
                   int numSamples) override
    {
        for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
            std::fill (buffer.getWritePointer (ch) + startSample,
                       buffer.getWritePointer (ch) + startSample + numSamples,
                       1.0f);
    }

    int64_t getTotalLength() const override { return -1; }
    void reset() override { currentSample = 0.0; }
};

//==============================================================================
TEST_CASE ("EnvelopeSignal indefinite carrier returns -1 and logs warning",
           "[envelope][indefinite-carrier]")
{
    // Arrange
    clearCrashLog();
    EnvelopeSignal env (std::make_unique<IndefiniteCarrier>());

    // Act
    const auto len = env.getTotalLength();

    // Assert: -1 sentinel propagates and the indefinite carrier leaves a
    // warning trail (issue #44) so SweepRunner's 10s fallback is never silent.
    REQUIRE (len == -1);
    REQUIRE (crashLogWarnContains ("EnvelopeSignal indefinite carrier"));
}

//==============================================================================
TEST_CASE ("EnvelopeSignal without envelope passes carrier through unchanged",
           "[envelope][identity]")
{
    constexpr double sr = 48000.0;
    const int64_t carrierLen = 48000;

    EnvelopeSignal env (std::make_unique<ConstantCarrier> (carrierLen));
    env.prepare (sr, 512);

    REQUIRE (env.getTotalLength() == carrierLen);

    juce::AudioBuffer<float> buffer (1, static_cast<int> (carrierLen));
    buffer.clear();
    env.generate (buffer, 0, static_cast<int> (carrierLen));

    for (int i = 0; i < buffer.getNumSamples(); ++i)
        REQUIRE (buffer.getSample (0, i) == 1.0f);
}

//==============================================================================
TEST_CASE ("EnvelopeSignal ADSR follows attack/decay/sustain/release shape",
           "[envelope][adsr]")
{
    constexpr double sr = 48000.0;
    constexpr double attack = 0.1;   // s
    constexpr double decay = 0.2;    // s
    constexpr double sustain = 0.5;
    constexpr double release = 0.1;  // s

    // 0.8 s total: 0.1 attack + 0.2 decay + 0.4 sustain flat + 0.1 release
    const double envLen = attack + decay + release + 0.4;
    const int64_t carrierLen = static_cast<int64_t> (envLen * sr);

    EnvelopeSignal env (std::make_unique<ConstantCarrier> (carrierLen));
    env.setEnvelope (EnvelopeSignal::Envelope::adsr);
    env.setADSR (attack, decay, sustain, release);
    env.prepare (sr, 512);

    // Generate past the nominal end so the tail after release is observable.
    const int numSamples = static_cast<int> (carrierLen + static_cast<int64_t> (sr * release));
    juce::AudioBuffer<float> buffer (1, numSamples);
    buffer.clear();
    env.generate (buffer, 0, numSamples);

    const auto sampleAt = [&] (double sec) { return static_cast<int> (std::llround (sec * sr)); };
    const auto out = [&] (int i) { return buffer.getSample (0, i); };

    // t=0: attack start
    REQUIRE (out (sampleAt (0.0)) == 0.0f);
    // mid-attack: linear ramp 0 -> 1 (0.05 / 0.1 = 0.5)
    REQUIRE (out (sampleAt (0.05)) == Catch::Approx (0.5f).margin (1e-3f));
    // mid-decay: linear ramp 1 -> sustain (1 - 0.5 * (0.2-0.1)/0.2 = 0.75)
    REQUIRE (out (sampleAt (0.2)) == Catch::Approx (0.75f).margin (1e-3f));
    // sustain start (decay finishes at t=0.3)
    REQUIRE (out (sampleAt (0.3)) == Catch::Approx (0.5f).margin (1e-3f));
    // sustain end (last sample before release)
    REQUIRE (out (sampleAt (0.7) - 1) == Catch::Approx (0.5f).margin (1e-3f));
    // release start (t = envLen - release = 0.7)
    REQUIRE (out (sampleAt (0.7)) == Catch::Approx (0.5f).margin (1e-3f));
    // release end (t = 0.8): sustain -> 0
    REQUIRE (out (sampleAt (0.8)) == Catch::Approx (0.0f).margin (1e-4f));
    // tail after the release stays at zero
    REQUIRE (out (sampleAt (0.85)) == Catch::Approx (0.0f).margin (1e-4f));
}

//==============================================================================
TEST_CASE ("EnvelopeSignal sine envelope has period 1/hz and spans [0,1]",
           "[envelope][sine]")
{
    constexpr double sr = 48000.0;
    constexpr double hz = 0.5;   // period = 2 s = 96000 samples
    const int periodSamples = static_cast<int> (sr / hz);
    const int numSamples = periodSamples * 2;

    EnvelopeSignal env (std::make_unique<ConstantCarrier> (96000));
    env.setEnvelope (EnvelopeSignal::Envelope::sine);
    env.setSineRate (hz);
    env.prepare (sr, 512);

    juce::AudioBuffer<float> buffer (1, numSamples);
    buffer.clear();
    env.generate (buffer, 0, numSamples);

    // Over the whole range the envelope must reach exactly 0 and 1.
    float minVal = std::numeric_limits<float>::infinity();
    float maxVal = -std::numeric_limits<float>::infinity();
    for (int i = 0; i < numSamples; ++i)
    {
        minVal = std::min (minVal, buffer.getSample (0, i));
        maxVal = std::max (maxVal, buffer.getSample (0, i));
    }

    REQUIRE (minVal == Catch::Approx (0.0f).margin (1e-4f));
    REQUIRE (maxVal == Catch::Approx (1.0f).margin (1e-4f));

    // Quarter-period peaks: sin peaks at t=period/4 -> 1, troughs at 3/4 -> 0.
    REQUIRE (buffer.getSample (0, periodSamples / 4) == Catch::Approx (1.0f).margin (1e-4f));
    REQUIRE (buffer.getSample (0, 3 * periodSamples / 4) == Catch::Approx (0.0f).margin (1e-4f));

    // Periodicity: env(t + period) == env(t).
    for (int t = 0; t < periodSamples; t += 1000)
        REQUIRE (buffer.getSample (0, t + periodSamples)
                 == Catch::Approx (buffer.getSample (0, t)).margin (1e-4f));
}

//==============================================================================
TEST_CASE ("EnvelopeSignal exponential envelope decays as e^(-t/tau)",
           "[envelope][exponential]")
{
    constexpr double sr = 48000.0;
    constexpr double tau = 0.5;   // s

    EnvelopeSignal env (std::make_unique<ConstantCarrier> (96000));
    env.setEnvelope (EnvelopeSignal::Envelope::exponential);
    env.setTau (tau);
    env.prepare (sr, 512);

    const int numSamples = static_cast<int> (sr * 2.0);   // 2 s
    juce::AudioBuffer<float> buffer (1, numSamples);
    buffer.clear();
    env.generate (buffer, 0, numSamples);

    const auto sampleAt = [&] (double sec) { return static_cast<int> (std::llround (sec * sr)); };
    const auto out = [&] (double sec) { return buffer.getSample (0, sampleAt (sec)); };

    // t=0 -> e^0 = 1
    REQUIRE (out (0.0) == Catch::Approx (1.0f).margin (1e-4f));

    // out(t2)/out(t1) == exp(-(t2-t1)/tau)
    const double ratio = out (1.0) / out (0.1);
    const double expected = std::exp (-(1.0 - 0.1) / tau);
    REQUIRE (ratio == Catch::Approx (expected).margin (1e-4));

    const double ratio2 = out (0.5) / out (0.25);
    const double expected2 = std::exp (-(0.5 - 0.25) / tau);
    REQUIRE (ratio2 == Catch::Approx (expected2).margin (1e-4));
}

//==============================================================================
TEST_CASE ("EnvelopeSignal getTotalLength scales inversely with speed",
           "[envelope][speed-length]")
{
    constexpr double sr = 48000.0;
    const int64_t carrierLen = static_cast<int64_t> (sr * 2.0);   // 2 s sweep

    auto makeSweep = []
    {
        auto sweep = std::make_unique<SineSweep>();
        sweep->setFrequencyRange (100.0, 10000.0);
        sweep->setDuration (2.0);
        return sweep;
    };

    EnvelopeSignal envDefault (makeSweep());
    envDefault.prepare (sr, 512);
    REQUIRE (envDefault.getTotalLength() == carrierLen);

    EnvelopeSignal envFast (makeSweep());
    envFast.setSpeed (2.0);
    envFast.prepare (sr, 512);
    REQUIRE (envFast.getTotalLength() == carrierLen / 2);

    EnvelopeSignal envSlow (makeSweep());
    envSlow.setSpeed (0.5);
    envSlow.prepare (sr, 512);
    REQUIRE (envSlow.getTotalLength() == carrierLen * 2);
}

//==============================================================================
TEST_CASE ("EnvelopeSignal setSpeed scales the envelope time axis",
           "[envelope][speed-samples]")
{
    constexpr double sr = 48000.0;
    constexpr int numSamples = 20000;

    auto generateWithSpeed = [&] (double spd, juce::AudioBuffer<float>& out, int num)
    {
        EnvelopeSignal env (std::make_unique<ConstantCarrier> (96000));
        env.setEnvelope (EnvelopeSignal::Envelope::sine);
        env.setSineRate (0.25);
        env.setSpeed (spd);
        env.prepare (sr, 512);

        out.clear();
        env.generate (out, 0, num);
    };

    // The speed=1 reference must cover sample index 2t, so it needs twice
    // the samples of the speed=2 output.
    juce::AudioBuffer<float> out1 (1, 2 * numSamples);
    juce::AudioBuffer<float> out2 (1, numSamples);
    generateWithSpeed (1.0, out1, 2 * numSamples);
    generateWithSpeed (2.0, out2, numSamples);

    // speed=2 output at sample t equals the speed=1 (bare) output at sample 2t,
    // i.e. the whole signal plays twice as fast.
    for (int t = 0; t < numSamples; ++t)
        REQUIRE (out2.getSample (0, t)
                 == Catch::Approx (out1.getSample (0, 2 * t)).margin (1e-6f));

    // Sanity: the two outputs actually differ somewhere (not a tautology).
    REQUIRE (std::abs (out2.getSample (0, 12000) - out1.getSample (0, 12000)) > 0.1f);
}

//==============================================================================
TEST_CASE ("EnvelopeSignal reset restarts carrier and envelope phase",
           "[envelope][reset]")
{
    constexpr double sr = 48000.0;
    constexpr double tau = 0.5;

    EnvelopeSignal env (std::make_unique<ConstantCarrier> (48000));
    env.setEnvelope (EnvelopeSignal::Envelope::exponential);
    env.setTau (tau);
    env.prepare (sr, 512);

    // Pass A: two consecutive blocks; the envelope phase continues across them.
    juce::AudioBuffer<float> passA (1, 12000);
    passA.clear();
    env.generate (passA, 0, 5000);
    env.generate (passA, 5000, 7000);

    // reset() must rewind both the carrier and the envelope phase to t=0.
    env.reset();

    // Pass B: identical block layout after reset.
    juce::AudioBuffer<float> passB (1, 12000);
    passB.clear();
    env.generate (passB, 0, 5000);
    env.generate (passB, 5000, 7000);

    for (int i = 0; i < 12000; ++i)
    {
        // Post-reset output is identical to the pre-reset pass...
        REQUIRE (passB.getSample (0, i) == passA.getSample (0, i));
        // ...and the phase restarted at t=0 (analytic check; without a working
        // reset the phase would have continued at t = (12000+i)/sr).
        const double expected = std::exp (-static_cast<double> (i) / sr / tau);
        REQUIRE (passB.getSample (0, i) == Catch::Approx (expected).margin (1e-5));
    }
}
