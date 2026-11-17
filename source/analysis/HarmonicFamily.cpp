#include "HarmonicFamily.h"

#include <atomic>
#include <cmath>
#include <utility>

namespace
{

/** Process-global cancellation flag: one family measurement at a time (same
 *  convention as CompressionFamily — the host runs a single measurement). */
std::atomic<bool> g_cancelled { false };

/** Scope guard: restore the session's harmonic amplitude state on every exit
 *  path of measure(). The session is reused by every IPC command, so a leaked
 *  per-cell override would silently change the next harmonic measurement (and
 *  break the byte-compatible default path). */
class ScopedHarmonicAmplitude
{
public:
    explicit ScopedHarmonicAmplitude (MeasurementSession& sessionToGuard)
        : session (sessionToGuard),
          hadOverride (sessionToGuard.isHarmonicAmplitudeSet()),
          previousDB (sessionToGuard.getHarmonicAmplitudeDB())
    {
    }

    ~ScopedHarmonicAmplitude()
    {
        if (hadOverride)
            session.setHarmonicAmplitudeDB (previousDB);
        else
            session.clearHarmonicAmplitudeOverride();
    }

    ScopedHarmonicAmplitude (const ScopedHarmonicAmplitude&) = delete;
    ScopedHarmonicAmplitude& operator= (const ScopedHarmonicAmplitude&) = delete;

private:
    MeasurementSession& session;
    bool hadOverride;
    double previousDB;
};

} // namespace

//==============================================================================

HarmonicFamily::FamilyResult HarmonicFamily::measure (
    MeasurementSession* session,
    const std::vector<double>& levelsDB,
    std::function<void(int, int)> progress)
{
    FamilyResult result;
    result.levelsDB = levelsDB;

    if (session == nullptr || levelsDB.empty())
        return result;

    // The harmonic excitation is a built-in analytical signal; the type selects
    // the single-tone-per-segment analysis (issue #38).
    session->setSource (MeasurementSession::Source::signal);
    session->setMeasurementType (MeasurementSession::Type::harmonicAnalysis);
    session->setHarmonicLevelsDB (levelsDB);
    g_cancelled.store (false);

    // Restores the caller's amplitude state when measure() returns.
    ScopedHarmonicAmplitude amplitudeGuard (*session);

    const int total = static_cast<int> (levelsDB.size());
    int done = 0;

    for (double levelDB : levelsDB)
    {
        // Cancellation takes effect at level boundaries.
        if (g_cancelled.load())
        {
            result.cancelled = true;
            break;
        }

        // Per-cell input level: amplitude is derived from dBFS inside run().
        session->setHarmonicAmplitudeDB (levelDB);

        // One entry per requested level, even when the run fails: a silently
        // shorter array would hide which level produced no data (the export
        // carries valid=false for it instead).
        FamilyEntry entry;
        entry.inputLevelDB = levelDB;

        if (session->run())
        {
            const auto& capture = session->getResult();

            HarmonicAnalysis analyzer;
            auto analysis = analyzer.analyze (capture.getWetBuffer(),
                                              capture.getSampleRate(),
                                              session->getFundamentalFreqs(),
                                              session->getSegmentDurationSec());

            entry.tones = std::move (analysis.tones);
            entry.valid = ! entry.tones.empty();
        }

        result.entries.push_back (std::move (entry));

        ++done;
        if (progress)
            progress (done, total);
    }

    return result;
}

//==============================================================================

void HarmonicFamily::cancel()
{
    g_cancelled.store (true);
}
