#include "StereoAnalysis.h"
#include "../utils/FftHelper.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <functional>
#include <vector>

namespace
{
constexpr double kMinFreqHz = 20.0;
constexpr double kMaxFreqHz = 20000.0;

/** Magnitude floor for the dB conversion (-120 dB, matching
 *  MathUtils::amplitudeToDB's default floor), so a dead side path reports a
 *  finite width instead of -inf/NaN. */
constexpr double kMagnitudeFloor = 1e-6;

double toDB (double magnitude)
{
    return 20.0 * std::log10 (std::max (magnitude, kMagnitudeFloor));
}

using ComplexVec = std::vector<std::complex<double>>;

/** Phase unwrap + latency-ramp removal, mirroring FreqResponse::applyPhasePost:
 *  L and R share the plugin latency, so the decoded M/S response carries the
 *  same linear ramp. */
void applyPhasePost (std::vector<StereoAnalysis::Point>& points,
                     double sampleRate,
                     int latencySamples)
{
    if (points.size() >= 2)
    {
        double cumulativeOffset = 0.0;
        double prevRaw = points[0].phaseDeg;

        for (size_t i = 1; i < points.size(); ++i)
        {
            const double raw  = points[i].phaseDeg;
            const double diff = raw - prevRaw;

            if (diff > 180.0)
                cumulativeOffset -= 360.0;
            else if (diff < -180.0)
                cumulativeOffset += 360.0;

            points[i].phaseDeg = raw + cumulativeOffset;
            prevRaw = raw;
        }
    }

    if (latencySamples > 0 && sampleRate > 0.0)
    {
        const double coeff = 360.0 * static_cast<double> (latencySamples) / sampleRate;
        for (auto& p : points)
            p.phaseDeg += p.frequency * coeff;
    }
}

/** Per-bin accumulators for one excitation run: the dry autospectrum and the
 *  conj(dry) * wet cross-spectrum of the H1 estimator. */
struct RunAccum
{
    explicit RunAccum (int numBins)
        : sxx (static_cast<size_t> (numBins), 0.0),
          sxy (static_cast<size_t> (numBins), { 0.0, 0.0 })
    {
    }

    std::vector<double> sxx;
    ComplexVec sxy;
};
}  // namespace

StereoAnalysis::Result StereoAnalysis::analyze (const juce::AudioBuffer<float>& midDry,
                                                const juce::AudioBuffer<float>& midWet,
                                                const juce::AudioBuffer<float>& sideDry,
                                                const juce::AudioBuffer<float>& sideWet,
                                                double sr,
                                                int latencySamples)
{
    Result result;
    result.sampleRate = sr;

    // MS decode needs a stereo pair on every recording. A mono capture (mono
    // plugin, or a channel-count mismatch) is an explicit "not measurable":
    // valid stays false and no curve is produced, so the callers report an
    // error instead of exporting mono data under a stereo_width label.
    const int numChannels = std::min ({ midDry.getNumChannels(), midWet.getNumChannels(),
                                        sideDry.getNumChannels(), sideWet.getNumChannels() });
    if (numChannels < 2)
        return result;

    const int numSamples = std::min ({ midDry.getNumSamples(), midWet.getNumSamples(),
                                       sideDry.getNumSamples(), sideWet.getNumSamples() });

    constexpr int fftSize = 1 << fftOrder;
    constexpr int hopSize = fftSize / 4;
    constexpr int numBins = fftSize / 2 + 1;
    const double freqStep = sr / fftSize;

    if (numSamples < fftSize)
        return result;

    FftHelper fft (fftOrder);
    std::vector<float> window (static_cast<size_t> (fftSize));
    std::vector<float> real (static_cast<size_t> (numBins));
    std::vector<float> imag (static_cast<size_t> (numBins));

    // Hann-windowed FFT of one (possibly MS-decoded) channel combination.
    const auto spectrumOf = [&] (const std::function<double (int)>& sampleAt) -> ComplexVec
    {
        for (int i = 0; i < fftSize; ++i)
            window[static_cast<size_t> (i)] = static_cast<float> (sampleAt (i));

        FftHelper::applyHannWindow (window.data(), fftSize);
        fft.forwardReal (window.data(), real.data(), imag.data(), false);

        ComplexVec spectrum (static_cast<size_t> (numBins));
        for (int b = 0; b < numBins; ++b)
            spectrum[static_cast<size_t> (b)] = std::complex<double> (real[static_cast<size_t> (b)],
                                                                      imag[static_cast<size_t> (b)]);
        return spectrum;
    };

    const auto accumulate = [&] (RunAccum& acc, const ComplexVec& dry, const ComplexVec& wet)
    {
        for (int b = 0; b < numBins; ++b)
        {
            const size_t idx = static_cast<size_t> (b);
            acc.sxx[idx] += std::norm (dry[idx]);
            acc.sxy[idx] += std::conj (dry[idx]) * wet[idx];
        }
    };

    // Run A (mid-only): dry/wet M for H_M, plus the wet L/R pair for rho.
    RunAccum runA (numBins);
    std::vector<double> wetLsxx (static_cast<size_t> (numBins), 0.0);
    std::vector<double> wetRsyy (static_cast<size_t> (numBins), 0.0);
    ComplexVec wetLrSxy (static_cast<size_t> (numBins), { 0.0, 0.0 });

    // Run B (side-only): dry/wet S for H_S.
    RunAccum runB (numBins);

    int windowCount = 0;

    for (int pos = 0; pos + fftSize <= numSamples; pos += hopSize)
    {
        // --- Run A: M = (L + R) / 2 ---
        const auto midDrySpec = spectrumOf ([&midDry, pos] (int i)
            { return 0.5 * (midDry.getReadPointer (0)[pos + i]
                            + midDry.getReadPointer (1)[pos + i]); });
        const auto midWetSpec = spectrumOf ([&midWet, pos] (int i)
            { return 0.5 * (midWet.getReadPointer (0)[pos + i]
                            + midWet.getReadPointer (1)[pos + i]); });
        accumulate (runA, midDrySpec, midWetSpec);

        // rho is measured on the plugin OUTPUT pair: input L=R (rho = +1) with
        // an output rho < 1 is the evidence of widening (ADR-0003).
        const auto wetLeftSpec  = spectrumOf ([&midWet, pos] (int i)
            { return midWet.getReadPointer (0)[pos + i]; });
        const auto wetRightSpec = spectrumOf ([&midWet, pos] (int i)
            { return midWet.getReadPointer (1)[pos + i]; });

        for (int b = 0; b < numBins; ++b)
        {
            const size_t idx = static_cast<size_t> (b);
            wetLsxx[idx] += std::norm (wetLeftSpec[idx]);
            wetRsyy[idx] += std::norm (wetRightSpec[idx]);
            wetLrSxy[idx] += std::conj (wetLeftSpec[idx]) * wetRightSpec[idx];
        }

        // --- Run B: S = (L - R) / 2 ---
        const auto sideDrySpec = spectrumOf ([&sideDry, pos] (int i)
            { return 0.5 * (sideDry.getReadPointer (0)[pos + i]
                            - sideDry.getReadPointer (1)[pos + i]); });
        const auto sideWetSpec = spectrumOf ([&sideWet, pos] (int i)
            { return 0.5 * (sideWet.getReadPointer (0)[pos + i]
                            - sideWet.getReadPointer (1)[pos + i]); });
        accumulate (runB, sideDrySpec, sideWetSpec);

        ++windowCount;
    }

    if (windowCount == 0)
        return result;

    const double lowEnergyThreshold = static_cast<double> (windowCount) * 1e-8;

    for (int bin = 0; bin < numBins; ++bin)
    {
        const double freq = bin * freqStep;
        if (freq < kMinFreqHz || freq > kMaxFreqHz)
            continue;

        const size_t idx = static_cast<size_t> (bin);

        // The two runs are gated independently: the mid-only excitation has no
        // dry S energy (and the side-only one no dry M energy), so a shared
        // gate would discard every bin. A bin without dry energy in a run is
        // simply not part of that run's response.
        const bool hasMid  = runA.sxx[idx] >= lowEnergyThreshold;
        const bool hasSide = runB.sxx[idx] >= lowEnergyThreshold;

        double midDB = 0.0;
        double sideDB = 0.0;

        if (hasMid)
        {
            const std::complex<double> hM = runA.sxy[idx] / runA.sxx[idx];
            midDB = toDB (std::abs (hM));

            Point midPoint;
            midPoint.frequency = freq;
            midPoint.magnitudeDB = midDB;
            midPoint.phaseDeg = std::atan2 (hM.imag(), hM.real()) * 180.0
                                / juce::MathConstants<double>::pi;
            result.midResponse.push_back (midPoint);
        }

        if (hasSide)
        {
            const std::complex<double> hS = runB.sxy[idx] / runB.sxx[idx];
            sideDB = toDB (std::abs (hS));

            Point sidePoint;
            sidePoint.frequency = freq;
            sidePoint.magnitudeDB = sideDB;
            sidePoint.phaseDeg = std::atan2 (hS.imag(), hS.real()) * 180.0
                                 / juce::MathConstants<double>::pi;
            result.sideResponse.push_back (sidePoint);
        }

        // rho(f) = |Sxy|^2 / (Sxx * Syy) on the wet L/R pair of the mid run
        // (ADR-0003). Magnitude-squared, so it lives in [0, 1]: 1 = the output
        // pair is fully coherent (mono/untouched), lower = decorrelated.
        const double denom = wetLsxx[idx] * wetRsyy[idx];
        if (denom > 0.0)
        {
            CorrelationPoint corrPoint;
            corrPoint.frequency = freq;
            corrPoint.rho = juce::jlimit (0.0, 1.0, std::norm (wetLrSxy[idx]) / denom);
            result.correlation.push_back (corrPoint);
        }

        // width_db(f) = 20*log10(|M| / |S|), evaluated as the difference of the
        // two MS responses so the magnitudes never leave dB space. Needs both.
        if (hasMid && hasSide)
        {
            WidthPoint widthPoint;
            widthPoint.frequency = freq;
            widthPoint.widthDB = midDB - sideDB;
            result.widthDB.push_back (widthPoint);
        }
    }

    applyPhasePost (result.midResponse, sr, latencySamples);
    applyPhasePost (result.sideResponse, sr, latencySamples);

    // An empty curve set means nothing was measurable (e.g. a plugin that
    // silences both paths): valid stays false so the caller reports an error.
    result.valid = ! result.midResponse.empty();
    return result;
}