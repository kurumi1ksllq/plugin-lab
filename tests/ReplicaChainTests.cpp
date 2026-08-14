#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <cmath>
#include <vector>

#include "../source/replica/ReplicaChain.h"

//==============================================================================
// Tests for ReplicaChain — the spec-driven replica DSP chain (series EQ bell
// biquads followed by a feed-forward compressor).
//
// Signal model under test (issue #27, T4):
//   EQ:      per band, juce::dsp::IIR::Filter<float> with RBJ peak
//            coefficients; centre-frequency gain is exactly dBToGain (gainDB).
//   Order:   EQ -> compressor (canonical eq->dyn->eq collapses to a single EQ
//            pass before the dynamics).
//   Compressor: identical model to TestCompressorPlugin (ground truth):
//            levelDB = 20*log10 (max over channels |sample|)
//            grTarget = levelDB > threshold ? (1 - 1/ratio) * (levelDB - threshold) : 0
//            tauDir = grTarget > grSmoothed ? attackSec : releaseSec
//            grSmoothed += (1 - exp (-1 / (sr * tauDir))) * (grTarget - grSmoothed)
//            output = input * dBToGain (-grSmoothed) * dBToGain (makeupGainDB)
//
// Because grSmoothed is a true single-pole filter, a step in input level
// produces exactly measurable exponential curves:
//   attack : GR(t) = GR_ss * (1 - e^(-t / tau_attack))
//   release: GR(t) = GR_ss * e^(-t / tau_release)

namespace
{
constexpr double kSampleRate = 48000.0;

/** dB -> linear amplitude gain. */
double dBToGain (double db)
{
    return std::pow (10.0, db / 20.0);
}

/** Fills a stereo buffer with a sine at the given frequency and amplitude. */
void fillSine (juce::AudioBuffer<float>& buffer, double freqHz, float amplitude)
{
    const double twoPi = juce::MathConstants<double>::twoPi;
    for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
        for (int s = 0; s < buffer.getNumSamples(); ++s)
            buffer.setSample (ch, s, static_cast<float> (amplitude * std::sin (twoPi * freqHz * s / kSampleRate)));
}

/** Fills a stereo buffer with a constant level. */
void fillConstant (juce::AudioBuffer<float>& buffer, float level)
{
    for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
        for (int s = 0; s < buffer.getNumSamples(); ++s)
            buffer.setSample (ch, s, level);
}

/**
    Processes the buffer through the chain in `blockSize`-sized chunks using
    non-owning AudioBuffer views, so cross-block state continuity is exercised.
*/
void processChain (ReplicaChain& chain, juce::AudioBuffer<float>& buffer, int blockSize)
{
    juce::MidiBuffer midi;
    std::vector<float*> channelData (static_cast<size_t> (buffer.getNumChannels()), nullptr);

    for (int start = 0; start < buffer.getNumSamples(); start += blockSize)
    {
        const int num = juce::jmin (blockSize, buffer.getNumSamples() - start);
        for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
            channelData[static_cast<size_t> (ch)] = buffer.getWritePointer (ch) + start;

        juce::AudioBuffer<float> view (channelData.data(), buffer.getNumChannels(), num);
        chain.processBlock (view, midi);
    }
}

/** RMS (linear amplitude) of channel 0 over [startSample, endSample). */
double rmsOf (const juce::AudioBuffer<float>& buffer, int startSample, int endSample)
{
    double sum = 0.0;
    const int num = endSample - startSample;
    for (int s = startSample; s < endSample; ++s)
    {
        const double v = static_cast<double> (buffer.getSample (0, s));
        sum += v * v;
    }
    return std::sqrt (sum / static_cast<double> (num));
}
} // namespace

//==============================================================================

TEST_CASE ("ReplicaChain EQ bell boosts a sine exactly at the centre frequency",
           "[replica-chain][eq-bell]")
{
    // Arrange: one +6 dB bell at 1 kHz, Q = 1.
    ReplicaSpec spec;
    spec.hasEq = true;
    spec.bands = { ReplicaEQBand { 1000.0, 6.0, 1.0 } };

    ReplicaChain chain;
    chain.configure (spec);
    chain.prepare (kSampleRate);

    // Act: 1.1 s of a 1 kHz sine at amplitude 0.1 (1000 cycles per second ->
    // integer cycles in every full-second window).
    juce::AudioBuffer<float> buffer (2, static_cast<int> (1.1 * kSampleRate));
    fillSine (buffer, 1000.0, 0.1f);
    processChain (chain, buffer, 512);

    // Assert: skip the first 0.1 s (filter settle-in), then RMS of the
    // remaining 1 s equals input RMS * dBToGain (6).
    const double inputRms = 0.1 / std::sqrt (2.0);
    const double outRms = rmsOf (buffer, static_cast<int> (0.1 * kSampleRate),
                                 buffer.getNumSamples());
    REQUIRE (outRms == Catch::Approx (inputRms * dBToGain (6.0)).margin (0.005));
}

TEST_CASE ("ReplicaChain EQ bell leaves a far-off frequency essentially unchanged",
           "[replica-chain][eq-bell]")
{
    // Arrange: one +6 dB bell at 1 kHz, Q = 1.
    ReplicaSpec spec;
    spec.hasEq = true;
    spec.bands = { ReplicaEQBand { 1000.0, 6.0, 1.0 } };

    ReplicaChain chain;
    chain.configure (spec);
    chain.prepare (kSampleRate);

    // Act: 1.1 s of a 100 Hz sine (a decade below the centre).
    juce::AudioBuffer<float> buffer (2, static_cast<int> (1.1 * kSampleRate));
    fillSine (buffer, 100.0, 0.1f);
    processChain (chain, buffer, 512);

    // Assert: response at 100 Hz deviates < 0.5 dB from unity (the RBJ peak
    // with Q = 1 is +0.065 dB there).
    const double inputRms = 0.1 / std::sqrt (2.0);
    const double outRms = rmsOf (buffer, static_cast<int> (0.1 * kSampleRate),
                                 buffer.getNumSamples());
    REQUIRE (outRms == Catch::Approx (inputRms).margin (0.004));
}

TEST_CASE ("ReplicaChain compressor passes below-threshold signals unchanged",
           "[replica-chain][compressor][below-threshold]")
{
    // Arrange: threshold -20 dB, ratio 4.
    ReplicaChain chain;
    chain.setCompressor (-20.0, 4.0, 0.005, 0.05);
    chain.setMakeupGainDB (0.0);
    chain.prepare (kSampleRate);

    // Act: 0.5 s of a 440 Hz sine at amplitude 0.01 (-40 dBFS, well below the
    // threshold).
    juce::AudioBuffer<float> buffer (2, static_cast<int> (0.5 * kSampleRate));
    fillSine (buffer, 440.0, 0.01f);
    processChain (chain, buffer, 512);

    // Assert: no gain reduction and the output equals the input.
    REQUIRE (chain.getCurrentGRDB() == Catch::Approx (0.0).margin (0.001));
    const double inputRms = 0.01 / std::sqrt (2.0);
    const double outRms = rmsOf (buffer, 0, buffer.getNumSamples());
    REQUIRE (outRms == Catch::Approx (inputRms).margin (0.0005));
}

TEST_CASE ("ReplicaChain compressor reaches the static-curve gain reduction at steady state",
           "[replica-chain][compressor][steady-state]")
{
    // Arrange: threshold -20 dB, ratio 4, fast smoothing (0.5 ms).
    ReplicaChain chain;
    chain.setCompressor (-20.0, 4.0, 0.0005, 0.0005);
    chain.setMakeupGainDB (0.0);
    chain.prepare (kSampleRate);

    // Act: 2 s of constant level 0.5 (-6.02 dBFS);
    // GR_ss = (1 - 1/4) * (-6.02 - (-20)) = 10.4846 dB.
    const double grExpected = (1.0 - 1.0 / 4.0) * (20.0 * std::log10 (0.5) + 20.0);
    juce::AudioBuffer<float> buffer (2, static_cast<int> (2.0 * kSampleRate));
    fillConstant (buffer, 0.5f);
    processChain (chain, buffer, 512);

    // Assert: GR converged to the static curve and the output RMS matches
    // input * dBToGain (-GR_ss).
    REQUIRE (chain.getCurrentGRDB() == Catch::Approx (grExpected).margin (0.1));
    const double expectedOut = 0.5 * dBToGain (-grExpected);
    const double outRms = rmsOf (buffer, buffer.getNumSamples() - 512, buffer.getNumSamples());
    REQUIRE (outRms == Catch::Approx (expectedOut).epsilon (0.01));
}

TEST_CASE ("ReplicaChain compressor attack follows a 1-pole curve with the configured attack tau",
           "[replica-chain][compressor][attack-tau]")
{
    // Arrange: attack tau = 5 ms = 240 samples at 48 kHz.
    ReplicaChain chain;
    chain.setCompressor (-20.0, 4.0, 0.005, 0.05);
    chain.setMakeupGainDB (0.0);
    chain.prepare (kSampleRate);

    // Act: a single 240-sample block of constant 0.5 lands the final sample
    // exactly at t = tau.
    const int tauSamples = static_cast<int> (0.005 * kSampleRate);
    juce::AudioBuffer<float> buffer (2, tauSamples);
    fillConstant (buffer, 0.5f);
    processChain (chain, buffer, tauSamples);

    // Assert: GR (tau) == GR_ss * (1 - e^-1) = 0.6321 * GR_ss.
    const double grExpected = (1.0 - 1.0 / 4.0) * (20.0 * std::log10 (0.5) + 20.0);
    REQUIRE (chain.getCurrentGRDB() == Catch::Approx ((1.0 - std::exp (-1.0)) * grExpected).epsilon (0.05));
}

TEST_CASE ("ReplicaChain compressor release follows a 1-pole curve with the configured release tau",
           "[replica-chain][compressor][release-tau]")
{
    // Arrange: release tau = 50 ms = 2400 samples at 48 kHz.
    ReplicaChain chain;
    chain.setCompressor (-20.0, 4.0, 0.005, 0.05);
    chain.setMakeupGainDB (0.0);
    chain.prepare (kSampleRate);

    // Act: reach steady state at 0.5 (GR = GR_ss), then drop below threshold.
    juce::AudioBuffer<float> steady (2, static_cast<int> (2.0 * kSampleRate));
    fillConstant (steady, 0.5f);
    processChain (chain, steady, 512);
    const double grBefore = chain.getCurrentGRDB();

    // A single 2400-sample block of 0.01 (-40 dBFS) puts the final sample at
    // t = tau_release.
    const int tauSamples = static_cast<int> (0.05 * kSampleRate);
    juce::AudioBuffer<float> release (2, tauSamples);
    fillConstant (release, 0.01f);
    processChain (chain, release, tauSamples);

    // Assert: GR (tau) == GR_ss * e^-1 = 0.3679 * GR_ss.
    REQUIRE (chain.getCurrentGRDB() == Catch::Approx (std::exp (-1.0) * grBefore).epsilon (0.05));
}

TEST_CASE ("ReplicaChain EQ bands act independently at their centre frequencies",
           "[replica-chain][eq-multiband]")
{
    // Arrange: four bells with distinct centres/gains; Q = 3 keeps the skirts
    // narrow enough that each centre sees its own band within 0.5 dB.
    struct BandSpec
    {
        double freqHz;
        double gainDB;
    };
    const BandSpec bandSpecs[] = { { 100.0, 6.0 }, { 1000.0, -6.0 }, { 8000.0, 12.0 }, { 16000.0, -3.0 } };

    ReplicaSpec spec;
    spec.hasEq = true;
    for (const auto& band : bandSpecs)
        spec.bands.push_back (ReplicaEQBand { band.freqHz, band.gainDB, 3.0 });

    // Act + Assert: at each centre frequency the measured gain matches that
    // band's gain, independently of the others.
    for (const auto& band : bandSpecs)
    {
        ReplicaChain chain;
        chain.configure (spec);
        chain.prepare (kSampleRate);

        juce::AudioBuffer<float> buffer (2, static_cast<int> (1.1 * kSampleRate));
        fillSine (buffer, band.freqHz, 0.1f);
        processChain (chain, buffer, 512);

        const double inputRms = 0.1 / std::sqrt (2.0);
        const double outRms = rmsOf (buffer, static_cast<int> (0.1 * kSampleRate),
                                     buffer.getNumSamples());
        const double gainDB = 20.0 * std::log10 (outRms / inputRms);
        REQUIRE (gainDB == Catch::Approx (band.gainDB).margin (0.5));
    }
}

TEST_CASE ("ReplicaChain EQ boost feeds the compressor before it reaches the output",
           "[replica-chain][ordering]")
{
    // Arrange: +6 dB bell at 1 kHz followed by a compressor (threshold -20 dB,
    // ratio 4, slow 100 ms smoothing so GR settles to its steady-state mean).
    ReplicaSpec fullSpec;
    fullSpec.hasEq = true;
    fullSpec.bands = { ReplicaEQBand { 1000.0, 6.0, 1.0 } };
    fullSpec.hasCompression = true;
    fullSpec.thresholdDB = -20.0;
    fullSpec.ratio = 4.0;
    fullSpec.attackSec = 0.1;
    fullSpec.releaseSec = 0.1;

    ReplicaSpec compSpec;
    compSpec.hasCompression = true;
    compSpec.thresholdDB = -20.0;
    compSpec.ratio = 4.0;
    compSpec.attackSec = 0.1;
    compSpec.releaseSec = 0.1;

    ReplicaSpec eqSpec;
    eqSpec.hasEq = true;
    eqSpec.bands = { ReplicaEQBand { 1000.0, 6.0, 1.0 } };

    // A 1 kHz sine at amplitude 0.08: pre-EQ peak is -21.9 dBFS (below the
    // -20 dB threshold); post-EQ peak is -15.9 dBFS (above it). Only an EQ
    // placed BEFORE the compressor can trigger compression.
    const int numSamples = static_cast<int> (2.0 * kSampleRate);
    juce::AudioBuffer<float> buffer (2, numSamples);
    fillSine (buffer, 1000.0, 0.08f);

    ReplicaChain fullChain;
    fullChain.configure (fullSpec);
    fullChain.prepare (kSampleRate);
    ReplicaChain compChain;
    compChain.configure (compSpec);
    compChain.prepare (kSampleRate);
    ReplicaChain eqChain;
    eqChain.configure (eqSpec);
    eqChain.prepare (kSampleRate);

    // Act: process identical material through all three chains.
    juce::AudioBuffer<float> fullBuffer (buffer);
    juce::AudioBuffer<float> compBuffer (buffer);
    juce::AudioBuffer<float> eqBuffer (buffer);
    processChain (fullChain, fullBuffer, 512);
    processChain (compChain, compBuffer, 512);
    processChain (eqChain, eqBuffer, 512);

    // Assert: the compressor acting on the raw (pre-EQ) signal stays at zero
    // GR; with the EQ ahead of it the compressor sees the boosted signal and
    // applies clear gain reduction (steady-state mean well above the noise
    // floor of the tau measurement).
    REQUIRE (compChain.getCurrentGRDB() == Catch::Approx (0.0).margin (0.001));
    REQUIRE (fullChain.getCurrentGRDB() > 0.5);

    // The full chain's output is audibly attenuated relative to the EQ-only
    // chain (compression reduces the boosted signal; ~13% in the model).
    const double fullRms = rmsOf (fullBuffer, numSamples / 2, numSamples);
    const double eqRms = rmsOf (eqBuffer, numSamples / 2, numSamples);
    REQUIRE (fullRms < eqRms * 0.95);
    REQUIRE (fullRms > eqRms * 0.7);
}
