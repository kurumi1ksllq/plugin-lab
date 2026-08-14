#include "ReplicaChain.h"

#include <cmath>
#include <limits>
#include <utility>

//==============================================================================
// Signal model (issue #27, T4):
//   - EQ: per band, one RBJ peak biquad per channel (IIR::Filter is mono);
//     centre-frequency gain is exactly dBToGain (gainDB).
//   - Order: EQ -> compressor (decision U7).
//   - Compressor: identical feed-forward model to TestCompressorPlugin (the
//     ground-truth reference) - see ReplicaChain.h for the equations.

ReplicaChain::ReplicaChain() = default;

void ReplicaChain::configure (const ReplicaSpec& spec)
{
    bands.clear();
    hasCompressor = false;
    makeupGainDB = 0.0;

    if (spec.hasEq)
    {
        for (const auto& band : spec.bands)
            addBand (band.freqHz, band.gainDB, band.q);
    }

    if (spec.hasCompression)
        setCompressor (spec.thresholdDB, spec.ratio, spec.attackSec, spec.releaseSec);
}

void ReplicaChain::addBand (double freqHz, double gainDB, double q)
{
    Band band;
    band.freqHz = freqHz;
    band.gainDB = gainDB;
    band.q = q;
    bands.push_back (std::move (band));
}

void ReplicaChain::setCompressor (double threshold, double compressionRatio,
                                  double attack, double release)
{
    hasCompressor = true;
    thresholdDB = threshold;
    ratio = compressionRatio;
    attackSec = attack;
    releaseSec = release;
}

void ReplicaChain::setMakeupGainDB (double db)
{
    makeupGainDB = db;
}

void ReplicaChain::prepare (double newSampleRate)
{
    sampleRate = newSampleRate;
}

void ReplicaChain::reset()
{
    for (auto& band : bands)
        for (auto& filter : band.filters)
            filter.reset();

    grSmoothed = 0.0;
}

void ReplicaChain::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    const int numChannels = buffer.getNumChannels();
    const int numSamples = buffer.getNumSamples();

    if (numSamples == 0)
        return;

    ensureFilters (numChannels);

    // EQ: each channel through every active band in series.
    for (auto& band : bands)
    {
        for (int ch = 0; ch < numChannels; ++ch)
        {
            auto* data = buffer.getWritePointer (ch);
            auto& filter = band.filters[static_cast<size_t> (ch)];
            for (int i = 0; i < numSamples; ++i)
                data[i] = filter.processSample (data[i]);
        }
    }

    if (! hasCompressor)
        return;

    // Compressor: feed-forward, sample-by-sample, linked across channels.
    const double sr = std::max (sampleRate, 1.0);

    for (int i = 0; i < numSamples; ++i)
    {
        // Detector: the maximum instantaneous magnitude across channels.
        double level = 0.0;
        for (int ch = 0; ch < numChannels; ++ch)
            level = std::max (level, std::abs (static_cast<double> (buffer.getSample (ch, i))));

        // Static compression curve (instantaneous detector).
        const double levelDB = (level > 0.0)
                                   ? 20.0 * std::log10 (level)
                                   : -std::numeric_limits<double>::infinity();
        const double grTarget = (levelDB > thresholdDB)
                                    ? (1.0 - 1.0 / ratio) * (levelDB - thresholdDB)
                                    : 0.0;

        // Single-pole smoothing with direction-dependent time constant,
        // updated exactly once per sample so tau is measurable.
        const double tau = (grTarget > grSmoothed) ? attackSec : releaseSec;
        grSmoothed += (1.0 - std::exp (-1.0 / (sr * tau))) * (grTarget - grSmoothed);

        const double gain = dBToGain (-grSmoothed) * dBToGain (makeupGainDB);
        for (int ch = 0; ch < numChannels; ++ch)
        {
            const double input = static_cast<double> (buffer.getSample (ch, i));
            buffer.setSample (ch, i, static_cast<float> (input * gain));
        }
    }
}

double ReplicaChain::getCurrentGRDB() const noexcept
{
    return grSmoothed;
}

double ReplicaChain::dBToGain (double db) noexcept
{
    return std::pow (10.0, db / 20.0);
}

void ReplicaChain::ensureFilters (int numChannels)
{
    for (auto& band : bands)
    {
        while (static_cast<int> (band.filters.size()) < numChannels)
        {
            const float gain = static_cast<float> (dBToGain (band.gainDB));
            auto coefficients = juce::dsp::IIR::Coefficients<float>::makePeakFilter (
                sampleRate, static_cast<float> (band.freqHz), static_cast<float> (band.q), gain);

            band.filters.emplace_back (coefficients);

            juce::dsp::ProcessSpec spec { sampleRate, 512, 1 };
            band.filters.back().prepare (spec);
        }
    }
}
