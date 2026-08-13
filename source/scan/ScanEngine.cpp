#include "ScanEngine.h"

//==============================================================================
// Configuration
//==============================================================================

void ScanEngine::setPluginInstance (juce::AudioPluginInstance* plugin)
{
    plugin_ = plugin;
}

void ScanEngine::setSession (MeasurementSession* session)
{
    session_ = session;
}

void ScanEngine::cancel()
{
    cancelled_.store (true);
}

//==============================================================================
// Scan loop
//==============================================================================

namespace
{

/** Snapshot of every parameter's normalized value, for later restoration. */
std::vector<float> snapshotParams (juce::AudioPluginInstance* plugin)
{
    std::vector<float> saved;
    auto& params = plugin->getParameters();
    saved.reserve (static_cast<size_t> (params.size()));
    for (auto* param : params)
        saved.push_back (param->getValue());
    return saved;
}

/** Restore the saved values via setValueNotifyingHost (listeners notified). */
void restoreParams (juce::AudioPluginInstance* plugin, const std::vector<float>& saved)
{
    auto& params = plugin->getParameters();
    const size_t numToRestore = std::min (saved.size(), static_cast<size_t> (params.size()));
    for (size_t i = 0; i < numToRestore; ++i)
        params[static_cast<int> (i)]->setValueNotifyingHost (saved[i]);
}

} // namespace

ScanEngine::ScanResult ScanEngine::run (const juce::String& paramId,
                                        const std::vector<float>& values,
                                        MeasurementSession::Type type,
                                        std::function<void(int, int)> progress)
{
    ScanResult result;
    result.paramId = paramId;

    if (plugin_ == nullptr || session_ == nullptr)
        return result;

    // Locate the scanned parameter by ID (IDs are stable across versions).
    // Every parameter of an AudioPluginInstance is HostedAudioProcessorParameter
    // (or LegacyAudioParameter, which also derives from it), so the ID is
    // available via dynamic_cast.
    juce::AudioProcessorParameter* param = nullptr;
    auto& params = plugin_->getParameters();
    for (auto* candidate : params)
    {
        auto* hosted = dynamic_cast<juce::HostedAudioProcessorParameter*> (candidate);
        if (hosted != nullptr && hosted->getParameterID() == paramId)
        {
            param = candidate;
            break;
        }
    }
    if (param == nullptr)
        return result;

    result.paramName = param->getName (64);
    result.values.assign (values.begin(), values.end());

    // RAII guard: restore every parameter on exit — covers cancellation
    // (including mid-round) and any exception thrown while running/analysing.
    struct ParamGuard
    {
        juce::AudioPluginInstance* plugin;
        std::vector<float> saved;

        ~ParamGuard()
        {
            restoreParams (plugin, saved);
        }
    } guard { plugin_, snapshotParams (plugin_) };

    cancelled_.store (false);

    const int totalRounds = static_cast<int> (values.size());
    for (int round = 0; round < totalRounds; ++round)
    {
        // Cancellation takes effect at round boundaries.
        if (cancelled_.load())
        {
            result.cancelled = true;
            break;
        }

        param->setValueNotifyingHost (values[round]);

        // Let the host process the parameter change; only needed when the
        // scan itself runs on the message thread (CommandParser pattern).
        if (juce::MessageManager::getInstance()->isThisTheMessageThread())
            juce::MessageManager::getInstance()->runDispatchLoopUntil (20);

        session_->setMeasurementType (type);
        if (! session_->run())
            break;  // measurement failed — keep the rounds completed so far

        // Build the family entry: analyse the recorded result for this round,
        // then re-read the plugin latency (it may have changed with the value).
        ScanResultEntry entry;
        entry.paramValue      = static_cast<double> (values[round]);
        entry.paramValueText  = param->getText (values[round], 64);

        auto& recorded = session_->getResult();
        switch (type)
        {
            case MeasurementSession::Type::frequencyResponse:
            {
                FreqResponse analyser;
                analyser.setLatencySamples (plugin_->getLatencySamples());
                if (session_->getFreqExcitation())
                    entry.freq = analyser.analyzeMLS (recorded.getDryBuffer(),
                                                      recorded.getWetBuffer(),
                                                      recorded.getSampleRate(),
                                                      session_->getFreqMLSLength());
                else
                    entry.freq = analyser.analyze (recorded.getDryBuffer(),
                                                   recorded.getWetBuffer(),
                                                   recorded.getSampleRate());
                break;
            }
            case MeasurementSession::Type::harmonicAnalysis:
            {
                HarmonicAnalysis analyser;
                entry.harmonic = analyser.analyze (recorded.getWetBuffer(),
                                                   recorded.getSampleRate(),
                                                   session_->getFundamentalFreqs(),
                                                   session_->getSegmentDurationSec());
                break;
            }
            case MeasurementSession::Type::compressionCurve:
            {
                CompressionCurve analyser;
                entry.compression = analyser.analyze (recorded.getDryBuffer(),
                                                      recorded.getWetBuffer(),
                                                      recorded.getSampleRate(),
                                                      session_->getInputLevelsDB());
                break;
            }

            // The scan command rejects gr_timeline (GR timelines are
            // measured via the measure command) — defensive case to keep
            // the enum switch exhaustive.
            case MeasurementSession::Type::grTimeline:
                break;
        }

        entry.latencySamples = plugin_->getLatencySamples();

        result.family.push_back (std::move (entry));

        if (progress)
            progress (round + 1, totalRounds);
    }

    return result;
}
