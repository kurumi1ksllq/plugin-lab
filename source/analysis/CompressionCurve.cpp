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
    int burstAnalysisLen = samplesPerBurst / 2;  // Use middle half of each burst
    int burstStartOffset = samplesPerBurst / 4;

    for (int b = 0; b < numBursts; ++b)
    {
        int start = b * samplesPerBurst + burstStartOffset;
        int length = burstAnalysisLen;

        if (start + length > numSamples)
        {
            length = numSamples - start;
            if (length <= 0) break;
        }

        double dryRMS = measureRMS (dryData, start, length);
        double wetRMS = measureRMS (wetData, start, length);

        Point p;
        p.inputDB = MathUtils::amplitudeToDB (dryRMS);
        p.outputDB = MathUtils::amplitudeToDB (wetRMS);
        p.gainReductionDB = p.outputDB - p.inputDB;

        result.curve.push_back (p);
    }

    // Fit compression parameters
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

    // Simple ratio estimation from points above threshold
    double sumInputAbove = 0.0, sumOutputAbove = 0.0;
    int countAbove = 0;

    for (auto& p : curve)
    {
        if (p.inputDB > thresholdDB && p.gainReductionDB < -0.5)
        {
            sumInputAbove += p.inputDB;
            sumOutputAbove += p.outputDB;
            countAbove++;
        }
    }

    if (countAbove > 0)
    {
        double avgInput = sumInputAbove / countAbove;
        double avgOutput = sumOutputAbove / countAbove;
        if (avgInput > avgOutput && std::abs (avgInput - thresholdDB) > 0.1)
            params.ratio = (avgInput - thresholdDB) / (avgOutput - (avgInput - (avgInput - avgOutput)));
        if (params.ratio < 1.0) params.ratio = 1.0;
    }

    params.kneeDB = 3.0;  // Default knee; more sophisticated fitting later

    return params;
}
