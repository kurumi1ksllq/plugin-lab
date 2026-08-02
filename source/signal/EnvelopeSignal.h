#pragma once

#include "SignalGenerator.h"

#include <memory>

/**
 * EnvelopeSignal — wraps a carrier SignalGenerator and multiplies its output
 * by a time-varying envelope (ADSR, sine, or exponential decay).
 *
 * The envelope time axis is scaled by `speed`: setSpeed(2.0) makes the
 * envelope run twice as fast and halves the total output length.
 * The envelope is disabled (identity, output == carrier) until setEnvelope()
 * is called.
 *
 * ADSR total length: the release phase occupies the final `releaseSec` of the
 * carrier's total length when the carrier is finite; for an indefinite
 * carrier the sustain level holds forever.
 */
class EnvelopeSignal final : public SignalGenerator
{
public:
    enum class Envelope { adsr, sine, exponential };

    /** Take ownership of the carrier generator. */
    explicit EnvelopeSignal (std::unique_ptr<SignalGenerator> carrier);

    ~EnvelopeSignal() override = default;

    //==============================================================================
    /** Select the envelope shape; enables the envelope. */
    void setEnvelope (Envelope env);

    /** Scale the envelope time axis: total duration /= speed (default 1.0). */
    void setSpeed (double speed);

    /** Configure the ADSR envelope (attack/decay/release in seconds). */
    void setADSR (double attackSec, double decaySec, double sustain, double releaseSec);

    /** Sine modulation rate in Hz (0..1 recommended). */
    void setSineRate (double hz);

    /** Exponential decay time constant: e^(-t/tau). */
    void setTau (double sec);

    //==============================================================================
    void prepare (double sampleRate, int blockSize) override;
    void generate (juce::AudioBuffer<float>& buffer,
                   int startSample,
                   int numSamples) override;
    int64_t getTotalLength() const override;
    void reset() override;

private:
    /** Envelope value at envelope-time `t` (already scaled by speed).
     *  `totalSec` is the envelope's total duration in seconds, or infinity
     *  for an indefinite carrier (used for ADSR release placement). */
    double envelopeAt (double t, double totalSec) const;

    std::unique_ptr<SignalGenerator> carrier;

    Envelope envelope = Envelope::adsr;
    bool envelopeEnabled = false;

    double speed = 1.0;

    double attackSec = 0.01;
    double decaySec = 0.1;
    double sustainLevel = 0.5;
    double releaseSec = 0.1;

    double sineRateHz = 1.0;
    double tauSec = 0.5;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EnvelopeSignal)
};
