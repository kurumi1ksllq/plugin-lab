/**
 * ReplicaMeasurementTests (issue #27, T4): child-host 4-type measurement of
 * the spec-driven PluginLabReplica VST3 (AC1: scannable/loadable + all four
 * measurements; AC3: export structure).
 *
 * Each test writes a temp chain_doc (one usable_as_spec entry carrying a full
 * replica spec: EQ 1000 Hz / +6 dB / Q 1.0, compressor threshold -20 dB /
 * ratio 4:1, GR attack 5 ms / release 50 ms), points PLUGINLAB_REPLICA_SPEC
 * at it, and drives the REAL out-of-process host child (PluginHostChild.exe,
 * PLUGIN_HOST_CHILD_EXE) to measure one type against the built
 * PluginLabReplica.vst3 (REPLICA_PLUGIN): frequency_response (sweep),
 * harmonic (SequentialTone), compression (ToneBurst), gr_timeline (enveloped
 * sweep). The child only collects dry/wet into a 24-bit WAV (ADR-D-5); each
 * test turns the WAV into export JSON with the SAME host-side ChildWavAnalyzer
 * entry the product uses (AC3 shape), then asserts the measured type's point
 * array is non-empty (AC1 evidence). The frequency_response curve is
 * additionally checked for the spec bell centre: the mean magnitude of the
 * 900-1100 Hz band must sit above the adjacent 400-600 Hz band (measured
 * elevation ~+1.0 dB -- the H1-on-chirp estimate smears the +1.5 dB
 * steady-state elevation; its gain-release transient also spikes the top end,
 * so a global peak is not a valid check).
 *
 * If the artifact or the child exe is absent (should not happen --
 * add_dependencies guarantees the build order), the tests SKIP with the
 * reason logged, mirroring the ChildHostParityTests real-plugin exception.
 */
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "../source/host/ChildProcessCoordinator.h"
#include "../source/analysis/ChildWavAnalyzer.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <vector>

#ifndef REPLICA_PLUGIN
#error "REPLICA_PLUGIN must be defined by tests/CMakeLists.txt"
#endif

#ifndef PLUGIN_HOST_CHILD_EXE
#error "PLUGIN_HOST_CHILD_EXE must be defined by tests/CMakeLists.txt"
#endif

namespace
{
    //==============================================================================
    juce::File replicaPluginFile()
    {
        return juce::File (juce::String (REPLICA_PLUGIN));
    }

    juce::File realChildExe()
    {
        return juce::File (juce::String (PLUGIN_HOST_CHILD_EXE));
    }

    juce::File tempFile (const juce::String& prefix, const juce::String& suffix)
    {
        return juce::File::getSpecialLocation (juce::File::SpecialLocationType::tempDirectory)
                   .getNonexistentChildFile (prefix, suffix);
    }

    /** Pop lines until one contains `needle` (progress lines pass through
     *  silently). Returns the matching line or empty on timeout. */
    juce::String popUntil (PluginHostChildCoordinator& coord, const juce::String& needle,
                           int timeoutMs)
    {
        const auto deadline = juce::Time::getMillisecondCounter()
                            + static_cast<juce::uint32> (timeoutMs);
        while (juce::Time::getMillisecondCounter() < deadline)
        {
            const auto line = coord.popLine (200);
            if (line.isEmpty())
                continue;
            if (line.contains (needle))
                return line;
        }
        return {};
    }

    /** start -> load -> measure (type configurable); returns the measure
     *  result line (contains "samples") or empty on error/timeout. */
    juce::String measureViaChild (PluginHostChildCoordinator& coord,
                                  const juce::String& pluginPath,
                                  const juce::String& excitation,
                                  const juce::File& exportPath,
                                  const juce::File& wavPath,
                                  const juce::String& measureType)
    {
        // start handshake
        if (! coord.sendLine (R"({"cmd":"start"})"))
            return {};
        if (popUntil (coord, "\"pid\":", 5000).isEmpty())
            return {};

        // load
        {
            juce::DynamicObject req;
            req.setProperty ("cmd", "load");
            req.setProperty ("path", pluginPath);
            // JSON::toString(v) defaults to MULTILINE output -- the child's
            // getline would receive a truncated fragment. allOnOneLine=true.
            const auto reqVar = juce::var (new juce::DynamicObject (req));
            if (! coord.sendLine (juce::JSON::toString (reqVar, true)))
                return {};
            const auto loadLine = popUntil (coord, "\"name\"", 40000);
            if (loadLine.isEmpty() || ! loadLine.contains ("\"ok\":true"))
                return {};
        }

        // measure
        {
            juce::DynamicObject req;
            req.setProperty ("cmd", "measure");
            req.setProperty ("type", measureType);
            req.setProperty ("excitation", excitation);
            req.setProperty ("sample_rate", 48000);
            req.setProperty ("block_size", 512);
            req.setProperty ("export_path", exportPath.getFullPathName());
            req.setProperty ("wav_path", wavPath.getFullPathName());
            const auto reqVar = juce::var (new juce::DynamicObject (req));
            if (! coord.sendLine (juce::JSON::toString (reqVar, true)))
                return {};
            return popUntil (coord, "\"samples\"", 60000);
        }
    }

    /** Writes a temp chain_doc with ONE usable_as_spec entry carrying the full
     *  replica spec: EQ 1000 Hz / +6 dB / Q 1.0, compressor threshold -20 dB /
     *  ratio 4:1, GR attack 5 ms / release 50 ms -- the same shape
     *  ReplicaSpecTests.cpp / PluginLabReplicaTests.cpp use. */
    juce::File writeSpecFile()
    {
        const auto specFile = juce::File::getSpecialLocation (
                                  juce::File::SpecialLocationType::tempDirectory)
                                  .getNonexistentChildFile ("pluginlab_replica_spec_", ".json");
        const juce::String jsonText = juce::String (
            "{ \"generated_at\": \"2026-08-14T00:00:00Z\","
            " \"source\": { \"aggregate_report\": \"report.md\","
            " \"dataset_dir\": null, \"report_generated_at\": \"2026-08-14T00:00:00Z\" },"
            " \"plugins\": [ { \"slug\": \"replica\", \"plugin\": \"Unit Test\","
            " \"plugin_type\": { \"kind\": \"eq-dynamics\", \"confidence\": \"high\","
            " \"basis\": [] },"
            " \"eq\": { \"present\": true, \"overall\": \"clean\","
            " \"sections\": [ { \"freq_hz\": 1000, \"gain_db\": 6, \"q\": 1.0,"
            " \"plausible\": true } ], \"notes\": [] },"
            " \"dynamics\": { \"present\": true, \"compression\":"
            " { \"threshold_derived\": -20, \"ratio_derived\": 4,"
            " \"conflict\": false },"
            " \"gr\": { \"attack_ms\": 5, \"release_ms\": 50,"
            " \"attack_plausible\": true, \"release_plausible\": true },"
            " \"notes\": [] },"
            " \"nonlinearity\": { \"verdict\": \"clean\" },"
            " \"processing_order\": { \"order\": \"eq-first\","
            " \"confidence\": \"high\", \"basis\": [] },"
            " \"usable_as_spec\": true, \"why_not_spec\": [] } ] }");
        specFile.replaceWithText (jsonText);
        return specFile;
    }

    juce::String jsonField (const juce::String& line, const juce::String& key)
    {
        const auto doc = juce::JSON::parse (line);
        if (! doc.isObject())
            return {};
        return doc[juce::Identifier (key)].toString();
    }

    int jsonIntField (const juce::String& line, const juce::String& key)
    {
        const auto doc = juce::JSON::parse (line);
        if (! doc.isObject())
            return -1;
        return static_cast<int> (doc[juce::Identifier (key)]);
    }

    //==============================================================================
    // Export JSON parsers (AC3 schema -- keys per SPEC.md / Export.cpp).

    struct RawPoint
    {
        double freq = 0.0;
        double magDB = 0.0;
    };

    std::vector<RawPoint> parseRaw (const juce::String& json)
    {
        std::vector<RawPoint> points;
        const auto doc = juce::JSON::parse (json);
        if (! doc.isObject())
            return points;
        const auto raw = doc["raw"];
        if (! raw.isArray())
            return points;
        points.reserve (static_cast<size_t> (raw.size()));
        for (int i = 0; i < raw.size(); ++i)
            points.push_back ({ static_cast<double> (raw[i]["f"]),
                                static_cast<double> (raw[i]["mag"]) });
        return points;
    }
    struct ToneJson
    {
        double fundamentalHz = 0.0;
        double fundamentalDB = 0.0;
        double thdPercent = 0.0;
    };

    std::vector<ToneJson> parseTones (const juce::String& json)
    {
        std::vector<ToneJson> tones;
        const auto doc = juce::JSON::parse (json);
        if (! doc.isObject())
            return tones;
        const auto arr = doc["tones"];
        if (! arr.isArray())
            return tones;
        tones.reserve (static_cast<size_t> (arr.size()));
        for (int i = 0; i < arr.size(); ++i)
        {
            const auto item = arr[i];
            ToneJson t;
            t.fundamentalHz = static_cast<double> (item["fundamental_hz"]);
            t.fundamentalDB = static_cast<double> (item["fundamental_db"]);
            t.thdPercent = static_cast<double> (item["thd_percent"]);
            tones.push_back (t);
        }
        return tones;
    }

    struct CompressionPoint
    {
        double inputDB = 0.0;
        double outputDB = 0.0;
        double grDB = 0.0;
    };

    std::vector<CompressionPoint> parseCurve (const juce::String& json)
    {
        std::vector<CompressionPoint> curve;
        const auto doc = juce::JSON::parse (json);
        if (! doc.isObject())
            return curve;
        const auto arr = doc["curve"];
        if (! arr.isArray())
            return curve;
        curve.reserve (static_cast<size_t> (arr.size()));
        for (int i = 0; i < arr.size(); ++i)
        {
            const auto item = arr[i];
            CompressionPoint p;
            p.inputDB  = static_cast<double> (item["input_db"]);
            p.outputDB = static_cast<double> (item["output_db"]);
            p.grDB     = static_cast<double> (item["gr_db"]);
            curve.push_back (p);
        }
        return curve;
    }

    struct GrTimelinePoint
    {
        double t = 0.0;
        double grDB = 0.0;
    };

    struct GrJson
    {
        juce::String type;
        int numPoints = -1;
        std::vector<GrTimelinePoint> timeline;
    };

    GrJson parseGrTimelineJson (const juce::String& line)
    {
        GrJson out;
        const auto doc = juce::JSON::parse (line);
        if (! doc.isObject())
            return out;
        out.type = doc["type"].toString();
        const auto gr = doc["gr"];
        if (! gr.isObject())
            return out;
        out.numPoints = static_cast<int> (gr["num_points"]);
        const auto tl = gr["timeline"];
        if (! tl.isArray())
            return out;
        out.timeline.reserve (static_cast<size_t> (tl.size()));
        for (int i = 0; i < tl.size(); ++i)
            out.timeline.push_back ({ static_cast<double> (tl[i]["t"]),
                                      static_cast<double> (tl[i]["gr_db"]) });
        return out;
    }
}  // namespace

//==============================================================================
// T4 AC1 + AC3: frequency_response (sweep) -- ok:true, non-empty raw curve,
// global peak at the spec bell centre (1000 Hz).
//==============================================================================

TEST_CASE ("ReplicaMeasurement: frequency_response via child host shows the 1 kHz spec bell",
           "[replica][measurement][integration]")
{
    // Arrange -- a temp chain_doc (full EQ + compression + GR spec) pointed at
    // by PLUGINLAB_REPLICA_SPEC (decision A primary route; the child process
    // inherits the test process environment).
    const auto pluginFile = replicaPluginFile();
    if (! pluginFile.exists())
        SKIP ("REPLICA_PLUGIN not present: " + pluginFile.getFullPathName());
    if (! realChildExe().existsAsFile())
        SKIP ("PLUGIN_HOST_CHILD_EXE not present: " + realChildExe().getFullPathName());

    const auto specFile = writeSpecFile();
    REQUIRE (specFile.existsAsFile());
    REQUIRE (_putenv_s ("PLUGINLAB_REPLICA_SPEC", specFile.getFullPathName().toRawUTF8()) == 0);

    constexpr double kSr = 48000.0;
    constexpr int kBlockSize = 512;
    constexpr double kSpecBellHz = 1000.0;   // spec eq section freq_hz
    constexpr double kBandHalfWidthHz = 100.0;   // 900-1100 Hz band vs 400-600 Hz
    constexpr double kMinBellElevationDB = 0.5;  // measured elevation ~+1.0 dB

    // Act -- child host measures frequency_response (sweep) against the replica.
    PluginHostChildCoordinator coord (realChildExe().getFullPathName());
    std::atomic<bool> crashed { false };
    coord.setOnCrash ([&] (const juce::String&) { crashed.store (true); });
    REQUIRE (coord.start());

    const auto exportPath = tempFile ("pluginlab_replica_fr_", ".json");
    const auto wavPath = tempFile ("pluginlab_replica_fr_", ".wav");

    const auto resultLine = measureViaChild (coord, pluginFile.getFullPathName(),
                                             "sweep", exportPath, wavPath, "frequency_response");
    REQUIRE_FALSE (crashed.load());
    REQUIRE (resultLine.isNotEmpty());          // {"ok":true,...,"samples":...}
    REQUIRE (resultLine.contains ("\"ok\":true"));
    REQUIRE (wavPath.existsAsFile());
    REQUIRE (wavPath.getSize() > 44);

    const auto pluginName = jsonField (resultLine, "name");
    const auto classId = jsonField (resultLine, "class_id");
    const int latency = jsonIntField (resultLine, "latency_samples");
    const int channels = jsonIntField (resultLine, "channels");
    INFO ("child plugin: name=" << pluginName << " class_id=" << classId
          << " latency_samples=" << latency << " channels=" << channels
          << " wav=" << wavPath.getFullPathName() << " (" << wavPath.getSize() << " bytes)");
    REQUIRE (pluginName.isNotEmpty());          // AC1: loadable
    REQUIRE (channels > 0);

    // The child only collects dry/wet (ADR-D-5 WAV transit); the host-side
    // ChildWavAnalyzer entry turns the WAV into the export JSON (AC3 shape).
    const auto freqJson = ChildWavAnalyzer::analyzeChildFrequencyResponse (
        wavPath, channels, kSr, kBlockSize, "sweep", 0,
        pluginName, classId, latency);
    REQUIRE (freqJson.isNotEmpty());

    // Assert -- AC3: non-empty raw point array.
    const auto raw = parseRaw (freqJson);
    REQUIRE_FALSE (raw.empty());

    // Assert -- AC1 evidence: the 1 kHz spec bell shows as a mid-band
    // elevation. The H1-on-chirp estimate (Sxy/Sxx over Hann frames) is only
    // exact for linear time-invariant systems: with the level-dependent
    // compressor, the frames near the sweep end leak a single-bin artifact
    // spike at ~18.8 kHz (+7.6 dB while its neighbours sit at the -8.5 dB
    // base), so a GLOBAL peak check is invalid here. The honest,
    // artifact-robust signal is the mean magnitude of the band around the
    // spec bell centre (900-1100 Hz) minus the adjacent 400-600 Hz band:
    // with the EQ +6 dB at 1 kHz against the compressor's extra
    // (1 - 1/ratio) x 6 dB gain reduction, the steady-state elevation is
    // +1.5 dB; the frame averaging reduces the measured elevation to ~+1.0 dB.
    double lowBandSum = 0.0; int lowBandCount = 0;
    double bellBandSum = 0.0; int bellBandCount = 0;
    for (const auto& p : raw)
    {
        if (p.freq >= 400.0 && p.freq <= 600.0)
        {
            lowBandSum += p.magDB;
            ++lowBandCount;
        }
        else if (p.freq >= kSpecBellHz - kBandHalfWidthHz
                 && p.freq <= kSpecBellHz + kBandHalfWidthHz)
        {
            bellBandSum += p.magDB;
            ++bellBandCount;
        }
    }
    REQUIRE (lowBandCount > 0);
    REQUIRE (bellBandCount > 0);
    const double bellElevationDB = bellBandSum / bellBandCount - lowBandSum / lowBandCount;
    WARN ("mean mag [400,600]=" << lowBandSum / lowBandCount
          << " [900,1100]=" << bellBandSum / bellBandCount
          << " bell elevation=" << bellElevationDB);
    REQUIRE (bellElevationDB > kMinBellElevationDB);

    coord.stop();
    exportPath.deleteFile();
    wavPath.deleteFile();
    specFile.deleteFile();
    _putenv_s ("PLUGINLAB_REPLICA_SPEC", "");
}

//==============================================================================
// T4 AC1 + AC3: harmonic_analysis (SequentialTone) -- ok:true, one tone
// result per fundamental.
//==============================================================================

TEST_CASE ("ReplicaMeasurement: harmonic_analysis via child host yields tone results per fundamental",
           "[replica][measurement][integration][harmonic]")
{
    // Arrange
    const auto pluginFile = replicaPluginFile();
    if (! pluginFile.exists())
        SKIP ("REPLICA_PLUGIN not present: " + pluginFile.getFullPathName());
    if (! realChildExe().existsAsFile())
        SKIP ("PLUGIN_HOST_CHILD_EXE not present: " + realChildExe().getFullPathName());

    const auto specFile = writeSpecFile();
    REQUIRE (specFile.existsAsFile());
    REQUIRE (_putenv_s ("PLUGINLAB_REPLICA_SPEC", specFile.getFullPathName().toRawUTF8()) == 0);

    constexpr double kSr = 48000.0;
    constexpr int kBlockSize = 512;

    // Act -- child host measures harmonic (8 fundamentals, 100..12800 Hz).
    PluginHostChildCoordinator coord (realChildExe().getFullPathName());
    std::atomic<bool> crashed { false };
    coord.setOnCrash ([&] (const juce::String&) { crashed.store (true); });
    REQUIRE (coord.start());

    const auto exportPath = tempFile ("pluginlab_replica_ha_", ".json");
    const auto wavPath = tempFile ("pluginlab_replica_ha_", ".wav");

    const auto resultLine = measureViaChild (coord, pluginFile.getFullPathName(),
                                             "sweep", exportPath, wavPath, "harmonic");
    REQUIRE_FALSE (crashed.load());
    REQUIRE (resultLine.isNotEmpty());
    REQUIRE (resultLine.contains ("\"ok\":true"));
    REQUIRE (wavPath.existsAsFile());
    REQUIRE (wavPath.getSize() > 44);

    const auto pluginName = jsonField (resultLine, "name");
    const auto classId = jsonField (resultLine, "class_id");
    const int latency = jsonIntField (resultLine, "latency_samples");
    const int channels = jsonIntField (resultLine, "channels");
    REQUIRE (pluginName.isNotEmpty());
    REQUIRE (channels > 0);

    const auto harmonicJson = ChildWavAnalyzer::analyzeChildHarmonic (
        wavPath, channels, kSr, kBlockSize, pluginName, classId, latency);
    REQUIRE (harmonicJson.isNotEmpty());

    // Assert -- AC3: one tone result per measurable fundamental. The
    // 12800 Hz segment's harmonics all fall above Nyquist and are dropped by
    // design (analyzeTone breaks when the harmonic bin leaves the FFT range),
    // so 7 of the 8 fundamentals are expected -- tolerate drift in either
    // direction around that.
    const auto tones = parseTones (harmonicJson);
    WARN ("tones=" << tones.size());
    REQUIRE (tones.size() >= 7);

    coord.stop();
    exportPath.deleteFile();
    wavPath.deleteFile();
    specFile.deleteFile();
    _putenv_s ("PLUGINLAB_REPLICA_SPEC", "");
}

//==============================================================================
// T4 AC1 + AC3: compression_curve (ToneBurst) -- ok:true, one point per burst
// level, gain reduction engaged at the high levels (spec ratio 4:1 above
// -20 dB).
//==============================================================================

TEST_CASE ("ReplicaMeasurement: compression_curve via child host shows engaged compression",
           "[replica][measurement][integration][compression]")
{
    // Arrange
    const auto pluginFile = replicaPluginFile();
    if (! pluginFile.exists())
        SKIP ("REPLICA_PLUGIN not present: " + pluginFile.getFullPathName());
    if (! realChildExe().existsAsFile())
        SKIP ("PLUGIN_HOST_CHILD_EXE not present: " + realChildExe().getFullPathName());

    const auto specFile = writeSpecFile();
    REQUIRE (specFile.existsAsFile());
    REQUIRE (_putenv_s ("PLUGINLAB_REPLICA_SPEC", specFile.getFullPathName().toRawUTF8()) == 0);

    constexpr double kSr = 48000.0;
    constexpr int kBlockSize = 512;
    constexpr double kMinEngagedGRDB = 0.5;   // spec compresses at 4:1 above -20 dB

    // Act -- child host measures compression (9 tone-burst levels).
    PluginHostChildCoordinator coord (realChildExe().getFullPathName());
    std::atomic<bool> crashed { false };
    coord.setOnCrash ([&] (const juce::String&) { crashed.store (true); });
    REQUIRE (coord.start());

    const auto exportPath = tempFile ("pluginlab_replica_cc_", ".json");
    const auto wavPath = tempFile ("pluginlab_replica_cc_", ".wav");

    const auto resultLine = measureViaChild (coord, pluginFile.getFullPathName(),
                                             "sweep", exportPath, wavPath, "compression");
    REQUIRE_FALSE (crashed.load());
    REQUIRE (resultLine.isNotEmpty());
    REQUIRE (resultLine.contains ("\"ok\":true"));
    REQUIRE (wavPath.existsAsFile());
    REQUIRE (wavPath.getSize() > 44);

    const auto pluginName = jsonField (resultLine, "name");
    const auto classId = jsonField (resultLine, "class_id");
    const int latency = jsonIntField (resultLine, "latency_samples");
    const int channels = jsonIntField (resultLine, "channels");
    REQUIRE (pluginName.isNotEmpty());
    REQUIRE (channels > 0);

    const auto compressionJson = ChildWavAnalyzer::analyzeChildCompression (
        wavPath, channels, kSr, kBlockSize, pluginName, classId, latency);
    REQUIRE (compressionJson.isNotEmpty());

    // Assert -- AC3: one curve point per burst level.
    const auto curve = parseCurve (compressionJson);
    REQUIRE (curve.size() == 9);

    // Assert -- AC1 evidence: the high-level bursts are compressed (the spec
    // threshold is -20 dB and the loudest burst reaches ~0.9 amp, so the
    // 4:1 ratio must produce real gain reduction).
    double maxGR = 0.0;
    for (const auto& p : curve)
        maxGR = std::max (maxGR, -p.grDB);
    WARN ("curve points=" << curve.size() << " max GR=" << maxGR);
    REQUIRE (maxGR > kMinEngagedGRDB);

    coord.stop();
    exportPath.deleteFile();
    wavPath.deleteFile();
    specFile.deleteFile();
    _putenv_s ("PLUGINLAB_REPLICA_SPEC", "");
}

//==============================================================================
// T4 AC1 + AC3: gr_timeline (enveloped sweep) -- ok:true, non-empty timeline
// with a driven region (the ADSR sustain drives the input above the spec
// -20 dB threshold).
//==============================================================================

TEST_CASE ("ReplicaMeasurement: gr_timeline via child host shows driven gain reduction",
           "[replica][measurement][integration][gr]")
{
    // Arrange
    const auto pluginFile = replicaPluginFile();
    if (! pluginFile.exists())
        SKIP ("REPLICA_PLUGIN not present: " + pluginFile.getFullPathName());
    if (! realChildExe().existsAsFile())
        SKIP ("PLUGIN_HOST_CHILD_EXE not present: " + realChildExe().getFullPathName());

    const auto specFile = writeSpecFile();
    REQUIRE (specFile.existsAsFile());
    REQUIRE (_putenv_s ("PLUGINLAB_REPLICA_SPEC", specFile.getFullPathName().toRawUTF8()) == 0);

    constexpr double kSr = 48000.0;
    constexpr int kBlockSize = 512;
    constexpr double kDrivenThresholdDB = 0.5;   // |gr_db| above this = driven

    // Act -- child host measures gr_timeline (its own enveloped sweep).
    PluginHostChildCoordinator coord (realChildExe().getFullPathName());
    std::atomic<bool> crashed { false };
    coord.setOnCrash ([&] (const juce::String&) { crashed.store (true); });
    REQUIRE (coord.start());

    const auto exportPath = tempFile ("pluginlab_replica_gr_", ".json");
    const auto wavPath = tempFile ("pluginlab_replica_gr_", ".wav");

    const auto resultLine = measureViaChild (coord, pluginFile.getFullPathName(),
                                             "sweep", exportPath, wavPath, "gr_timeline");
    REQUIRE_FALSE (crashed.load());
    REQUIRE (resultLine.isNotEmpty());
    REQUIRE (resultLine.contains ("\"ok\":true"));
    REQUIRE (wavPath.existsAsFile());
    REQUIRE (wavPath.getSize() > 44);

    const auto pluginName = jsonField (resultLine, "name");
    const auto classId = jsonField (resultLine, "class_id");
    const int latency = jsonIntField (resultLine, "latency_samples");
    const int channels = jsonIntField (resultLine, "channels");
    REQUIRE (pluginName.isNotEmpty());
    REQUIRE (channels > 0);

    const auto grJsonString = ChildWavAnalyzer::analyzeChildGrTimeline (
        wavPath, channels, kSr, kBlockSize, pluginName, classId, latency);
    REQUIRE (grJsonString.isNotEmpty());

    // Assert -- AC3: non-empty timeline over the recorded region.
    const auto grJson = parseGrTimelineJson (grJsonString);
    REQUIRE (grJson.type == "gr_timeline");
    REQUIRE (grJson.numPoints > 0);
    REQUIRE_FALSE (grJson.timeline.empty());

    // Assert -- AC1 evidence: a driven region exists. The enveloped sweep's
    // ADSR sustain (~0.8 x 0.5 amp) sits far above the spec -20 dB threshold,
    // so real gain reduction must appear on the timeline.
    int drivenCount = 0;
    double maxGRDB = 0.0;
    for (const auto& p : grJson.timeline)
    {
        if (p.grDB < -kDrivenThresholdDB)
        {
            ++drivenCount;
            maxGRDB = std::max (maxGRDB, -p.grDB);
        }
    }
    WARN ("num_points=" << grJson.numPoints << " driven points=" << drivenCount
          << " max GR=" << maxGRDB);
    REQUIRE (drivenCount > 0);

    coord.stop();
    exportPath.deleteFile();
    wavPath.deleteFile();
    specFile.deleteFile();
    _putenv_s ("PLUGINLAB_REPLICA_SPEC", "");
}
