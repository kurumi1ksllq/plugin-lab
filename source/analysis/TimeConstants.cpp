#include "TimeConstants.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace
{
// ln(0.9/0.1) = ln(9): the 10%-90% transition time of a single pole equals
// tau * ln(9), so tau = edgeTime / ln(9).
constexpr double kEdgeToTauFactor = 2.1972245773362196;

// GR_ss below this (dB) is treated as "no compression": no valid estimate.
constexpr double kMinSteadyGRDB = 0.01;

// The log-domain fit is restricted to |ln(arg)| <= this bound. Near steady
// state the log argument collapses to the numerical noise floor and the
// resulting huge |y| values would dominate the least-squares fit.
constexpr double kMaxLogFitAbs = 4.0;

// |d(GR)/dt| below this (dB/s) is treated as numerical steady state: the
// derivative carries no signal, so the instantaneous tau is undefined there.
constexpr double kMinDerivativeDBSec = 1e-9;

// Default number of level bins for the tau(level) curve family.
constexpr int kDefaultNumBins = 10;

bool isAttackEdge (const char* edge)
{
    return std::strcmp (edge, "attack") == 0;
}

bool isReleaseEdge (const char* edge)
{
    return std::strcmp (edge, "release") == 0;
}

/**
 * Points of the timeline whose timeSec falls inside [startSample, endSample]
 * (sample indices relative to the timeline start). The end bound is inclusive
 * and intentionally not clamped: an end marker beyond the last point simply
 * keeps every remaining point.
 */
std::vector<GainReduction::Point> extractInterval (const GainReduction::Result& gr,
                                                   int64_t startSample,
                                                   int64_t endSample,
                                                   double sr)
{
    std::vector<GainReduction::Point> pts;
    const double t0 = static_cast<double> (startSample) / sr;
    const double t1 = static_cast<double> (endSample) / sr;
    for (const auto& p : gr.timeline)
        if (p.timeSec >= t0 && p.timeSec <= t1)
            pts.push_back (p);
    return pts;
}

/**
 * Steady-state GR of an edge: mean over the last 10% of the points for an
 * attack edge (the steady segment before the edge ends) or the first 10% for
 * a release edge (the steady segment where the decay starts).
 */
double steadyStateGR (const std::vector<GainReduction::Point>& points, bool attack)
{
    const size_t n = points.size();
    if (n == 0)
        return 0.0;

    size_t start = 0;
    size_t end = n;
    if (attack)
        start = (n * 9) / 10;
    else
        end = std::max<size_t> (1, n / 10);

    double sum = 0.0;
    size_t count = 0;
    for (size_t i = start; i < end; ++i)
    {
        sum += points[i].grDB;
        ++count;
    }
    return count > 0 ? sum / static_cast<double> (count) : 0.0;
}

/**
 * End sample of the release edge: releaseEnd when it extends past
 * releaseStart, otherwise the end of the timeline.
 */
int64_t releaseEndSample (const TimeConstants::EventMarkers& markers)
{
    if (markers.releaseEnd > markers.releaseStart)
        return markers.releaseEnd;
    return std::numeric_limits<int64_t>::max();
}
} // namespace

//==============================================================================

TimeConstants::Result TimeConstants::estimate (const GainReduction::Result& gr,
                                               const EventMarkers& markers,
                                               double sr)
{
    Result result;
    if (gr.timeline.empty() || sr <= 0.0)
        return result;

    // Attack edge: present iff attackEnd > 0 (attackStart may be 0).
    if (markers.attackEnd > 0)
    {
        const auto attackPts = extractInterval (gr, markers.attackStart, markers.attackEnd, sr);
        if (! attackPts.empty())
        {
            // Primary estimate: log-domain least-squares fit.
            result.tauAttackSec = fitTau (attackPts, "attack", sr);
            if (result.tauAttackSec <= 0.0)
            {
                // Fallback: 10%-90% edge estimate when the fit is impossible.
                const double tauEdge = edgeTime (gr, markers, "attack", sr) / kEdgeToTauFactor;
                result.tauAttackSec = (tauEdge > 0.0) ? tauEdge : 0.0;
            }
        }
        result.attackByLevel = instantaneousTau (gr, markers, "attack", sr, kDefaultNumBins);
    }

    // Release edge: present iff releaseStart > 0; runs to the end of the
    // timeline when releaseEnd does not extend past releaseStart.
    if (markers.releaseStart > 0)
    {
        const auto releasePts = extractInterval (gr, markers.releaseStart,
                                                 releaseEndSample (markers), sr);
        if (! releasePts.empty())
        {
            result.tauReleaseSec = fitTau (releasePts, "release", sr);
            if (result.tauReleaseSec <= 0.0)
            {
                const double tauEdge = edgeTime (gr, markers, "release", sr) / kEdgeToTauFactor;
                result.tauReleaseSec = (tauEdge > 0.0) ? tauEdge : 0.0;
            }
        }
        result.releaseByLevel = instantaneousTau (gr, markers, "release", sr, kDefaultNumBins);
    }

    result.valid = (result.tauAttackSec > 0.0 || result.tauReleaseSec > 0.0);
    return result;
}

//==============================================================================

double TimeConstants::edgeTime (const GainReduction::Result& gr,
                                const EventMarkers& markers,
                                const char* edge, double sr)
{
    if (sr <= 0.0 || gr.timeline.size() < 2)
        return 0.0;

    const bool attack = isAttackEdge (edge);
    if (! attack && ! isReleaseEdge (edge))
        return 0.0;

    int64_t startSample = 0;
    int64_t endSample = 0;
    if (attack)
    {
        if (markers.attackEnd <= 0)
            return 0.0;
        startSample = markers.attackStart;
        endSample = markers.attackEnd;
    }
    else
    {
        if (markers.releaseStart <= 0)
            return 0.0;
        startSample = markers.releaseStart;
        endSample = releaseEndSample (markers);
    }

    const auto pts = extractInterval (gr, startSample, endSample, sr);
    if (pts.size() < 2)
        return 0.0;

    double grMin = pts.front().grDB;
    double grMax = pts.front().grDB;
    for (const auto& p : pts)
    {
        grMin = std::min (grMin, p.grDB);
        grMax = std::max (grMax, p.grDB);
    }
    const double range = grMax - grMin;
    if (range < 1e-9)
        return 0.0;  // flat edge: nothing to measure

    // 10% and 90% thresholds of the full edge swing.
    const double low = grMin + 0.1 * range;
    const double high = grMin + 0.9 * range;

    bool found10 = false;
    bool found90 = false;
    double t10 = 0.0;
    double t90 = 0.0;
    for (const auto& p : pts)
    {
        if (attack)
        {
            // Rising edge: cross upward through the 10% then the 90% level.
            if (! found10 && p.grDB >= low)
            {
                t10 = p.timeSec;
                found10 = true;
            }
            if (! found90 && p.grDB >= high)
            {
                t90 = p.timeSec;
                found90 = true;
            }
        }
        else
        {
            // Falling edge: cross downward through the 90% then the 10% level.
            if (! found10 && p.grDB <= high)
            {
                t10 = p.timeSec;
                found10 = true;
            }
            if (! found90 && p.grDB <= low)
            {
                t90 = p.timeSec;
                found90 = true;
            }
        }
    }

    if (! found10 || ! found90)
        return 0.0;  // edge did not traverse the full 10%-90% swing in-interval

    return t90 - t10;
}

//==============================================================================

double TimeConstants::fitTau (const std::vector<GainReduction::Point>& points,
                              const char* edge, double sr)
{
    if (sr <= 0.0 || points.size() < 2)
        return 0.0;

    const bool attack = isAttackEdge (edge);
    if (! attack && ! isReleaseEdge (edge))
        return 0.0;

    const double grSS = steadyStateGR (points, attack);
    if (grSS < kMinSteadyGRDB)
        return 0.0;  // no compression: GR_ss ~ 0

    // Log-domain linearisation: collect the (t, y) pairs of the edge.
    //   attack : y = ln(1 - GR/GR_ss) = -t/tau
    //   release: y = ln(GR/GR_ss)     = -t/tau
    // Points whose log argument collapsed (GR at/beyond the estimated steady
    // state) or whose |y| is on the steady-state noise floor are excluded.
    std::vector<std::pair<double, double>> fitPoints;
    fitPoints.reserve (points.size());
    for (const auto& p : points)
    {
        const double arg = attack ? (1.0 - p.grDB / grSS) : (p.grDB / grSS);
        if (arg <= 0.0)
            continue;
        const double y = std::log (arg);
        if (std::abs (y) > kMaxLogFitAbs)
            continue;
        fitPoints.emplace_back (p.timeSec, y);
    }

    if (fitPoints.size() < 2)
        return 0.0;

    // Least-squares slope of y vs t.
    const size_t n = fitPoints.size();
    double tBar = 0.0;
    double yBar = 0.0;
    for (const auto& fp : fitPoints)
    {
        tBar += fp.first;
        yBar += fp.second;
    }
    tBar /= static_cast<double> (n);
    yBar /= static_cast<double> (n);

    double sxx = 0.0;
    double sxy = 0.0;
    for (const auto& fp : fitPoints)
    {
        const double dt = fp.first - tBar;
        sxx += dt * dt;
        sxy += dt * (fp.second - yBar);
    }
    if (sxx <= 0.0)
        return 0.0;

    const double slope = sxy / sxx;
    if (slope >= 0.0)
        return 0.0;  // slope must be -1/tau < 0 for a decaying/rising pole

    return -1.0 / slope;
}

//==============================================================================

TimeConstants::TauCurve TimeConstants::instantaneousTau (const GainReduction::Result& gr,
                                                         const EventMarkers& markers,
                                                         const char* edge, double sr,
                                                         int numBins)
{
    TauCurve curve;
    if (sr <= 0.0 || gr.timeline.size() < 3 || numBins <= 0)
        return curve;

    const bool attack = isAttackEdge (edge);
    if (! attack && ! isReleaseEdge (edge))
        return curve;

    int64_t startSample = 0;
    int64_t endSample = 0;
    if (attack)
    {
        if (markers.attackEnd <= 0)
            return curve;
        startSample = markers.attackStart;
        endSample = markers.attackEnd;
    }
    else
    {
        if (markers.releaseStart <= 0)
            return curve;
        startSample = markers.releaseStart;
        endSample = releaseEndSample (markers);
    }

    const auto pts = extractInterval (gr, startSample, endSample, sr);
    if (pts.size() < 3)
        return curve;

    const double grSS = steadyStateGR (pts, attack);
    if (grSS < kMinSteadyGRDB)
        return curve;  // no compression: no tau(level) family

    // Pointwise instantaneous tau via central difference:
    //   attack : tau(t) = (GR_ss - GR) / (dGR/dt)
    //   release: tau(t) = GR / (-dGR/dt)
    // Points at (numerical) steady state — |dGR/dt| below the noise floor —
    // carry no signal and are excluded.
    std::vector<std::pair<double, double>> samples;  // (grDB, tauSec)
    samples.reserve (pts.size());
    for (size_t i = 1; i + 1 < pts.size(); ++i)
    {
        const double dt = pts[i + 1].timeSec - pts[i - 1].timeSec;
        if (dt <= 0.0)
            continue;
        const double dgrdt = (pts[i + 1].grDB - pts[i - 1].grDB) / dt;

        double tau = 0.0;
        if (attack)
        {
            if (dgrdt >= kMinDerivativeDBSec)
                tau = (grSS - pts[i].grDB) / dgrdt;
        }
        else
        {
            if (dgrdt <= -kMinDerivativeDBSec)
                tau = pts[i].grDB / (-dgrdt);
        }

        if (tau > 0.0 && std::isfinite (tau))
            samples.emplace_back (pts[i].grDB, tau);
    }

    if (samples.empty())
        return curve;

    // Level axis: bins over the edge's GR swing; levelDB = mean GR_dB of the
    // bin's points (bin centre when the bin is empty, keeping the axis
    // strictly increasing); tauSec = mean instantaneous tau in the bin.
    double grMin = samples.front().first;
    double grMax = samples.front().first;
    for (const auto& s : samples)
    {
        grMin = std::min (grMin, s.first);
        grMax = std::max (grMax, s.first);
    }

    curve.levelDB.assign (static_cast<size_t> (numBins), 0.0);
    curve.tauSec.assign (static_cast<size_t> (numBins), 0.0);

    const double width = (grMax - grMin) / static_cast<double> (numBins);
    if (width <= 0.0)
    {
        // Degenerate: every sample at one level. Keep the numBins structure
        // with all samples in the first bin.
        double sumTau = 0.0;
        for (const auto& s : samples)
            sumTau += s.second;
        curve.levelDB[0] = grMin;
        curve.tauSec[0] = sumTau / static_cast<double> (samples.size());
        for (int b = 1; b < numBins; ++b)
            curve.levelDB[static_cast<size_t> (b)] = grMin;
        return curve;
    }

    std::vector<double> sumTau (static_cast<size_t> (numBins), 0.0);
    std::vector<double> sumGR (static_cast<size_t> (numBins), 0.0);
    std::vector<size_t> count (static_cast<size_t> (numBins), 0);

    for (const auto& s : samples)
    {
        int bin = static_cast<int> ((s.first - grMin) / width);
        bin = std::clamp (bin, 0, numBins - 1);
        const size_t b = static_cast<size_t> (bin);
        sumTau[b] += s.second;
        sumGR[b] += s.first;
        ++count[b];
    }

    for (int b = 0; b < numBins; ++b)
    {
        const size_t ub = static_cast<size_t> (b);
        if (count[ub] > 0)
        {
            curve.levelDB[ub] = sumGR[ub] / static_cast<double> (count[ub]);
            curve.tauSec[ub] = sumTau[ub] / static_cast<double> (count[ub]);
        }
        else
        {
            curve.levelDB[ub] = grMin + (static_cast<double> (b) + 0.5) * width;
        }
    }

    return curve;
}
