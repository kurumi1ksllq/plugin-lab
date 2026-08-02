#include "CompressionFamily.h"

#include <algorithm>
#include <atomic>
#include <cmath>

namespace
{

//==============================================================================
// Measurement configuration for every cell.
//==============================================================================

/** Sweep start frequency of the dynamic carrier (Hz). A high start keeps the
 *  attack edge away from the low-frequency carrier wobble that would pollute
 *  the GR timeline (the detector is sample-instantaneous). */
constexpr double kCarrierStartHz = 10000.0;

/** Envelope ADSR times (envelope-time seconds). The attack/decay are kept
 *  short so the GR attack edge tracks the compressor's own pole; the sustain
 *  keeps the input in compression for the static curve point. */
constexpr double kEnvAttackSec = 0.002;
constexpr double kEnvDecaySec = 0.002;
constexpr double kEnvSustain = 0.8;
constexpr double kEnvReleaseSec = 1.2;

/** GainReduction RMS window: 1 ms gives ~48 samples per point at 48 kHz,
 *  enough resolution to resolve a 5 ms attack edge. */
constexpr double kGRWindowSec = 0.001;

//==============================================================================
// detectMarkers thresholds.
//==============================================================================

/** Peak reduction (dB) below which no compression edge is declared. */
constexpr double kMinCompressionDB = 0.5;

/** Reduction level (dB) at which the release edge is declared finished: below
 *  the threshold the decay is the compressor's own release pole, so the fit
 *  sees a pure exponential from here. */
constexpr double kReleaseFloorDB = 1.0;

//==============================================================================

/** Process-global cancellation flag: one family measurement at a time. */
std::atomic<bool> g_cancelled { false };

} // namespace

//==============================================================================

CompressionFamily::FamilyResult CompressionFamily::measure (
    juce::AudioPluginInstance* plugin,
    MeasurementSession* session,
    const std::vector<double>& levelsDB,
    const std::vector<double>& speeds,
    std::function<void(int, int)> progress)
{
    FamilyResult result;
    result.levelsDB = levelsDB;
    result.speeds = speeds;

    if (plugin == nullptr || session == nullptr || levelsDB.empty() || speeds.empty())
        return result;

    session->setSource (MeasurementSession::Source::dynamic);
    g_cancelled.store (false);

    const int total = static_cast<int> (levelsDB.size() * speeds.size());
    int done = 0;

    for (double levelDB : levelsDB)
    {
        const double amp = std::pow (10.0, levelDB / 20.0);

        for (double speed : speeds)
        {
            // Cancellation takes effect at cell boundaries.
            if (g_cancelled.load())
            {
                result.cancelled = true;
                break;
            }

            FamilyEntry entry;
            entry.inputLevelDB = levelDB;
            entry.speed = speed;

            // Envelope release (envelope-time) clipped so the release ramp
            // starts inside the signal: releaseStart = totalEnv - release
            // with totalEnv = 2 * speed (EnvelopeSignal semantics).
            const double totalEnv = 2.0 * speed;
            const double release = std::min (kEnvReleaseSec, totalEnv - 0.05);

            session->setDynamicAmplitude (amp);
            session->setDynamicSpeed (speed);
            session->setDynamicADSR (kEnvAttackSec, kEnvDecaySec, kEnvSustain, release);
            session->setDynamicCarrierStartHz (kCarrierStartHz);

            if (session->run())
            {
                const auto& dry = session->getResult().getDryBuffer();
                const auto& wet = session->getResult().getWetBuffer();
                const double sr = session->getResult().getSampleRate();
                const int latency = plugin->getLatencySamples();

                // Static curve: one point measured on the sustain plateau.
                CompressionCurve curveAnalyzer;
                entry.curve = curveAnalyzer.analyze (dry, wet, sr, { levelDB });

                // GR timeline. GainReduction reports the wet/dry ratio
                // (negative dB for a compressor); TimeConstants expects the
                // reduction amount (positive dB), so negate the timeline.
                entry.gr = GainReduction::analyze (dry, wet, sr, latency, kGRWindowSec);
                for (auto& p : entry.gr.timeline)
                    p.grDB = -p.grDB;

                const auto markers = detectMarkers (entry.gr);
                entry.tau = TimeConstants::estimate (entry.gr, markers, sr);

                entry.valid = ! entry.gr.timeline.empty()
                              && ! entry.curve.curve.empty()
                              && entry.tau.valid;

                result.entries.push_back (std::move (entry));
            }

            ++done;
            if (progress)
                progress (done, total);
        }

        if (result.cancelled)
            break;
    }

    return result;
}

//==============================================================================

TimeConstants::EventMarkers CompressionFamily::detectMarkers (
    const GainReduction::Result& gr)
{
    TimeConstants::EventMarkers markers;
    const size_t n = gr.timeline.size();
    if (n == 0)
        return markers;

    // Peak reduction (timeline convention: positive dB = reduction).
    double maxGR = gr.timeline[0].grDB;
    for (const auto& p : gr.timeline)
        maxGR = std::max (maxGR, p.grDB);

    if (maxGR < kMinCompressionDB)
        return markers;   // no meaningful compression → no edges

    const double half = 0.5 * maxGR;

    // Attack start: the first rising crossing of the half-maximum reduction.
    size_t attackStartIdx = 0;
    for (size_t i = 1; i < n; ++i)
    {
        if (gr.timeline[i].grDB >= half && gr.timeline[i].grDB > gr.timeline[i - 1].grDB)
        {
            attackStartIdx = i;
            break;
        }
    }

    // Peak: the first index reaching the maximum — the attack edge has
    // converged there (start of the sustain plateau). Used as the attack end
    // so the sustain/release regions cannot pollute the attack fit.
    size_t peakIdx = 0;
    for (size_t i = 1; i < n; ++i)
        if (gr.timeline[i].grDB > gr.timeline[peakIdx].grDB)
            peakIdx = i;

    // Release start: the first point after the peak where the reduction has
    // fallen to the ~1 dB residual — the input is below the threshold there
    // and the decay is the compressor's own release pole.
    size_t releaseStartIdx = 0;
    for (size_t i = peakIdx + 1; i < n; ++i)
    {
        if (gr.timeline[i].grDB <= kReleaseFloorDB)
        {
            releaseStartIdx = i;
            break;
        }
    }

    // EventMarkers are SAMPLE positions relative to the timeline start, while
    // the indices above are array positions into the (windowed) timeline.
    // Convert via each point's time and the timeline sample rate.
    const auto sampleIndex = [&gr] (size_t idx)
    {
        return static_cast<int64_t> (std::llround (gr.timeline[idx].timeSec * gr.sampleRate));
    };

    markers.attackStart = sampleIndex (attackStartIdx);
    markers.attackEnd = sampleIndex (peakIdx);
    markers.releaseStart = (releaseStartIdx > 0) ? sampleIndex (releaseStartIdx) : 0;
    markers.releaseEnd = 0;   // release edge runs to the end of the timeline
    return markers;
}

//==============================================================================

void CompressionFamily::cancel()
{
    g_cancelled.store (true);
}
