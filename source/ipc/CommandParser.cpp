#include "CommandParser.h"
#include "Protocol.h"
#include "../analysis/Export.h"
#include "../analysis/HarmonicAnalysis.h"
#include "../analysis/CompressionCurve.h"
#include "../analysis/GainReduction.h"
#include "../analysis/TimeConstants.h"
#include "../analysis/CompressionFamily.h"

#include <cfloat>

namespace
{
/** Escape a string for embedding inside a JSON string literal. juce::String::quoted()
    only doubles quote chars — backslashes (Windows paths!) stay unescaped, which
    yields INVALID JSON (`\P`, `\C` are illegal escapes). See verifier V1 finding and
    analysis/AGENTS.md (Oracle P0-4: pluginName.quoted() 不转内部引号). */
juce::String escapeJsonString (const juce::String& s)
{
    juce::String out;
    out.preallocateBytes (s.getNumBytesAsUTF8() + 16);
    for (auto c : s)
    {
        switch (c)
        {
            case '\\': out += "\\\\"; break;
            case '"':  out += "\\\""; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:   out += c;      break;
        }
    }
    return out;
}

// Runs a measurement and dispatches the analysis into `results` exactly as
// the measure command's lambda does: session->run(), then one analyzer
// matched to the session type (signal source), the GR-timeline dry/wet pair
// analysis (non-signal sources), or the raw-capture metadata (non-signal
// sources without GR). Returns false with `error` set on failure; does NOT
// write files or fire callbacks — export and notification stay in the
// caller. Shared by the measure and dataset commands.
static bool runAndAnalyze (MeasurementSession* session, juce::AudioPluginInstance* plugin,
                           MeasurementSession::Source source, const juce::String& sourceStr,
                           MeasurementResults& results, juce::String& error)
{
    if (! session->run())
    {
        error = R"("error":"measurement failed")";
        return false;
    }

    auto& result = session->getResult();

    results.type   = session->getType();
    results.source = sourceStr;

    if (source == MeasurementSession::Source::signal)
    {
        // Dispatch the analysis to the analyzer matching the session type.
        switch (session->getType())
        {
            case MeasurementSession::Type::frequencyResponse:
            {
                FreqResponse fr;
                fr.setLatencySamples (plugin->getLatencySamples());
                if (session->getFreqExcitation())
                    results.freq = fr.analyzeMLS (result.getDryBuffer(),
                                                  result.getWetBuffer(),
                                                  result.getSampleRate(),
                                                  session->getFreqMLSLength());
                else
                    results.freq = fr.analyze (result.getDryBuffer(),
                                               result.getWetBuffer(),
                                               result.getSampleRate());
                break;
            }

            case MeasurementSession::Type::harmonicAnalysis:
            {
                HarmonicAnalysis ha;
                results.harmonic = ha.analyze (result.getWetBuffer(),
                                               result.getSampleRate(),
                                               session->getFundamentalFreqs());
                break;
            }

            case MeasurementSession::Type::compressionCurve:
            {
                CompressionCurve cc;
                results.compression = cc.analyze (result.getDryBuffer(),
                                                  result.getWetBuffer(),
                                                  result.getSampleRate(),
                                                  session->getInputLevelsDB());
                break;
            }

            // Unreachable — the parser rejects gr_timeline for
            // Source::signal (defensive, keeps the enum switch
            // exhaustive).
            case MeasurementSession::Type::grTimeline:
                error = R"("error":"gr_timeline requires a non-signal source")";
                return false;
        }
    }
    else if (session->getType() == MeasurementSession::Type::grTimeline)
    {
        // GR timeline analysis (T4.4): gain reduction over the
        // recorded dry/wet pair, then attack/release time constants.
        //
        // - GainReduction reports the wet/dry ratio (negative dB for
        //   a compressor); the exported timeline keeps that convention.
        // - TimeConstants needs controlled envelope edges: the dynamic
        //   source has them (auto-detected via CompressionFamily); the
        //   file/noise sources are real-world material with no
        //   controlled edges, so their markers stay empty and the tau
        //   estimate is invalid by design (GR timeline still exported).
        // - A 1 ms RMS window (matching CompressionFamily's internal
        //   configuration) keeps enough timeline points to resolve a
        //   few-ms attack edge; the default 5 ms window blurs it away.
        results.gr = GainReduction::analyze (result.getDryBuffer(),
                                             result.getWetBuffer(),
                                             result.getSampleRate(),
                                             plugin->getLatencySamples(),
                                             0.001);

        TimeConstants::EventMarkers markers;
        if (source == MeasurementSession::Source::dynamic)
        {
            // detectMarkers and TimeConstants expect positive dB =
            // reduction; GainReduction reports the wet/dry ratio
            // (negative dB for a compressor). Negate a copy for the
            // edge detection AND the tau estimate (CompressionFamily
            // pattern — the exported timeline is NOT negated).
            auto grPositive = results.gr;
            for (auto& p : grPositive.timeline)
                p.grDB = -p.grDB;
            markers = CompressionFamily::detectMarkers (grPositive);
            results.tau = TimeConstants::estimate (grPositive, markers,
                                                   result.getSampleRate());
        }
        else
        {
            // file/noise sources have no controlled envelope edges:
            // markers stay empty and the tau estimate is invalid by
            // design (GR timeline still exported).
            results.tau = TimeConstants::estimate (results.gr, markers,
                                                   result.getSampleRate());
        }
    }
    else
    {
        // Raw capture: record-only export, no analysis (phase 4).
        results.rawSamples    = result.getNumRecordedSamples();
        results.rawSampleRate = result.getSampleRate();
    }

    return true;
}

// Serialises a completed measurement result into the export JSON, matching
// the type dispatch of the measure command's lambda. Raw captures
// (non-signal sources without GR-timeline analysis) are exported via
// rawCaptureToJSON. Shared by the measure and dataset commands.
static juce::String exportResultsToJSON (const MeasurementResults& results, const Export::Context& ctx,
                                         int blockSize)
{
    // Raw capture: record-only export, no analysis (phase 4).
    if (results.source != Protocol::Source::signal
        && results.type != MeasurementSession::Type::grTimeline)
        return Export::rawCaptureToJSON (results.rawSamples, results.rawSampleRate, blockSize, ctx);

    switch (results.type)
    {
        case MeasurementSession::Type::frequencyResponse:
            return Export::freqResponseToJSON (results.freq, ctx);

        case MeasurementSession::Type::harmonicAnalysis:
            return Export::harmonicAnalysisToJSON (results.harmonic, ctx);

        case MeasurementSession::Type::compressionCurve:
            return Export::compressionCurveToJSON (results.compression, ctx);

        case MeasurementSession::Type::grTimeline:
            return Export::grTimelineToJSON (results.gr, results.tau, ctx);
    }

    // Unreachable — MeasurementSession::Type is exhaustive above.
    return {};
}
}  // namespace

// Crash-protection WAV mirror: every measure/scan command flushes the
// captured dry/wet audio to a 24-bit .wav file (see CaptureBuffer::
// setFlushConfig) so a plugin crash — which kills the process — still
// leaves the audio recorded up to the last flush boundary.
constexpr double kDefaultFlushIntervalSec = 5.0;

// Maps the protocol source string to the session source enum. Returns false
// when the value is not a known protocol source; callers respond with the
// "unknown source" error.
static bool parseSource (const juce::String& sourceStr, MeasurementSession::Source& source)
{
    if      (sourceStr == Protocol::Source::signal)  source = MeasurementSession::Source::signal;
    else if (sourceStr == Protocol::Source::file)    source = MeasurementSession::Source::file;
    else if (sourceStr == Protocol::Source::noise)   source = MeasurementSession::Source::noise;
    else if (sourceStr == Protocol::Source::dynamic) source = MeasurementSession::Source::dynamic;
    else return false;

    return true;
}

// Applies the source-specific session configuration: the input file path
// (with existence check) for the file source, the noise generator settings
// for the noise source, and the carrier frequency / sweep-start for the
// dynamic source. Returns an error message when the configuration is
// invalid (empty string on success); callers respond with it. Shared by
// the measure and scan commands.
static juce::String configureSessionSource (MeasurementSession& session, const juce::DynamicObject& obj,
                                            MeasurementSession::Source source)
{
    if (source == MeasurementSession::Source::file)
    {
        // For the file source "path" names the INPUT audio file (the
        // export path is disambiguated via "export_path" below).
        auto inputPath = obj.getProperty ("path").toString();
        if (inputPath.isEmpty())
            return R"("error":"path required")";
        juce::File inputFile (inputPath);
        if (! inputFile.existsAsFile())
            return R"("error":"file not found")";
        session.setFilePath (inputFile);
    }
    else if (source == MeasurementSession::Source::noise)
    {
        NoiseGenerator::Type noiseType = NoiseGenerator::Type::white;
        auto noiseTypeStr = obj.getProperty ("noise_type").toString();
        if (noiseTypeStr == "pink")
            noiseType = NoiseGenerator::Type::pink;
        else if (! noiseTypeStr.isEmpty() && noiseTypeStr != "white")
            return R"("error":"unknown noise type")";

        double duration = 2.0;
        if (obj.hasProperty ("duration"))
            duration = static_cast<double> (obj.getProperty ("duration"));

        uint32_t seed = 0x2E42A5;
        if (obj.hasProperty ("seed"))
            seed = static_cast<uint32_t> (static_cast<int64_t> (obj.getProperty ("seed")));

        session.setNoiseConfig (noiseType, duration, seed);
    }
    else if (source == MeasurementSession::Source::dynamic)
    {
        if (obj.hasProperty ("carrier_freq"))
            session.setDynamicCarrierFreq (static_cast<double> (obj.getProperty ("carrier_freq")));

        // The carrier sweep must start high enough that the detector is
        // not polluted by low-frequency carrier wobble (otherwise the GR
        // attack edge is corrupted and tau comes back invalid). Default
        // to 10 kHz, matching CompressionFamily's internal configuration.
        double carrierStartHz = 10000.0;
        if (obj.hasProperty ("carrier_start_hz"))
            carrierStartHz = static_cast<double> (obj.getProperty ("carrier_start_hz"));
        session.setDynamicCarrierStartHz (carrierStartHz);
    }

    return {};
}

// Resolves the export path for a command: "export_path" for the file
// source (where "path" names the input audio file), "path" otherwise.
// Falls back to <current directory>/<defaultFileName> when omitted. The
// default file name differs per command (measure/scan) and per source.
static juce::String resolveExportPath (const juce::DynamicObject& obj, MeasurementSession::Source source,
                                       const juce::String& defaultFileName)
{
    auto path = (source == MeasurementSession::Source::file)
                    ? obj.getProperty ("export_path").toString()
                    : obj.getProperty ("path").toString();
    if (path.isEmpty())
        path = juce::File::getCurrentWorkingDirectory()
                   .getChildFile (defaultFileName)
                   .getFullPathName();
    return path;
}

// Derives the crash-protection WAV path for a command's export path: a
// trailing ".json" is swapped for ".wav" (e.g. "pluginlab_scan.json" →
// "pluginlab_scan.wav"); a path without a ".json" suffix gets ".wav"
// appended. The WAV mirrors the captured dry/wet audio (see CaptureBuffer::
// setFlushConfig) so a plugin crash still yields audio.
static juce::File wavPathFor (const juce::String& exportPath)
{
    if (exportPath.endsWith (".json"))
        return juce::File (exportPath).withFileExtension (".wav");
    return juce::File (exportPath + ".wav");
}

// Builds the export context for a measurement/scan: plugin identity and
// latency, session sample rate / block size / parameter snapshot, plus the
// input-source metadata (file source: path/sample-rate/resample/duration;
// noise source: type/seed/duration). Shared verbatim by the measure and
// scan commands.
static Export::Context buildExportContext (juce::AudioPluginInstance* plugin, MeasurementSession& session,
                                           MeasurementSession::Source source, const juce::String& sourceStr)
{
    Export::Context ctx;
    ctx.pluginName = plugin->getName();
    {
        juce::PluginDescription desc;
        plugin->fillInPluginDescription (desc);
        ctx.classId = desc.fileOrIdentifier;
    }
    ctx.latencySamples = plugin->getLatencySamples();
    ctx.sampleRate     = session.getSampleRate();
    ctx.blockSize      = session.getBlockSize();
    ctx.paramSnapshot  = session.getParameterSnapshot();
    ctx.excitation     = session.getFreqExcitation() ? Protocol::Excitation::mls
                                                     : Protocol::Excitation::sweep;

    // Attach the input-source metadata to the export.
    ctx.source.type = sourceStr;
    if (source == MeasurementSession::Source::file)
    {
        ctx.source.filePath         = session.getSourceFilePath();
        ctx.source.sourceSampleRate = session.getSourceSampleRate();
        ctx.source.resampleRatio    = session.getResampleRatio();
        ctx.source.durationSec      = session.getSourceDurationSec();
    }
    else if (source == MeasurementSession::Source::noise)
    {
        ctx.source.noiseType   = (session.getNoiseType() == NoiseGenerator::Type::pink)
                                     ? juce::String ("pink") : juce::String ("white");
        ctx.source.seed        = session.getNoiseSeed();
        ctx.source.durationSec = session.getNoiseDuration();
    }

    return ctx;
}

// Reads a JSON number array (non-empty, all numeric, optionally within
// [min,max]). Returns false when var is not such an array; on success `out`
// holds the parsed values in order. Shared by the dataset scan and
// compression_family blocks (the scan block additionally requires the 0..1
// normalized range).
static bool parseNumberArray (const juce::var& var, std::vector<double>& out,
                              double min = -DBL_MAX, double max = DBL_MAX)
{
    if (! var.isArray() || var.size() == 0)
        return false;

    for (int i = 0; i < var.size(); ++i)
    {
        const auto v = var[i];
        if (! (v.isDouble() || v.isInt() || v.isInt64()))
            return false;
        const double dv = static_cast<double> (v);
        if (dv < min || dv > max)
            return false;
        out.push_back (dv);
    }
    return true;
}

// Finds the parameter whose stable id matches, or nullptr when no parameter
// exposes that id. Hosted parameters expose a stable id that survives
// display-name changes; anything else is not a match. Shared by the scan
// command and the dataset scan block.
static juce::AudioProcessorParameter* findParamByStableId (juce::AudioProcessor& processor,
                                                           const juce::String& paramId)
{
    for (auto* candidate : processor.getParameters())
    {
        auto* hosted = dynamic_cast<juce::HostedAudioProcessorParameter*> (candidate);
        if (hosted != nullptr && hosted->getParameterID() == paramId)
            return candidate;
    }
    return nullptr;
}

juce::String CommandParser::handleCommand (const juce::String& jsonCommand)
{
    auto json = juce::JSON::parse (jsonCommand);
    auto* obj = json.getDynamicObject();

    if (obj == nullptr)
        return Protocol::makeResponse (false, R"("error":"invalid JSON")");

    auto cmd = obj->getProperty ("cmd").toString();

    // --- getScanStatus (计划步骤 5)：插件扫描状态快照。快照+推送双轨中的快照
    // 侧——纯推送漏掉中途连接的客户端，中途连接者用此命令拿当前状态。
    // 命名避开参数扫描（scan）语义。字段：running/done/progress/count/
    // currentFile/blacklisted/hangCount。
    if (cmd == Protocol::Command::getScanStatus)
    {
        if (pluginManager == nullptr)
            return Protocol::makeResponse (false, R"("error":"no plugin manager")");

        // 快照方法内部加锁（verifier M1）：count 走 KnownPluginList 内部锁，
        // blacklisted 走 knownListGuard，IPC 线程读与扫描/加载线程写互斥。
        const auto s = pluginManager->getScanStatusSnapshot();

        return Protocol::makeResponse (true,
            R"("running":)" + juce::String (s.running ? "true" : "false")
            + R"(,"done":)" + juce::String (s.done ? "true" : "false")
            + R"(,"progress":)" + juce::String (s.progress, 3)
            + R"(,"count":)" + juce::String (s.count)
            + R"(,"blacklisted":)" + juce::String (s.blacklisted)
            + R"(,"hangCount":)" + juce::String (s.hangCount)
            + R"(,"currentFile":")" + escapeJsonString (s.currentFile) + "\"");
    }

    // --- loadPlugin ---
    if (cmd == "loadPlugin")
    {
        auto path = obj->getProperty ("path").toString();
        if (path.isEmpty())
            return Protocol::makeResponse (false, R"("error":"path required")");

        if (pluginManager == nullptr)
            return Protocol::makeResponse (false, R"("error":"no plugin manager")");

        auto& knownPlugins = pluginManager->getKnownPlugins();
        auto types = knownPlugins.getTypes();

        for (auto& d : types)
        {
            // fileOrIdentifier matches exactly (it is a canonical path/ID);
            // the display name matches case-insensitively so callers can
            // address a plugin by a loosely-cased name.
            if (d.fileOrIdentifier == path || d.name.equalsIgnoreCase (path))
            {
                if (loadPluginCallback)
                {
                    auto descCopy = d;
                    juce::MessageManager::callAsync ([this, descCopy] { loadPluginCallback (descCopy); });
                }
                return Protocol::makeResponse (true, R"("name":")" + escapeJsonString (d.name) + "\"");
            }
        }
        return Protocol::makeResponse (false, R"("error":"plugin not found")");
    }

    // --- setParam ---
    if (cmd == "setParam")
    {
        if (plugin == nullptr)
            return Protocol::makeResponse (false, R"("error":"no plugin loaded")");

        auto name = obj->getProperty ("name").toString();
        auto paramId = obj->getProperty ("param_id").toString();
        double value = obj->getProperty ("value");

        auto& params = plugin->getParameters();

        // Parameter resolution order (documented):
        //   1. param_id (stable hosted-parameter ID) — authoritative when
        //      present. Display names are ambiguous across EQ bands ("Band 1
        //      Gain" / "Band 1 Used" both contain "Band 1"), so an id that
        //      matches nothing returns "parameter not found" rather than
        //      falling back to a name guess (ambiguity lock; mirrors the
        //      scan command's strict id lookup).
        //   2. name match — legacy single pass over getParameters() in order,
        //      first parameter whose display name equals OR contains the given
        //      name wins (== is checked per parameter before contains, but an
        //      earlier contains-hit still beats a later exact hit).
        if (paramId.isNotEmpty())
        {
            for (auto* candidate : params)
            {
                auto* hosted = dynamic_cast<juce::HostedAudioProcessorParameter*> (candidate);
                if (hosted != nullptr && hosted->getParameterID() == paramId)
                {
                    candidate->setValueNotifyingHost (static_cast<float> (value));
                    return Protocol::makeResponse (true, R"("param":")" + escapeJsonString (candidate->getName (128))
                                                     + R"(","value":)" + juce::String (value, 4));
                }
            }
            return Protocol::makeResponse (false, R"("error":"parameter not found")");
        }

        for (int i = 0; i < params.size(); ++i)
        {
            auto n = params[i]->getName (128);
            if (n == name || n.contains (name))
            {
                params[i]->setValueNotifyingHost (static_cast<float> (value));
                return Protocol::makeResponse (true, R"("param":")" + escapeJsonString (name)
                                                 + R"(","value":)" + juce::String (value, 4));
            }
        }
        return Protocol::makeResponse (false, R"("error":"parameter not found")");
    }

    // --- getParams ---
    if (cmd == "getParams")
    {
        if (plugin == nullptr)
            return Protocol::makeResponse (false, R"("error":"no plugin loaded")");

        juce::String data = R"("params":[)";
        auto& params = plugin->getParameters();
        for (int i = 0; i < params.size(); ++i)
        {
            auto p = params[i];
            auto n = p->getName (128);
            auto v = p->getValue();

            // Stable parameter ID, resolved the same way the scan command
            // does (see the "scan" case below): hosted parameters expose a
            // stable ID that survives display-name changes; anything else
            // gets an empty id.
            juce::String paramId;
            if (auto* hosted = dynamic_cast<juce::HostedAudioProcessorParameter*> (p))
                paramId = hosted->getParameterID();

            data += R"({"index":)" + juce::String (i)
                  + R"(,"name":")" + escapeJsonString (n)
                  + R"(","value":)" + juce::String (v, 4)
                  + R"(,"param_id":")" + paramId + R"("})";
            if (i < params.size() - 1) data += ",";
        }
        data += "]";
        return Protocol::makeResponse (true, data);
    }

    // --- measure ---
    if (cmd == "measure")
    {
        if (session == nullptr || plugin == nullptr)
            return Protocol::makeResponse (false, R"("error":"no session or plugin")");

        // --- input source (default: signal) ---
        auto sourceStr = obj->getProperty ("source").toString();
        if (sourceStr.isEmpty())
            sourceStr = Protocol::Source::signal;

        MeasurementSession::Source source = MeasurementSession::Source::signal;
        if (! parseSource (sourceStr, source))
            return Protocol::makeResponse (false, R"("error":"unknown source")");

        // --- measurement type (analysis field; for raw sources it is only
        //     metadata — defaults to frequency_response when omitted) ---
        auto t = obj->getProperty ("type").toString();
        if (t.isEmpty())
            t = Protocol::MeasureType::freq;
        if (t == Protocol::MeasureType::freq)
            session->setMeasurementType (MeasurementSession::Type::frequencyResponse);
        else if (t == Protocol::MeasureType::harmonic)
            session->setMeasurementType (MeasurementSession::Type::harmonicAnalysis);
        else if (t == Protocol::MeasureType::compression)
            session->setMeasurementType (MeasurementSession::Type::compressionCurve);
        else if (t == Protocol::MeasureType::grTimeline)
            session->setMeasurementType (MeasurementSession::Type::grTimeline);
        else
            return Protocol::makeResponse (false, R"("error":"unknown measure type")");

        // The GR timeline needs a recorded dry/wet pair with dynamics:
        // built-in signal sources have no GR timeline generator.
        if (t == Protocol::MeasureType::grTimeline
            && source == MeasurementSession::Source::signal)
            return Protocol::makeResponse (false,
                R"("error":"gr_timeline requires a non-signal source")");

        // --- source-specific configuration ---
        auto sourceError = configureSessionSource (*session, *obj, source);
        if (sourceError.isNotEmpty())
            return Protocol::makeResponse (false, sourceError);

        // --- frequency-response excitation (optional; default sweep) ---
        // Applies only to frequency_response measurements; other types force
        // sweep so no stale MLS residue leaks into non-freq exports.
        if (t == Protocol::MeasureType::freq)
        {
            auto excitationStr = obj->getProperty ("excitation").toString();
            if (excitationStr.isEmpty())
                excitationStr = Protocol::Excitation::sweep;
            if (excitationStr == Protocol::Excitation::mls)
                session->setFreqExcitation (true);
            else if (excitationStr == Protocol::Excitation::sweep)
                session->setFreqExcitation (false);
            else
                return Protocol::makeResponse (false,
                    R"("error":"unknown excitation ')" + escapeJsonString (excitationStr)
                    + "' (expected sweep|mls)\"");
        }
        else
        {
            session->setFreqExcitation (false);
        }

        session->setSource (source);
        session->setPluginInstance (plugin);

        // Determine export path:
        //   - signal/noise/dynamic → "path" (existing protocol)
        //   - file → "export_path" ("path" names the input audio file)
        auto path = resolveExportPath (*obj, source,
                                       source == MeasurementSession::Source::signal
                                           ? "pluginlab_freq_response.json"
                                           : "pluginlab_raw_capture.json");

        // Crash protection: mirror the captured dry/wet audio to a WAV file
        // next to the export JSON. Set before the run (safe on either thread —
        // the sync and async dispatch paths both run after this point); a
        // plugin crash then loses at most kDefaultFlushIntervalSec of audio.
        session->getResult().setFlushConfig (wavPathFor (path), kDefaultFlushIntervalSec);

        if (statusCallback)
            juce::MessageManager::callAsync ([this] { statusCallback ("Measuring..."); });

        // Body of the measurement: run, analyse, export, build response.
        // Extracted as a reusable lambda so both the sync (message-thread)
        // and async (IPC-thread → message-thread dispatch) paths share it.
        auto runMeasurement = [&]() -> juce::String
        {
            MeasurementResults results;
            juce::String error;

            if (! runAndAnalyze (session, plugin, source, sourceStr, results, error))
                return Protocol::makeResponse (false, error);

            Export::Context ctx = buildExportContext (plugin, *session, source, sourceStr);

            auto exportJson = exportResultsToJSON (results, ctx, session->getBlockSize());

            juce::File exportFile (path);
            Export::writeToFile (exportJson, exportFile);

            // measurementCompleteCallback is fired synchronously on the
            // measurement thread — unit tests assert this timing.
            if (measurementCompleteCallback)
                measurementCompleteCallback (results);

            juce::String d = R"("samples":)" + juce::String (session->getResult().getNumRecordedSamples())
                           + R"(,"rate":)"    + juce::String (session->getResult().getSampleRate())
                           + R"(,"export_path":")" + escapeJsonString (path) + "\""
                           + R"(,"wav_path":")" + escapeJsonString (wavPathFor (path).getFullPathName()) + "\"";
            return Protocol::makeResponse (true, d);
        };

        // Dispatch strategy:
        //   - already on the message thread (unit tests, or when called from
        //     within a message callback):  execute synchronously.
        //   - any other thread (real IPC PipeServer thread):  dispatch via
        //     callAsync + WaitableEvent so processBlock runs on the message
        //     thread (required by Pro-Q 4 and similar VST3 plugins).
        if (juce::MessageManager::getInstance()->isThisTheMessageThread())
        {
            return runMeasurement();
        }

        juce::WaitableEvent done;
        juce::String response;

        juce::MessageManager::callAsync ([&]
        {
            response = runMeasurement();
            done.signal();
        });

        done.wait();
        return response;
    }

    // --- scan ---
    if (cmd == "scan")
    {
        if (session == nullptr || plugin == nullptr)
            return Protocol::makeResponse (false, R"("error":"no session or plugin")");

        // --- measurement type (analysis field; default frequency_response) ---
        auto t = obj->getProperty ("type").toString();
        if (t.isEmpty())
            t = Protocol::MeasureType::freq;

        MeasurementSession::Type scanType;
        if (t == Protocol::MeasureType::freq)
            scanType = MeasurementSession::Type::frequencyResponse;
        else if (t == Protocol::MeasureType::harmonic)
            scanType = MeasurementSession::Type::harmonicAnalysis;
        else if (t == Protocol::MeasureType::compression)
            scanType = MeasurementSession::Type::compressionCurve;
        else
            return Protocol::makeResponse (false, R"("error":"unknown measure type")");

        // --- scanned parameter (located by stable ID) ---
        auto paramId = obj->getProperty ("param_id").toString();
        if (paramId.isEmpty())
            return Protocol::makeResponse (false, R"("error":"param_id required")");

        juce::AudioProcessorParameter* scanParam = findParamByStableId (*plugin, paramId);
        if (scanParam == nullptr)
            return Protocol::makeResponse (false, R"("error":"parameter not found")");

        // --- values (normalized 0..1; must be non-empty) ---
        auto valuesVar = obj->getProperty ("values");
        if (! valuesVar.isArray() || valuesVar.size() == 0)
            return Protocol::makeResponse (false, R"("error":"values array required")");

        std::vector<float> values;
        values.reserve (static_cast<size_t> (valuesVar.size()));
        for (int i = 0; i < valuesVar.size(); ++i)
        {
            const double v = static_cast<double> (valuesVar[i]);
            if (v < 0.0 || v > 1.0)
                return Protocol::makeResponse (false, R"#("error":"values out of range (0..1)")#");
            values.push_back (static_cast<float> (v));
        }

        // --- input source (default: signal; other sources capture raw per
        //     round — analysis still runs via ScanEngine) ---
        auto sourceStr = obj->getProperty ("source").toString();
        if (sourceStr.isEmpty())
            sourceStr = Protocol::Source::signal;

        MeasurementSession::Source source = MeasurementSession::Source::signal;
        if (! parseSource (sourceStr, source))
            return Protocol::makeResponse (false, R"("error":"unknown source")");

        // --- source-specific configuration (mirrors the measure command) ---
        auto sourceError = configureSessionSource (*session, *obj, source);
        if (sourceError.isNotEmpty())
            return Protocol::makeResponse (false, sourceError);

        // The scan command carries no excitation field: it always measures
        // with the sweep excitation. Force sweep so a stale MLS residue from
        // a previous measure/dataset cannot silently change the scan's
        // signal/analysis path (review fix).
        session->setFreqExcitation (false);

        session->setSource (source);
        session->setPluginInstance (plugin);

        // --- export path (default pluginlab_scan.json) ---
        auto path = resolveExportPath (*obj, source, "pluginlab_scan.json");

        // Crash protection: same WAV mirror as the measure command. Set once
        // here — the ScanEngine reuses this session for every round, so the
        // config persists across rounds (each round's first append re-opens /
        // truncates the .wav; acceptable for crash-protection purposes).
        session->getResult().setFlushConfig (wavPathFor (path), kDefaultFlushIntervalSec);

        if (statusCallback)
            juce::MessageManager::callAsync ([this] { statusCallback ("Scanning..."); });

        // Body of the scan: run the engine, export, build the response.
        // Extracted as a reusable lambda so both the sync (message-thread)
        // and async (IPC-thread → message-thread dispatch) paths share it.
        auto runScan = [&]() -> juce::String
        {
            ScanEngine engine;
            engine.setPluginInstance (plugin);
            engine.setSession (session);

            // Per-round progress → status callback (e.g. "scan round 2/5").
            auto scanResult = engine.run (paramId, values, scanType,
                [this] (int round, int totalRounds)
                {
                    juce::MessageManager::callAsync ([this, round, totalRounds]
                    {
                        if (statusCallback)
                            statusCallback ("scan round " + juce::String (round)
                                            + "/" + juce::String (totalRounds));
                    });
                });

            // No rounds completed (round 1 failed or the scan was aborted
            // before the first round): report the error.
            if (scanResult.family.empty())
                return Protocol::makeResponse (false, R"("error":"scan failed")");

            Export::Context ctx = buildExportContext (plugin, *session, source, sourceStr);

            auto exportJson = Export::scanToJSON (scanResult, scanType, ctx);
            juce::File exportFile (path);
            Export::writeToFile (exportJson, exportFile);

            // scanCompleteCallback fires synchronously on the measurement
            // thread — unit tests assert this timing (measure pattern).
            if (scanCompleteCallback)
                scanCompleteCallback (scanResult);

            juce::String d = R"("runs":)" + juce::String (static_cast<int> (scanResult.family.size()))
                           + R"(,"export_path":")" + escapeJsonString (path) + "\""
                           + R"(,"wav_path":")" + escapeJsonString (wavPathFor (path).getFullPathName()) + "\"";
            return Protocol::makeResponse (true, d);
        };

        // Dispatch strategy mirrors the measure command: synchronous on the
        // message thread, callAsync + WaitableEvent from any other thread.
        if (juce::MessageManager::getInstance()->isThisTheMessageThread())
            return runScan();

        juce::WaitableEvent done;
        juce::String response;
        juce::MessageManager::callAsync ([&] { response = runScan(); done.signal(); });
        done.wait();
        return response;
    }

    // --- dataset ---
    if (cmd == Protocol::Command::dataset)
    {
        if (session == nullptr || plugin == nullptr)
            return Protocol::makeResponse (false, R"("error":"no session or plugin")");

        // --- battery spec table ---
        // One entry per protocol measure type: the session type/source to
        // run plus the per-run results/ok state. Declared here — per command,
        // outside the run lambda — so the state is fresh for every invocation
        // and the Export::Dataset pointers into it stay valid until
        // datasetToJSON runs.
        struct BatteryRun
        {
            const char* key;        // Protocol::MeasureType string
            MeasurementSession::Type type;
            MeasurementSession::Source source;
            const char* sourceStr;  // protocol source string ("signal"/"dynamic")
            MeasurementResults results;
            bool ok = false;
        };

        BatteryRun battery[] = {
            { Protocol::MeasureType::freq,        MeasurementSession::Type::frequencyResponse, MeasurementSession::Source::signal,  Protocol::Source::signal,  {}, false },
            { Protocol::MeasureType::harmonic,    MeasurementSession::Type::harmonicAnalysis,  MeasurementSession::Source::signal,  Protocol::Source::signal,  {}, false },
            { Protocol::MeasureType::compression, MeasurementSession::Type::compressionCurve, MeasurementSession::Source::signal,  Protocol::Source::signal,  {}, false },
            { Protocol::MeasureType::grTimeline,  MeasurementSession::Type::grTimeline,       MeasurementSession::Source::dynamic, Protocol::Source::dynamic, {}, false },
        };

        // Locates a battery entry by protocol key (nullptr when the key is
        // unknown — the type validation below rejects unknown keys).
        auto batteryRunFor = [&] (const juce::String& key) -> BatteryRun*
        {
            for (auto& b : battery)
                if (key == b.key)
                    return &b;
            return nullptr;
        };

        // --- measurement types (validated BEFORE any block runs) ---
        // Omitted → the default 4-type battery; an explicit empty array
        // requests no battery (a scan/compression_family-only dataset is
        // still valid); an unknown string fails the whole command.
        std::vector<juce::String> requestedTypes;
        auto typesVar = obj->getProperty ("types");
        if (! typesVar.isArray())
        {
            for (const auto& b : battery)
                requestedTypes.push_back (juce::String (b.key));
        }
        else
        {
            for (int i = 0; i < typesVar.size(); ++i)
            {
                const auto t = typesVar[i].toString();
                if (batteryRunFor (t) == nullptr)
                    return Protocol::makeResponse (false, R"("error":"unknown measure type")");
                requestedTypes.push_back (t);
            }
        }

        // --- frequency-response excitation (optional; default sweep) ---
        // Applies to every frequency_response measurement in the dataset —
        // the battery freq block AND the optional scan block share one
        // excitation; an unknown value skips the freq block (deterministic
        // partial failure, mirroring the scan/compression_family block
        // validation).
        bool freqExcitationValid = true;
        bool freqExcitationMLS = false;
        auto excitationStr = obj->getProperty ("excitation").toString();
        if (excitationStr.isEmpty())
            excitationStr = Protocol::Excitation::sweep;
        if (excitationStr == Protocol::Excitation::mls)
            freqExcitationMLS = true;
        else if (excitationStr != Protocol::Excitation::sweep)
            freqExcitationValid = false;

        // Apply the (validated) excitation up front so the whole dataset —
        // including the scan block when the battery omits frequency_response —
        // measures with the requested excitation (review fix).
        session->setFreqExcitation (freqExcitationValid ? freqExcitationMLS : false);

        // Export path: the dataset command has no input file, so "path"
        // always names the JSON destination.
        auto path = obj->getProperty ("path").toString();

        // Crash protection: mirror the captured dry/wet audio to a WAV file
        // next to the export JSON (same as measure/scan; the last run's
        // append wins).
        session->getResult().setFlushConfig (wavPathFor (path), kDefaultFlushIntervalSec);

        if (statusCallback)
            juce::MessageManager::callAsync ([this] { statusCallback ("Collecting dataset..."); });

        // Body of the dataset: run the battery, the optional scan and the
        // optional compression family, aggregate everything into one Export::
        // Dataset package and write it. Extracted as a reusable lambda so
        // both the sync (message-thread) and async (IPC-thread → message-
        // thread dispatch) paths share it.
        auto runDataset = [&]() -> juce::String
        {
            // --- battery: run each requested type with its fixed source ---
            // Source mapping (v1, not overridable): freq/harmonic/compression
            // → signal; gr_timeline → dynamic (so the tau estimate is valid).
            // Runs in the order the caller listed the types (requestedTypes
            // is the source of truth); results/ok accumulate in the battery
            // table, which outlives datasetToJSON below.
            for (const auto& t : requestedTypes)
            {
                BatteryRun* run = batteryRunFor (t);
                if (run == nullptr)
                    continue;   // unreachable — requestedTypes was validated above

                // Unknown excitation: skip the frequency_response block only
                // (deterministic partial failure, like the scan block).
                if (run->type == MeasurementSession::Type::frequencyResponse
                    && ! freqExcitationValid)
                    continue;

                // Source-specific configuration — mirrors the measure command.
                // For the gr_timeline run this applies the same dynamic-carrier
                // defaults (carrier_start_hz = 10 kHz) the standalone measure
                // path relies on, keeping the tau estimate valid.
                auto sourceError = configureSessionSource (*session, *obj, run->source);
                if (sourceError.isNotEmpty())
                    continue;   // skip this type; unreachable for signal/dynamic

                session->setSource (run->source);
                session->setMeasurementType (run->type);
                session->setPluginInstance (plugin);

                // Frequency-response excitation applies to the whole dataset.
                if (run->type == MeasurementSession::Type::frequencyResponse)
                    session->setFreqExcitation (freqExcitationMLS);

                juce::String error;
                if (runAndAnalyze (session, plugin, run->source, run->sourceStr, run->results, error))
                    run->ok = true;
                // Per-type failure: record types[type] = false and continue
                // with the next type.
            }

            // --- scan (optional block; deterministic partial failure) ---
            ScanEngine::ScanResult scanResult;
            bool scanOk = false;
            MeasurementSession::Type scanType = MeasurementSession::Type::frequencyResponse;

            auto scanVar = obj->getProperty ("scan");
            if (scanVar.isObject())
            {
                auto* scanObj = scanVar.getDynamicObject();
                auto paramId = scanObj->getProperty ("param_id").toString();
                auto valuesVar = scanObj->getProperty ("values");

                // Scan values must be numeric and normalized 0..1 (the
                // scan-command contract); invalid → the scan block is
                // skipped (deterministic partial failure).
                bool scanValid = ! paramId.isEmpty();

                std::vector<float> scanValues;
                if (scanValid)
                {
                    std::vector<double> parsedValues;
                    scanValid = parseNumberArray (valuesVar, parsedValues, 0.0, 1.0);
                    if (scanValid)
                    {
                        scanValues.reserve (static_cast<size_t> (parsedValues.size()));
                        for (const double dv : parsedValues)
                            scanValues.push_back (static_cast<float> (dv));
                    }
                }

                // Scan analysis type: default frequency_response; gr_timeline
                // (and anything unknown) is rejected for scans.
                if (scanValid)
                {
                    auto st = scanObj->getProperty ("type").toString();
                    if (st.isEmpty())
                        st = Protocol::MeasureType::freq;
                    if (st == Protocol::MeasureType::freq)
                        scanType = MeasurementSession::Type::frequencyResponse;
                    else if (st == Protocol::MeasureType::harmonic)
                        scanType = MeasurementSession::Type::harmonicAnalysis;
                    else if (st == Protocol::MeasureType::compression)
                        scanType = MeasurementSession::Type::compressionCurve;
                    else
                        scanValid = false;
                }

                // Parameter lookup by stable ID (scan-command pattern).
                if (scanValid && findParamByStableId (*plugin, paramId) == nullptr)
                    scanValid = false;

                if (scanValid)
                {
                    // The battery leaves the session on the dynamic source;
                    // reset it for the scan (signal, matching the standalone
                    // scan command's default).
                    session->setSource (MeasurementSession::Source::signal);
                    session->setMeasurementType (scanType);
                    session->setPluginInstance (plugin);

                    ScanEngine engine;
                    engine.setPluginInstance (plugin);
                    engine.setSession (session);

                    // Per-round progress → status callback (e.g. "scan round 2/5").
                    scanResult = engine.run (paramId, scanValues, scanType,
                        [this] (int round, int totalRounds)
                        {
                            juce::MessageManager::callAsync ([this, round, totalRounds]
                            {
                                if (statusCallback)
                                    statusCallback ("scan round " + juce::String (round)
                                                    + "/" + juce::String (totalRounds));
                            });
                        });

                    // No rounds completed (round 1 failed or aborted) → the
                    // scan block is skipped (deterministic partial failure).
                    if (! scanResult.family.empty() && ! scanResult.cancelled)
                        scanOk = true;
                }
            }

            // --- compression_family (optional block) ---
            CompressionFamily::FamilyResult cfResult;
            bool cfOk = false;

            auto cfVar = obj->getProperty ("compression_family");
            if (cfVar.isObject())
            {
                auto* cfObj = cfVar.getDynamicObject();

                // levels_db / speeds may be omitted — internal defaults
                // [-12.0, 0.0] × [0.5, 1.0, 2.0]. Present but invalid
                // (non-array / empty / non-numeric) → block skipped.
                std::vector<double> levelsDB;
                std::vector<double> speeds;
                bool cfValid = true;

                auto levelsVar = cfObj->getProperty ("levels_db");
                if (levelsVar.isVoid())
                {
                    levelsDB = { -12.0, 0.0 };
                }
                else if (! parseNumberArray (levelsVar, levelsDB))
                {
                    cfValid = false;
                }

                auto speedsVar = cfObj->getProperty ("speeds");
                if (cfValid)
                {
                    if (speedsVar.isVoid())
                    {
                        speeds = { 0.5, 1.0, 2.0 };
                    }
                    else if (! parseNumberArray (speedsVar, speeds))
                    {
                        cfValid = false;
                    }
                }

                if (cfValid)
                {
                    // Per-cell progress → status callback.
                    cfResult = CompressionFamily::measure (plugin, session, levelsDB, speeds,
                        [this] (int done, int total)
                        {
                            juce::MessageManager::callAsync ([this, done, total]
                            {
                                if (statusCallback)
                                    statusCallback ("compression family " + juce::String (done)
                                                    + "/" + juce::String (total));
                            });
                        });

                    // Empty family (all cells failed) or cancellation → skip.
                    if (! cfResult.entries.empty() && ! cfResult.cancelled)
                        cfOk = true;
                }
            }

            // All blocks failed → the whole dataset fails (S2/S3).
            bool anyBatteryOk = false;
            for (const auto& b : battery)
                anyBatteryOk = anyBatteryOk || b.ok;
            if (! anyBatteryOk && ! scanOk && ! cfOk)
                return Protocol::makeResponse (false, R"("error":"all measurements failed")");

            // --- aggregate + export ---
            // Battery results/ok are addressed by protocol key (the table is
            // the single mapping between protocol keys and per-type state).
            auto* grRun   = batteryRunFor (Protocol::MeasureType::grTimeline);
            auto* freqRun = batteryRunFor (Protocol::MeasureType::freq);
            auto* harmRun = batteryRunFor (Protocol::MeasureType::harmonic);
            auto* compRun = batteryRunFor (Protocol::MeasureType::compression);

            Export::Dataset dataset;
            dataset.scan = scanOk ? &scanResult : nullptr;
            dataset.scanType = scanType;
            dataset.compressionFamily = cfOk ? &cfResult : nullptr;
            dataset.grTimeline = grRun->ok ? &grRun->results.gr : nullptr;
            dataset.grTau = grRun->ok ? &grRun->results.tau : nullptr;
            dataset.freq = freqRun->ok ? &freqRun->results.freq : nullptr;
            dataset.harmonic = harmRun->ok ? &harmRun->results.harmonic : nullptr;
            dataset.compression = compRun->ok ? &compRun->results.compression : nullptr;

            // Context source: the last successful battery type (dynamic when
            // the GR timeline ran, signal otherwise).
            MeasurementSession::Source ctxSource = MeasurementSession::Source::signal;
            juce::String ctxSourceStr = Protocol::Source::signal;
            if (grRun->ok)
            {
                ctxSource = MeasurementSession::Source::dynamic;
                ctxSourceStr = Protocol::Source::dynamic;
            }

            Export::Context ctx = buildExportContext (plugin, *session, ctxSource, ctxSourceStr);

            auto exportJson = Export::datasetToJSON (dataset, ctx);
            juce::File (path).getParentDirectory().createDirectory();
            Export::writeToFile (exportJson, juce::File (path));

            // Response "types" object always carries all 4 keys (true/false),
            // derived from the battery table in protocol order.
            const int numBattery = static_cast<int> (sizeof (battery) / sizeof (battery[0]));
            juce::String d = R"("export_path":")" + escapeJsonString (path) + "\""
                           + R"(,"types":{)";
            for (int i = 0; i < numBattery; ++i)
            {
                d += R"(")" + juce::String (battery[i].key) + R"(":)" + juce::String (battery[i].ok ? "true" : "false");
                if (i < numBattery - 1)
                    d += ",";
            }
            d += R"(},"scan":)" + juce::String (scanOk ? "true" : "false")
               + R"(,"compression_family":)" + juce::String (cfOk ? "true" : "false");
            return Protocol::makeResponse (true, d);
        };

        // Dispatch strategy mirrors measure/scan: synchronous on the message
        // thread, callAsync + WaitableEvent from any other thread.
        if (juce::MessageManager::getInstance()->isThisTheMessageThread())
            return runDataset();

        juce::WaitableEvent done;
        juce::String response;
        juce::MessageManager::callAsync ([&] { response = runDataset(); done.signal(); });
        done.wait();
        return response;
    }

    // --- stop ---
    if (cmd == "stop")
    {
        if (session != nullptr)
            session->cancel();
        return Protocol::makeResponse (true);
    }

    return Protocol::makeResponse (false, R"("error":"unknown cmd")");
}
