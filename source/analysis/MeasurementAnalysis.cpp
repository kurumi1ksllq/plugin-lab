#include "MeasurementAnalysis.h"

namespace MeasurementAnalysis
{

SignalResult analyzeByType (MeasurementSession& session, int latencySamples)
{
    SignalResult result;

    auto& recorded = session.getResult();

    // The mapping: each measurement type runs the one analyzer it is
    // measured with, configured from the session's excitation settings.
    // Behaviour copied verbatim from the pre-refactor inline switches in
    // CommandParser::runAndAnalyze and ScanEngine::run (issue #42).
    switch (session.getType())
    {
        case MeasurementSession::Type::frequencyResponse:
        {
            FreqResponse analyser;
            analyser.setLatencySamples (latencySamples);
            if (session.getFreqExcitation())
                result.freq = analyser.analyzeMLS (recorded.getDryBuffer(),
                                                   recorded.getWetBuffer(),
                                                   recorded.getSampleRate(),
                                                   session.getFreqMLSLength());
            else
                result.freq = analyser.analyze (recorded.getDryBuffer(),
                                                recorded.getWetBuffer(),
                                                recorded.getSampleRate());
            break;
        }

        case MeasurementSession::Type::harmonicAnalysis:
        {
            HarmonicAnalysis analyser;
            result.harmonic = analyser.analyze (recorded.getWetBuffer(),
                                                recorded.getSampleRate(),
                                                session.getFundamentalFreqs(),
                                                session.getSegmentDurationSec());
            break;
        }

        case MeasurementSession::Type::compressionCurve:
        {
            CompressionCurve analyser;
            result.compression = analyser.analyze (recorded.getDryBuffer(),
                                                   recorded.getWetBuffer(),
                                                   recorded.getSampleRate(),
                                                   session.getInputLevelsDB());
            break;
        }

        // Not a signal-source measurement (the parsers reject it for
        // Source::signal); the empty result keeps the enum switch
        // exhaustive — ScanEngine's old switch broke early here, which
        // produced the same all-empty entry.
        case MeasurementSession::Type::grTimeline:
            break;
    }

    return result;
}

} // namespace MeasurementAnalysis
