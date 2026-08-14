#pragma once

#include <JuceHeader.h>

#include <vector>

#include "ReplicaSpec.h"

//==============================================================================
/**
    Spec-driven replica DSP chain: a series of EQ bell biquads (RBJ peak
    filters, one per band per channel) followed by a feed-forward compressor
    whose signal model is identical to the ground-truth TestCompressorPlugin
    (issue #27, T4).

    Signal model:
      - EQ (per band): juce::dsp::IIR::Filter<float> with
        Coefficients<float>::makePeakFilter (sampleRate, freqHz, q, gain)
        where gain = dBToGain (gainDB) is LINEAR (RBJ peak gain is exactly
        gainFactor at the centre frequency).
      - Order: EQ -> compressor (canonical eq->dyn->eq collapses to a single
        EQ pass before the dynamics; decision U7).
      - Compressor (feed-forward, sample-by-sample, linked across channels):
            levelDB    = 20 * log10 (max over channels |sample|)
            grTarget   = (levelDB > thresholdDB) ? (1 - 1/ratio) * (levelDB - thresholdDB) : 0
            tauDir     = (grTarget > grSmoothed) ? attackSec : releaseSec
            grSmoothed += (1 - exp (-1 / (sr * tauDir))) * (grTarget - grSmoothed)
            output     = input * dBToGain (-grSmoothed) * dBToGain (makeupGainDB)
      grSmoothed holds the gain reduction as a positive dB amount.

    Configuration happens before processing (thread-safety of live parameter
    reads is the wrapper's concern, task T5). Double-precision smoothing, zero
    latency, zero tail.
*/
class ReplicaChain
{
public:
    //==============================================================================
    ReplicaChain();

    /** Convenience: maps a spec onto the setters below. Clears any previous
        configuration first. A spec without eq/dynamics content yields an
        identity chain. */
    void configure (const ReplicaSpec& spec);

    /** Adds one EQ bell (RBJ peak filter) to the end of the series. */
    void addBand (double freqHz, double gainDB, double q);

    /** Sets the feed-forward compressor parameters (same model as
        TestCompressorPlugin: threshold in dB, ratio > 1, true attack/release
        tau in seconds). */
    void setCompressor (double thresholdDB, double ratio, double attackSec, double releaseSec);

    /** Sets a constant make-up gain in dB applied after the gain reduction
        (default: 0). */
    void setMakeupGainDB (double db);

    /** Prepares the chain for the given sample rate. Must be called before
        processBlock. */
    void prepare (double sampleRate);

    /** Resets all DSP state: per-channel filter states and the smoothed GR. */
    void reset();

    /** Processes one audio block: each channel through every active band in
        series (EQ), then the linked compressor. */
    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages);

    /** Returns the smoothed gain reduction in dB at the end of the last
        processed block (mirrors TestCompressorPlugin). */
    double getCurrentGRDB() const noexcept;

private:
    //==============================================================================
    /** One EQ band: its configuration plus per-channel biquad state
        (IIR::Filter is single-channel, so stereo needs one instance per
        channel). */
    struct Band
    {
        double freqHz = 0.0;
        double gainDB = 0.0;
        double q = 1.0;
        std::vector<juce::dsp::IIR::Filter<float>> filters; // one per channel
    };

    static double dBToGain (double db) noexcept;

    /** Grows every band's filter vector to numChannels, (re)creating filters
        from the band configuration and preparing them at the current rate. */
    void ensureFilters (int numChannels);

    std::vector<Band> bands;
    double sampleRate = 48000.0;

    bool hasCompressor = false;
    double thresholdDB = 0.0;
    double ratio = 1.0;
    double attackSec = 0.0;
    double releaseSec = 0.0;
    double makeupGainDB = 0.0;
    double grSmoothed = 0.0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ReplicaChain)
};
