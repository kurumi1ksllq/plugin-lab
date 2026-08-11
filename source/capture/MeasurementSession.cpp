#include "MeasurementSession.h"
#include "../signal/SineSweep.h"
#include "../signal/MultiTone.h"
#include "../signal/ToneBurst.h"
#include "../signal/Impulse.h"
#include "../signal/FilePlayback.h"
#include "../signal/NoiseGenerator.h"
#include "../signal/EnvelopeSignal.h"
#include "../utils/MathUtils.h"

namespace
{
// Restores a parameter to its pre-playback value (R2). Reuses the shared
// stable-id lookup on ParameterTimeline (one lookup across capture/).
void restoreParamValue (juce::AudioPluginInstance* plugin, const juce::String& paramId, float value)
{
    if (auto* param = ParameterTimeline::findParam (*plugin, paramId))
        param->setValueNotifyingHost (value);
}
}  // namespace

void MeasurementSession::setPluginInstance (juce::AudioPluginInstance* p)
{
    plugin = p;
}

void MeasurementSession::setPluginDescription (const juce::PluginDescription& desc)
{
    pluginDesc = desc;
}

void MeasurementSession::setMeasurementType (Type t)
{
    type = t;
}

void MeasurementSession::setNoiseConfig (NoiseGenerator::Type t, double durationSec, uint32_t seed)
{
    noiseType = t;
    noiseDuration = durationSec;
    noiseSeed = seed;
}

void MeasurementSession::setDynamicADSR (double attackSec, double decaySec,
                                         double sustain, double releaseSec)
{
    dynamicADSR[0] = attackSec;
    dynamicADSR[1] = decaySec;
    dynamicADSR[2] = sustain;
    dynamicADSR[3] = releaseSec;
}

void MeasurementSession::setTimelinePlayback (std::vector<TimelineEvent> events, double playbackRate)
{
    timelinePlayback.setPlayback (std::move (events), playbackRate);
    timelinePlaybackActive = true;

    // R2: snapshot the current value of every parameter the timeline will
    // touch, so the play run can restore them afterwards.
    timelineRestore.clear();
    if (plugin == nullptr)
        return;

    juce::StringArray seenIds;
    for (const auto& ev : timelinePlayback.getEvents())
    {
        if (seenIds.contains (ev.paramId))
            continue;
        seenIds.add (ev.paramId);

        for (auto* candidate : plugin->getParameters())
        {
            auto* hosted = dynamic_cast<juce::HostedAudioProcessorParameter*> (candidate);
            if (hosted != nullptr && hosted->getParameterID() == ev.paramId)
            {
                timelineRestore.emplace_back (ev.paramId, candidate->getValue());
                break;
            }
        }
    }
}

void MeasurementSession::captureParameterSnapshot()
{
    if (plugin == nullptr)
    {
        paramSnapshot = "{}";
        return;
    }

    juce::String json = "{\n";
    auto& params = plugin->getParameters();

    for (int i = 0; i < params.size(); ++i)
    {
        auto* param = params[i];
        auto value = param->getValue();

        json += "  \"" + param->getName (64) + "\": "
                + juce::String (value, 4);
        if (i < params.size() - 1) json += ",";
        json += "\n";
    }

    json += "}";
    paramSnapshot = json;
}

bool MeasurementSession::run()
{
    // Timeline playback (B2): one-shot — this run consumes the flag even if
    // it fails early, so a stale playback timeline can never leak into a
    // later measurement run.
    const bool timelineActive = timelinePlaybackActive;
    timelinePlaybackActive = false;

    if (plugin == nullptr)
        return false;

    // Prepare the runner
    runner.prepare (sampleRate, blockSize);
    runner.setPlugin (plugin);

    // Create the signal generator for the configured input source.
    // The source decides the signal; Type stays an analysis field (only
    // used on the Source::signal path).
    std::unique_ptr<SignalGenerator> gen;

    switch (source)
    {
        case Source::signal:
        {
            switch (type)
            {
                case Type::frequencyResponse:
                {
                    if (freqExcitationMLS)
                    {
                        auto mls = std::make_unique<Impulse>();
                        mls->useMLS (true);
                        mls->setMLSLength (freqMLSLength);   // 0.34 s @48k vs 5 s sweep
                        mls->setAmplitude (0.5);
                        gen = std::move (mls);
                    }
                    else
                    {
                        auto sweep = std::make_unique<SineSweep>();
                        sweep->setFrequencyRange (20.0, 20000.0);
                        sweep->setDuration (5.0);
                        sweep->setAmplitude (0.5);
                        gen = std::move (sweep);
                    }
                    break;
                }

                case Type::harmonicAnalysis:
                {
                    // Explicit frequency list shared with HarmonicAnalysis: the
                    // analysis must know exactly which fundamentals to look for.
                    static const std::vector<double> kFundamentals =
                        { 100.0, 200.0, 400.0, 800.0, 1600.0, 3200.0, 6400.0, 12800.0 };

                    auto multi = std::make_unique<MultiTone>();
                    multi->setDuration (3.0);
                    multi->setAmplitude (0.4);
                    multi->setFrequencies (kFundamentals);
                    fundamentalFreqs = kFundamentals;
                    gen = std::move (multi);
                    break;
                }

                case Type::compressionCurve:
                {
                    auto bursts = std::make_unique<ToneBurst>();
                    bursts->setFrequency (1000.0);
                    lastLevels = bursts->getLevels();
                    gen = std::move (bursts);
                    break;
                }

                // grTimeline has no signal-source generator — the CommandParser
                // rejects gr_timeline for Source::signal, so this is defensive
                // (also keeps the enum switch exhaustive).
                case Type::grTimeline:
                    return false;
            }
            break;
        }

        case Source::file:
        {
            // The file is opened inside FilePlayback::prepare() (called by the
            // runner); a missing file would fall back to 10 s of silence, so
            // reject it up front instead.
            if (! filePath.existsAsFile())
                return false;

            auto playback = std::make_unique<FilePlayback>();
            playback->setFile (filePath);
            gen = std::move (playback);
            break;
        }

        case Source::noise:
        {
            auto noise = std::make_unique<NoiseGenerator>();
            noise->setType (noiseType);
            noise->setDuration (noiseDuration);
            noise->setAmplitude (0.3);
            noise->setSeed (noiseSeed);
            gen = std::move (noise);
            break;
        }

        case Source::dynamic:
        {
            // 2 s sine sweep (start frequency configurable) shaped by the
            // (configurable) ADSR envelope. The carrier amplitude, envelope
            // speed and edge times drive the dynamic level used by the
            // compression-family sweep (T4.3); the defaults reproduce the
            // original dynamic-source signal exactly.
            auto sweep = std::make_unique<SineSweep>();
            sweep->setFrequencyRange (dynamicCarrierStartHz, 20000.0);
            sweep->setDuration (2.0);
            sweep->setAmplitude (dynamicAmplitude);

            auto env = std::make_unique<EnvelopeSignal> (std::move (sweep));
            env->setEnvelope (EnvelopeSignal::Envelope::adsr);
            env->setADSR (dynamicADSR[0], dynamicADSR[1], dynamicADSR[2], dynamicADSR[3]);
            env->setSpeed (dynamicSpeed);
            gen = std::move (env);
            break;
        }
    }

    if (gen == nullptr)
        return false;

    runner.setGenerator (gen.get());

    // Wire progress callback
    runner.setProgressCallback ([this] (float p)
    {
        lastProgress = p;
        if (progressCallback)
            progressCallback (p);
    });

    // Timeline playback (B2): apply the timeline's events between blocks
    // (elapsed wall-clock ms since the run started). The block callback is
    // wrapped so the caller's own callback still fires.
    const int64_t runStartMs = static_cast<int64_t> (juce::Time::getMillisecondCounter());

    runner.setBlockCallback ([this, timelineActive, runStartMs] (float progress,
                                                                 const juce::AudioBuffer<float>& dryBlock,
                                                                 const juce::AudioBuffer<float>& wetBlock)
    {
        if (timelineActive && plugin != nullptr)
        {
            const auto elapsedMs = static_cast<int64_t> (
                static_cast<uint32_t> (juce::Time::getMillisecondCounter())
                - static_cast<uint32_t> (runStartMs));
            const auto cursorBefore = timelinePlayback.getPlaybackCursor();
            timelinePlayback.applyEventsUpTo (elapsedMs, plugin);
            const auto cursorAfter = timelinePlayback.getPlaybackCursor();
            // Issue #2: fire only when an event was actually applied (cursor
            // advanced) — event-driven, not per-block spam. Runs on the
            // message thread, same thread that owns the cursor.
            if (cursorAfter != cursorBefore && playbackProgressCallback)
                playbackProgressCallback (static_cast<int> (cursorAfter),
                                          static_cast<int> (timelinePlayback.getPlaybackEventCount()),
                                          elapsedMs);
        }
        if (blockCallback)
            blockCallback (progress, dryBlock, wetBlock);
    });

    // Capture the parameter snapshot before running
    captureParameterSnapshot();

    // Run
    const bool runOk = runner.run();

    // R2: restore the parameters the playback timeline touched — also on a
    // failed/cancelled run, so the host never keeps the playback values.
    if (timelineActive && plugin != nullptr)
    {
        for (const auto& [paramId, value] : timelineRestore)
            restoreParamValue (plugin, paramId, value);
    }

    if (! runOk)
        return false;

    // Capture source metadata from the generator for the export (file only).
    if (auto* playback = dynamic_cast<FilePlayback*> (gen.get()))
    {
        sourceFilePath    = playback->getSourcePath();
        sourceSampleRate  = playback->getSourceSampleRate();
        resampleRatio     = playback->getResampleRatio();
        sourceDurationSec = playback->getDurationSec();
    }

    return true;
}

std::vector<double> MeasurementSession::getInputLevelsDB() const
{
    std::vector<double> levelsDB;
    levelsDB.reserve (lastLevels.size());

    for (double amp : lastLevels)
        levelsDB.push_back (MathUtils::amplitudeToDB (amp));

    return levelsDB;
}
