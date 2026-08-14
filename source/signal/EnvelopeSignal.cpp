#include "EnvelopeSignal.h"

#include "../utils/CrashLog.h"

#include <algorithm>
#include <cmath>
#include <limits>

EnvelopeSignal::EnvelopeSignal (std::unique_ptr<SignalGenerator> carrier_)
    : carrier (std::move (carrier_))
{
    jassert (carrier != nullptr);
}

void EnvelopeSignal::setEnvelope (Envelope env)
{
    envelope = env;
    envelopeEnabled = true;
}

void EnvelopeSignal::setSpeed (double newSpeed)
{
    speed = newSpeed > 0.0 ? newSpeed : 1.0;
}

void EnvelopeSignal::setADSR (double attack, double decay, double sustain, double release)
{
    attackSec = attack;
    decaySec = decay;
    sustainLevel = sustain;
    releaseSec = release;
}

void EnvelopeSignal::setSineRate (double hz)
{
    sineRateHz = hz;
}

void EnvelopeSignal::setTau (double sec)
{
    tauSec = sec;
}

void EnvelopeSignal::prepare (double sr, int bs)
{
    carrier->prepare (sr, bs);
    // Sets this generator's sampleRate/blockSize and (via the virtual
    // reset()) rewinds the envelope phase to t=0.
    SignalGenerator::prepare (sr, bs);
}

void EnvelopeSignal::generate (juce::AudioBuffer<float>& buffer,
                               int startSample,
                               int numSamples)
{
    carrier->generate (buffer, startSample, numSamples);

    if (! envelopeEnabled)
    {
        currentSample += numSamples;
        return;
    }

    // Total envelope duration in seconds; used to place the ADSR release.
    const auto carrierLen = carrier->getTotalLength();
    const double totalSec = carrierLen >= 0
                                ? static_cast<double> (carrierLen) / sampleRate * speed
                                : std::numeric_limits<double>::infinity();

    const auto numChannels = buffer.getNumChannels();

    for (int s = 0; s < numSamples; ++s)
    {
        const double t = (currentSample + s) / sampleRate;
        const float scale = static_cast<float> (envelopeAt (t * speed, totalSec));

        for (int ch = 0; ch < numChannels; ++ch)
            buffer.getWritePointer (ch)[startSample + s] *= scale;
    }

    currentSample += numSamples;
}

int64_t EnvelopeSignal::getTotalLength() const
{
    const auto carrierLen = carrier->getTotalLength();
    if (carrierLen < 0)
    {
        // Indefinite carrier (issue #44): -1 is the "infinite" sentinel
        // SweepRunner depends on (silent 10 s fallback). The semantics stay
        // as-is, but the fallback must never be silent again — log it so an
        // infinite carrier always leaves a trail.
        CRASH_LOG_WARN ("EnvelopeSignal indefinite carrier",
                        "getTotalLength() returning -1 (SweepRunner applies the 10s fallback)");
        return -1;
    }

    return static_cast<int64_t> (std::llround (static_cast<double> (carrierLen) / speed));
}

void EnvelopeSignal::reset()
{
    carrier->reset();
    currentSample = 0.0;
}

double EnvelopeSignal::envelopeAt (double t, double totalSec) const
{
    switch (envelope)
    {
        case Envelope::sine:
            return 0.5 * (1.0 + std::sin (2.0 * juce::MathConstants<double>::pi
                                          * sineRateHz * t));

        case Envelope::exponential:
            return std::exp (-t / tauSec);

        case Envelope::adsr:
        {
            if (t < 0.0)
                return 0.0;

            // The release occupies the final releaseSec of the envelope; if
            // the carrier is too short for a sustain flat, release starts
            // right after the decay.
            const double minEnvSec = attackSec + decaySec;
            const double releaseStart = std::max (minEnvSec, totalSec - releaseSec);

            if (t < attackSec)
                return t / attackSec;   // attack: 0 -> 1

            if (t < minEnvSec)
                return 1.0 - (1.0 - sustainLevel) * ((t - attackSec) / decaySec);   // decay: 1 -> sustain

            if (t < releaseStart)
                return sustainLevel;   // sustain hold

            if (t < releaseStart + releaseSec)
                return sustainLevel * (1.0 - ((t - releaseStart) / releaseSec));   // release: sustain -> 0

            return 0.0;
        }
    }

    jassertfalse;
    return 1.0;
}
