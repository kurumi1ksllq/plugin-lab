#include "FreqResponse.h"
#include "../utils/FftHelper.h"
#include "../utils/MathUtils.h"
#include <cmath>
#include <complex>

FreqResponse::Result FreqResponse::analyze (
    const juce::AudioBuffer<float>& dry,
    const juce::AudioBuffer<float>& wet,
    double sr)
{
    Result result;
    result.sampleRate = sr;

    const int numSamples = juce::jmin (dry.getNumSamples(), wet.getNumSamples());
    const int fftOrder = 14;  // 16384-point FFT for good frequency resolution
    const int fftSize = 1 << fftOrder;

    std::vector<Point> points;

    // Use channel 0 for analysis (both dry and wet are mono-compatible)
    const float* dryData = dry.getReadPointer (0);
    const float* wetData = wet.getReadPointer (0);

    processChannel (dryData, wetData, numSamples, sr, fftOrder, points);

    result.raw = points;

    // Apply smoothing (shared with the MLS deconvolution path)
    applySmoothing (points, sr, fftSize, result);

    return result;
}

void FreqResponse::applySmoothing (const std::vector<Point>& raw, double sr, int fftSize,
                                   Result& result) const
{
    if (raw.empty())
        return;
    std::vector<float> mags (raw.size());
    std::vector<float> freqs (raw.size());
    for (size_t i = 0; i < raw.size(); ++i)
    {
        mags[i] = std::pow (10.0f, static_cast<float> (raw[i].magnitudeDB / 20.0));
        freqs[i] = static_cast<float> (raw[i].frequency);
    }
    const double freqStep = sr / fftSize;
    auto mags12 = mags;
    MathUtils::smoothOctave (mags12.data(), freqStep, (int) mags12.size(), 12);
    for (size_t i = 0; i < raw.size(); ++i)
    {
        Point p;
        p.frequency = raw[i].frequency;
        p.magnitudeDB = MathUtils::amplitudeToDB (mags12[i]);
        p.phaseDeg = raw[i].phaseDeg;
        result.smoothed_1_12.push_back (p);
    }
    auto mags3 = mags;
    MathUtils::smoothOctave (mags3.data(), freqStep, (int) mags3.size(), 3);
    for (size_t i = 0; i < raw.size(); ++i)
    {
        Point p;
        p.frequency = raw[i].frequency;
        p.magnitudeDB = MathUtils::amplitudeToDB (mags3[i]);
        p.phaseDeg = raw[i].phaseDeg;
        result.smoothed_1_3.push_back (p);
    }
}

FreqResponse::Result FreqResponse::analyzeMLS (const juce::AudioBuffer<float>& dry,
                                               const juce::AudioBuffer<float>& wet,
                                               double sr, int mlsLength)
{
    Result result;
    result.sampleRate = sr;

    // Steady-state window: when the recording holds two periods (period +
    // tail — as the interface documents), analyse the SECOND period, whose
    // wet signal is transient-free (the plugin warm-up only contaminates the
    // first ~t samples); a single-period recording analyses what it has.
    const int numSamples = juce::jmin (dry.getNumSamples(), wet.getNumSamples(),
                                       mlsLength * 2);
    const int startSample = (numSamples >= mlsLength * 2) ? mlsLength : 0;
    const float* dryData = dry.getReadPointer (0);
    const float* wetData = wet.getReadPointer (0);

    // H(f) = Y(f) / X(f) evaluated at the EXACT MLS harmonics f_q = q·sr/N.
    //
    // A period-N MLS carries all its energy at those frequencies, and no
    // power-of-two FFT bin coincides with them (32768 != 2·16383), so an
    // FFT-based division samples the Dirichlet interpolation of the comb:
    // the bins drift up to ~0.4 bins from the harmonics at 10 kHz, where the
    // dry spectrum dips to ~45% and the estimate loses up to ~0.5 dB in the
    // rolloff. Direct summation over the (steady-state) period is exact; the
    // twiddle recursion keeps it cheap (~7k harmonics × N samples).
    const double freqStep = sr / mlsLength;
    const int qLo = juce::jmax (1, static_cast<int> (std::ceil (20.0 * mlsLength / sr)));
    const int qHi = static_cast<int> (std::floor (20000.0 * mlsLength / sr));
    const double lowEnergyThreshold = static_cast<double> (mlsLength) * 1e-8;

    // Clamp the DFT window to the samples actually available: a recording
    // shorter than one full period analyses what it has instead of reading
    // past the buffer end (review fix).
    const int dftLength = juce::jmin (mlsLength, numSamples - startSample);
    std::vector<Point> points;
    points.reserve (static_cast<size_t> (qHi - qLo + 1));

    for (int q = qLo; q <= qHi; ++q)
    {
        const double freq = q * freqStep;
        const double rotArg = -2.0 * juce::MathConstants<double>::pi * q / mlsLength;
        const std::complex<double> rot (std::cos (rotArg), std::sin (rotArg));
        std::complex<double> tw (1.0, 0.0);
        std::complex<double> X (0.0, 0.0), Y (0.0, 0.0);
        for (int n = 0; n < dftLength; ++n)
        {
            X += static_cast<double> (dryData[startSample + n]) * tw;
            Y += static_cast<double> (wetData[startSample + n]) * tw;
            tw *= rot;
        }

        const double energy = X.real() * X.real() + X.imag() * X.imag();
        if (energy < lowEnergyThreshold)
            continue;

        const std::complex<double> H = Y / X;
        Point p;
        p.frequency   = freq;
        p.magnitudeDB = 20.0 * std::log10 (std::max (std::abs (H), 1e-15));
        p.phaseDeg    = std::atan2 (H.imag(), H.real())
                        * 180.0 / juce::MathConstants<double>::pi;
        points.push_back (p);
    }

    applyPhasePost (points, sr);
    result.raw = points;
    // Points are spaced sr/mlsLength apart (one per harmonic) — pass the
    // period as the "FFT size" so the octave smoothing places them correctly.
    applySmoothing (points, sr, mlsLength, result);
    return result;
}

void FreqResponse::processChannel (
    const float* dryData,
    const float* wetData,
    int numSamples,
    double sampleRate,
    int fftOrder,
    std::vector<Point>& points)
{
    const int fftSize = 1 << fftOrder;
    const int hopSize = fftSize / 4;
    const int numBins = fftSize / 2 + 1;
    const double freqStep = sampleRate / fftSize;

    FftHelper fft (fftOrder);
    std::vector<float> dryWindow (fftSize);
    std::vector<float> wetWindow (fftSize);
    std::vector<float> dryReal (numBins), dryImag (numBins);
    std::vector<float> wetReal (numBins), wetImag (numBins);

    // ── H1 accumulators ──
    // Sxx = Σ |X|² (autospectrum of dry, real-valued)
    // Sxy = Σ conj(X) * Y (cross-spectrum, complex-valued)
    std::vector<double> sxx (numBins, 0.0);
    std::vector<std::complex<double>> sxy (numBins, {0.0, 0.0});
    int windowCount = 0;

    for (int pos = 0; pos + fftSize <= numSamples; pos += hopSize)
    {
        // Copy samples into window buffers
        for (int i = 0; i < fftSize; ++i)
        {
            dryWindow[i] = dryData[pos + i];
            wetWindow[i] = wetData[pos + i];
        }

        // Apply Hann window (amplitude correction handled by H1 ratio)
        FftHelper::applyHannWindow (dryWindow.data(), fftSize);
        FftHelper::applyHannWindow (wetWindow.data(), fftSize);

        // Forward FFT (window already applied, pass false)
        fft.forwardReal (dryWindow.data(), dryReal.data(), dryImag.data(), false);
        fft.forwardReal (wetWindow.data(), wetReal.data(), wetImag.data(), false);

        // Accumulate spectral densities
        for (int bin = 0; bin < numBins; ++bin)
        {
            const double Xr = static_cast<double> (dryReal[bin]);
            const double Xi = static_cast<double> (dryImag[bin]);
            const double Yr = static_cast<double> (wetReal[bin]);
            const double Yi = static_cast<double> (wetImag[bin]);

            // |X|² = Xr² + Xi²
            sxx[bin] += Xr * Xr + Xi * Xi;

            // conj(X) * Y = (Xr - j·Xi) * (Yr + j·Yi)
            //            = (Xr·Yr + Xi·Yi) + j·(Xr·Yi - Xi·Yr)
            sxy[bin] += std::complex<double> (Xr * Yr + Xi * Yi,
                                              Xr * Yi - Xi * Yr);
        }
        ++windowCount;
    }

    if (windowCount == 0)
        return;

    // ── Compute H1 = Sxy / Sxx per bin ──
    const double lowEnergyThreshold = static_cast<double> (windowCount) * 1e-8;

    for (int bin = 0; bin < numBins; ++bin)
    {
        const double freq = bin * freqStep;

        // Frequency range constraint
        if (freq < 20.0 || freq > 20000.0)
            continue;

        // Low-energy protection
        if (sxx[bin] < lowEnergyThreshold)
            continue;

        const std::complex<double> H = sxy[bin] / sxx[bin];

        Point p;
        p.frequency  = freq;
        p.magnitudeDB = 20.0 * std::log10 (std::max (std::abs (H), 1e-15));
        p.phaseDeg   = std::atan2 (H.imag(), H.real())
                       * 180.0 / juce::MathConstants<double>::pi;

        points.push_back (p);
    }

    // ── Phase unwrapping across frequency ──
    // atan2 wraps to [-π, π]; unwrap removes ±2π discontinuities
    // so the phase curve is continuous across frequency bins.
    // Latency compensation follows (shared with the MLS path).
    applyPhasePost (points, sampleRate);
}

void FreqResponse::applyPhasePost (std::vector<Point>& points, double sampleRate) const
{
    if (points.size() >= 2)
    {
        double cumulativeOffset = 0.0;
        double prevRaw = points[0].phaseDeg;  // wrapped reference
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

    // ── Latency compensation ──
    // Plugin latency introduces a linear phase ramp:
    //   φ_latency(f) = -2π · f · N / Fs
    // where N = latency in samples, Fs = sample rate.
    // Subtract it so the residual phase represents the filter alone.
    if (latencySamples > 0)
    {
        const double coeff = 360.0 * static_cast<double> (latencySamples) / sampleRate;
        for (auto& p : points)
            p.phaseDeg += p.frequency * coeff;  // subtract negative = add
    }
}
