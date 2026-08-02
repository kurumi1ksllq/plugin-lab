#include "CompressionCurve.h"
#include "../utils/MathUtils.h"
#include <cmath>

CompressionCurve::Result CompressionCurve::analyze (
    const juce::AudioBuffer<float>& dry,
    const juce::AudioBuffer<float>& wet,
                    double /*sr*/,
                    const std::vector<double>& inputLevelsDB)
{
    Result result;
    const float* dryData = dry.getReadPointer (0);
    const float* wetData = wet.getReadPointer (0);
    const int numSamples = juce::jmin (dry.getNumSamples(), wet.getNumSamples());

    // Assume bursts are evenly spaced through the recording
    int numBursts = (int) inputLevelsDB.size();
    if (numBursts == 0) return result;

    int samplesPerBurst = numSamples / numBursts;

    for (int b = 0; b < numBursts; ++b)
    {
        int segStart = b * samplesPerBurst;
        int segLen   = juce::jmin (samplesPerBurst, numSamples - segStart);
        if (segLen <= 0) break;

        // The tone burst occupies only part of each segment (ToneBurst uses a
        // 25% duty cycle at the segment start). Hunt for the loudest
        // quarter-length window within the segment — the burst itself — so the
        // measurement is robust to the exact duty cycle and any plugin delay.
        int windowLen = segLen / 4;
        if (windowLen <= 0) break;

        int bestStart = segStart;
        double bestRMS = 0.0;
        const int step = juce::jmax (1, windowLen / 2);

        for (int off = 0; off + windowLen <= segLen; off += step)
        {
            double rms = measureRMS (dryData, segStart + off, windowLen);
            if (rms > bestRMS)
            {
                bestRMS = rms;
                bestStart = segStart + off;
            }
        }

        double dryRMS = bestRMS;
        double wetRMS = measureRMS (wetData, bestStart, windowLen);

        Point p;
        p.inputDB = MathUtils::amplitudeToDB (dryRMS);
        p.outputDB = MathUtils::amplitudeToDB (wetRMS);
        p.gainReductionDB = p.outputDB - p.inputDB;

        result.curve.push_back (p);
    }
    result.fitted = fitParams (result.curve);

    return result;
}

double CompressionCurve::measureRMS (const float* data,
                                      int startSample,
                                      int numSamples)
{
    double sumSq = 0.0;
    for (int i = 0; i < numSamples; ++i)
    {
        double s = data[startSample + i];
        sumSq += s * s;
    }

    return (numSamples > 0) ? std::sqrt (sumSq / numSamples) : 0.0;
}

CompressionCurve::FittedParams CompressionCurve::fitParams (
    const std::vector<Point>& curve)
{
    FittedParams params;
    if (curve.size() < 4) return params;

    // Simple threshold detection: find where GR exceeds 1dB
    double thresholdDB = 0.0;
    for (auto& p : curve)
    {
        if (p.gainReductionDB < -0.5)  // at least 0.5dB of gain reduction
        {
            thresholdDB = p.inputDB;
            break;
        }
    }
    params.thresholdDB = thresholdDB;

    // Ratio estimation from points above threshold: least-squares slope of
    // output vs input, converted to a ratio (dIn / dOut). Always finite and
    // clamped to >= 1:1.
    std::vector<const Point*> above;
    for (auto& p : curve)
    {
        if (p.inputDB > thresholdDB && p.gainReductionDB < -0.5)
            above.push_back (&p);
    }

    if (above.size() >= 2)
    {
        double meanX = 0.0, meanY = 0.0;
        for (auto* p : above) { meanX += p->inputDB;  meanY += p->outputDB; }
        meanX /= static_cast<double> (above.size());
        meanY /= static_cast<double> (above.size());

        double covXY = 0.0, varX = 0.0;
        for (auto* p : above)
        {
            const double dx = p->inputDB - meanX;
            const double dy = p->outputDB - meanY;
            covXY += dx * dy;
            varX  += dx * dx;
        }

        if (varX > 1e-6 && covXY > 0.0)
        {
            params.ratio = varX / covXY;  // slope⁻¹ = compression ratio
            if (params.ratio < 1.0) params.ratio = 1.0;
        }
    }

    params.kneeDB = 3.0;  // Default knee; more sophisticated fitting later

    return params;
}
