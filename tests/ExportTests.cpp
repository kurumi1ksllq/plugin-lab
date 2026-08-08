#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <cstdlib>

#include "../source/analysis/Export.h"
#include "../source/scan/ScanEngine.h"
#include "../source/capture/MeasurementSession.h"

//==============================================================================
// Export JSON tests: the exported JSON must always be parseable, carry the
// full measurement context (class_id / latency / config / parameter snapshot)
// so the AI can reproduce the measurement, and preserve numeric precision.

namespace
{
    FreqResponse::Result makeBasicResult()
    {
        FreqResponse::Result result;
        result.sampleRate = 48000.0;
        result.raw =
        {
            { 100.0,  -3.0,  12.0 },
            { 1000.0,  0.0,   0.0 },
            { 10000.0, -6.0, -45.0 }
        };
        return result;
    }
}

//==============================================================================
// Test case A (RED-critical): strings containing quotes/backslashes must be
// escaped so the emitted JSON round-trips the original value exactly.

TEST_CASE ("plugin name with quotes and backslash produces parseable JSON", "[export][escape]")
{
    FreqResponse::Result result = makeBasicResult();
    Export::Context ctx;
    ctx.pluginName = "My \"Cool\" \\Plugin";

    const auto json = Export::freqResponseToJSON (result, ctx);
    const auto parsed = juce::JSON::parse (json);

    REQUIRE (! parsed.isUndefined());
    REQUIRE (parsed["plugin"].toString() == ctx.pluginName);
}

//==============================================================================
// Test case B: all measurement-context keys are present and round-trip.

TEST_CASE ("context keys present and round-trip", "[export][context]")
{
    FreqResponse::Result result = makeBasicResult();
    Export::Context ctx;
    ctx.pluginName = "UnitTest";
    ctx.classId = "com.example.unit-test";
    ctx.latencySamples = 64;
    ctx.sampleRate = 48000.0;
    ctx.blockSize = 512;
    ctx.paramSnapshot = "{\"gain\": 0.5, \"drive\": 2.0}";

    const auto json = Export::freqResponseToJSON (result, ctx);
    const auto parsed = juce::JSON::parse (json);

    REQUIRE (! parsed.isUndefined());
    REQUIRE (parsed["class_id"].toString() == ctx.classId);
    REQUIRE (parsed["latency_samples"].equals (64));
    REQUIRE (parsed["sample_rate"].equals (48000.0));
    REQUIRE (parsed["measurement"]["sample_rate"].equals (48000.0));
    REQUIRE (parsed["measurement"]["block_size"].equals (512));
    REQUIRE (parsed["parameter_snapshot"]["gain"].equals (0.5));
    REQUIRE (parsed["parameter_snapshot"]["drive"].equals (2.0));
}

//==============================================================================
// Test case C: magnitude/phase points must serialize without precision loss.

TEST_CASE ("emitted magnitude points are exact", "[export][precision]")
{
    FreqResponse::Result result;
    result.sampleRate = 48000.0;
    result.raw = { { 1000.0, 6.0, 45.0 } };

    const auto json = Export::freqResponseToJSON (result, Export::Context{});
    const auto parsed = juce::JSON::parse (json);

    REQUIRE (! parsed.isUndefined());
    REQUIRE (parsed["raw"].size() == 1);
    REQUIRE (static_cast<double> (parsed["raw"][0]["f"]) == Catch::Approx (1000.0));
    REQUIRE (static_cast<double> (parsed["raw"][0]["mag"]) == Catch::Approx (6.0));
    REQUIRE (static_cast<double> (parsed["raw"][0]["phase"]) == Catch::Approx (45.0));
}

//==============================================================================
// Test case D: the "source" metadata block is emitted by every exporter and
// round-trips exactly.

TEST_CASE ("source block round-trips through all three exporters", "[export][source]")
{
    Export::Context ctx;
    ctx.source.type = "noise";
    ctx.source.noiseType = "pink";
    ctx.source.seed = 42;
    ctx.source.durationSec = 2.0;

    {
        const auto parsed = juce::JSON::parse (Export::freqResponseToJSON (makeBasicResult(), ctx));
        REQUIRE (! parsed.isUndefined());
        REQUIRE (parsed["source"]["type"].toString() == "noise");
        REQUIRE (parsed["source"]["noise_type"].toString() == "pink");
        REQUIRE (parsed["source"]["seed"].equals (42));
        REQUIRE (static_cast<double> (parsed["source"]["duration_sec"]) == Catch::Approx (2.0));
    }

    {
        HarmonicAnalysis::Result hr;
        hr.sampleRate = 48000.0;
        hr.tones.push_back ({ 1000.0, -6.0, 0.5, {} });

        const auto parsed = juce::JSON::parse (Export::harmonicAnalysisToJSON (hr, ctx));
        REQUIRE (! parsed.isUndefined());
        REQUIRE (parsed["source"]["type"].toString() == "noise");
        REQUIRE (parsed["source"]["seed"].equals (42));
        REQUIRE (parsed["tones"].size() == 1);
    }

    {
        CompressionCurve::Result cr;
        cr.curve.push_back ({ -60.0, -60.0, 0.0 });

        const auto parsed = juce::JSON::parse (Export::compressionCurveToJSON (cr, ctx));
        REQUIRE (! parsed.isUndefined());
        REQUIRE (parsed["source"]["type"].toString() == "noise");
        REQUIRE (parsed["source"]["seed"].equals (42));
        REQUIRE (parsed["curve"].size() == 1);
    }
}

//==============================================================================
// Test case E: rawCaptureToJSON — record-only payload with source metadata
// (no analysis; that is phase 4).

TEST_CASE ("rawCaptureToJSON emits raw_capture payload with source metadata", "[export][raw]")
{
    Export::Context ctx;
    ctx.pluginName = "UnitTest";
    ctx.classId = "com.example.raw";
    ctx.latencySamples = 32;
    ctx.sampleRate = 48000.0;
    ctx.blockSize = 256;
    ctx.paramSnapshot = "{\"gain\": 1.0}";
    ctx.source.type = "file";
    ctx.source.filePath = "C:\\audio\\test.wav";
    ctx.source.sourceSampleRate = 48000.0;
    ctx.source.resampleRatio = 1.0;
    ctx.source.durationSec = 1.0;

    const auto json = Export::rawCaptureToJSON (48000, 48000.0, 256, ctx);
    const auto parsed = juce::JSON::parse (json);

    REQUIRE (! parsed.isUndefined());
    REQUIRE (parsed["type"].toString() == "raw_capture");
    REQUIRE (static_cast<int64_t> (parsed["samples"]) == 48000);
    REQUIRE (parsed["sample_rate"].equals (48000.0));
    REQUIRE (parsed["block_size"].equals (256));
    REQUIRE (parsed["parameter_snapshot"]["gain"].equals (1.0));
    REQUIRE (parsed["source"]["type"].toString() == "file");
    REQUIRE (parsed["source"]["file_path"].toString() == ctx.source.filePath);
    REQUIRE (parsed["source"]["sample_rate"].equals (48000.0));
    REQUIRE (parsed["source"]["resample_ratio"].equals (1.0));
    REQUIRE (static_cast<double> (parsed["source"]["duration_sec"]) == Catch::Approx (1.0));
}

//==============================================================================
// Test case F: scanToJSON — parameter-scan continuity JSON. Every round
// ("family" entry) carries the per-value analysis body plus the context that
// lets the consumer reproduce the measurement.

namespace
{
    FreqResponse::Result makeFreqResult (double peakGainDB)
    {
        FreqResponse::Result result;
        result.sampleRate = 48000.0;
        result.raw =
        {
            { 100.0,   peakGainDB - 3.0,  12.0 },
            { 1000.0,  peakGainDB,        0.0 },
            { 10000.0, peakGainDB - 6.0, -45.0 }
        };
        return result;
    }

    /** 3-round gain scan: each round a distinct frequency response. */
    ScanEngine::ScanResult makeScanResult()
    {
        ScanEngine::ScanResult scan;
        scan.paramId = "gain";
        scan.paramName = "Gain";
        scan.values = { 0.0, 0.5, 1.0 };

        for (double v : scan.values)
        {
            ScanEngine::ScanResultEntry entry;
            entry.paramValue = v;
            entry.paramValueText = juce::String (v, 2) + " dB";
            entry.latencySamples = 32 + static_cast<int> (v * 100);
            entry.freq = makeFreqResult (v * 12.0);
            scan.family.push_back (entry);
        }
        return scan;
    }
}

TEST_CASE ("scanToJSON emits scan schema with context, scan and family", "[export][scan-schema]")
{
    const auto scan = makeScanResult();
    Export::Context ctx;
    ctx.pluginName = "UnitTest";
    ctx.classId = "com.example.scan";
    ctx.latencySamples = 64;
    ctx.sampleRate = 48000.0;
    ctx.blockSize = 512;
    ctx.paramSnapshot = "{\"gain\": 0.5}";

    const auto json = Export::scanToJSON (scan, MeasurementSession::Type::frequencyResponse, ctx);
    const auto parsed = juce::JSON::parse (json);

    REQUIRE (! parsed.isUndefined());
    REQUIRE (parsed["type"].toString() == "scan");

    // Context block matches the standalone exporters' context fields.
    REQUIRE (parsed["context"]["plugin"].toString() == ctx.pluginName);
    REQUIRE (parsed["context"]["class_id"].toString() == ctx.classId);
    REQUIRE (parsed["context"]["latency_samples"].equals (64));
    REQUIRE (parsed["context"]["sample_rate"].equals (48000.0));   // schema §5 top-level
    REQUIRE (parsed["context"]["measurement"]["sample_rate"].equals (48000.0));
    REQUIRE (parsed["context"]["measurement"]["block_size"].equals (512));
    REQUIRE (parsed["context"]["parameter_snapshot"]["gain"].equals (0.5));
    REQUIRE (parsed["context"]["source"]["type"].toString() == "signal");  // schema §5 default source

    REQUIRE (parsed["scan"]["param_id"].toString() == scan.paramId);
    REQUIRE (parsed["scan"]["param_name"].toString() == scan.paramName);
    REQUIRE (parsed["scan"]["values"].size() == 3);
    REQUIRE (parsed["scan"]["param_texts"].size() == 3);
    REQUIRE (parsed["family"].size() == 3);

    for (size_t i = 0; i < scan.family.size(); ++i)
    {
        const auto idx = static_cast<int> (i);
        INFO ("family entry " << i);
        REQUIRE (parsed["family"][idx]["latency_samples"].equals (scan.family[i].latencySamples));
        REQUIRE (static_cast<double> (parsed["family"][idx]["param_value_normalized"])
                 == Catch::Approx (scan.family[i].paramValue));
        REQUIRE (parsed["family"][idx]["param_value_text"].toString() == scan.family[i].paramValueText);
        REQUIRE (parsed["family"][idx]["result"].isObject());
        REQUIRE (static_cast<double> (parsed["scan"]["values"][idx])
                 == Catch::Approx (scan.values[i]));
    }
}

//==============================================================================
// Test case G: family[i].result must be data-equivalent to the standalone
// frequency-response export body (same points, same precision).

TEST_CASE ("scanToJSON family result matches standalone frequency response body",
           "[export][scan-body-equiv]")
{
    const auto freq = makeFreqResult (6.0);
    Export::Context ctx;
    ctx.pluginName = "EquivTest";
    ctx.latencySamples = 64;

    ScanEngine::ScanResult scan;
    scan.paramId = "gain";
    scan.paramName = "Gain";
    scan.values = { 0.5 };
    ScanEngine::ScanResultEntry entry;
    entry.paramValue = 0.5;
    entry.paramValueText = "0.50 dB";
    entry.latencySamples = 64;
    entry.freq = freq;
    scan.family.push_back (entry);

    const auto standalone = juce::JSON::parse (Export::freqResponseToJSON (freq, ctx));
    const auto parsed = juce::JSON::parse (
        Export::scanToJSON (scan, MeasurementSession::Type::frequencyResponse, ctx));
    const auto result = parsed["family"][0]["result"];

    REQUIRE (! parsed.isUndefined());
    REQUIRE (! result.isUndefined());
    REQUIRE (result["raw"] == standalone["raw"]);
    REQUIRE (result["smoothed_1_12"] == standalone["smoothed_1_12"]);
    REQUIRE (result["smoothed_1_3"] == standalone["smoothed_1_3"]);
}

//==============================================================================
// Test case H: every user-supplied string (context, param id/name, value
// text) is escaped so the JSON round-trips exactly.

TEST_CASE ("scanToJSON escapes quotes and backslashes", "[export][scan-escaping]")
{
    const auto scan = makeScanResult();
    Export::Context ctx;
    ctx.pluginName = "My \"Cool\" \\Plugin";
    ctx.classId = "com.example.\"scan\"";
    ctx.source.type = "file";
    ctx.source.filePath = "C:\\audio\\\"weird\".wav";

    auto tricky = scan;
    tricky.paramId = "dr\\ive\"x";
    tricky.paramName = "Dr \"X\"";
    tricky.family[0].paramValueText = "0\\\"5 dB";

    const auto json = Export::scanToJSON (tricky, MeasurementSession::Type::frequencyResponse, ctx);
    const auto parsed = juce::JSON::parse (json);

    REQUIRE (! parsed.isUndefined());
    REQUIRE (parsed["context"]["plugin"].toString() == ctx.pluginName);
    REQUIRE (parsed["context"]["class_id"].toString() == ctx.classId);
    REQUIRE (parsed["context"]["source"]["file_path"].toString() == ctx.source.filePath);
    REQUIRE (parsed["scan"]["param_id"].toString() == tricky.paramId);
    REQUIRE (parsed["scan"]["param_name"].toString() == tricky.paramName);
    REQUIRE (parsed["family"][0]["param_value_text"].toString() == tricky.family[0].paramValueText);
}

//==============================================================================
// Test case I: the emitted JSON is parseable by python's stdlib json module
// (the same interpreter that consumes the other exports via verify_export.py).

TEST_CASE ("scanToJSON output parses with python json.load", "[export][scan-python]")
{
    const auto scan = makeScanResult();
    Export::Context ctx;
    ctx.pluginName = "PythonScan";

    const auto json = Export::scanToJSON (scan, MeasurementSession::Type::frequencyResponse, ctx);
    const auto file = juce::File::getSpecialLocation (juce::File::tempDirectory)
                          .getChildFile ("pluginlab_scan_test.json");
    REQUIRE (Export::writeToFile (json, file));

    const auto command = "python -c \"import json,sys; json.load(open(sys.argv[1]))\" "
                         + file.getFullPathName().quoted();
    const auto exitCode = std::system (command.toRawUTF8());
    file.deleteFile();

    REQUIRE (exitCode == 0);
}

//==============================================================================
// Test case J: an empty family (cancelled before the first round) still
// yields parseable JSON with family == [].

TEST_CASE ("scanToJSON handles empty family", "[export][scan-empty-family]")
{
    ScanEngine::ScanResult scan;
    scan.paramId = "gain";
    scan.paramName = "Gain";
    scan.values = { 0.0, 0.5, 1.0 };

    Export::Context ctx;
    ctx.pluginName = "EmptyScan";

    const auto json = Export::scanToJSON (scan, MeasurementSession::Type::compressionCurve, ctx);
    const auto parsed = juce::JSON::parse (json);

    REQUIRE (! parsed.isUndefined());
    REQUIRE (parsed["family"].size() == 0);
    REQUIRE (parsed["scan"]["values"].size() == 3);
    REQUIRE (parsed["scan"]["param_texts"].size() == 0);
}

//==============================================================================
// Test case K (T4.4): grTimelineToJSON — GR timeline + tau export. The
// schema carries the nested context block, the gr timeline and the tau
// summary (attack/release sec + valid flag + tau(level) curve families).

TEST_CASE ("grTimelineToJSON emits gr_timeline schema with gr and tau blocks", "[export][gr-schema]")
{
    GainReduction::Result gr;
    gr.sampleRate = 48000.0;
    gr.timeline =
    {
        { 0.000, -6.0 },
        { 0.005, -6.0 },
        { 0.010, -6.02 }
    };
    gr.numPoints = static_cast<int> (gr.timeline.size());

    TimeConstants::Result tau;
    tau.tauAttackSec  = 0.001;
    tau.tauReleaseSec = 0.05;
    tau.attackByLevel.levelDB = { -6.0 };
    tau.attackByLevel.tauSec  = { 0.001 };
    tau.releaseByLevel.levelDB = { -6.0, -3.0 };
    tau.releaseByLevel.tauSec  = { 0.05, 0.049 };
    tau.valid = true;

    Export::Context ctx;
    ctx.pluginName = "UnitTest";
    ctx.classId = "com.example.gr";
    ctx.latencySamples = 0;
    ctx.sampleRate = 48000.0;
    ctx.blockSize = 512;
    ctx.source.type = "dynamic";

    const auto json = Export::grTimelineToJSON (gr, tau, ctx);
    const auto parsed = juce::JSON::parse (json);

    REQUIRE (! parsed.isUndefined());
    REQUIRE (parsed["type"].toString() == "gr_timeline");

    // Nested context block (mirrors scanToJSON / compressionFamilyToJSON).
    REQUIRE (parsed["context"]["plugin"].toString() == ctx.pluginName);
    REQUIRE (parsed["context"]["class_id"].toString() == ctx.classId);
    REQUIRE (parsed["context"]["latency_samples"].equals (0));
    REQUIRE (parsed["context"]["source"]["type"].toString() == "dynamic");

    // gr block: sample_rate + timeline points round-trip exactly.
    REQUIRE (parsed["gr"]["sample_rate"].equals (48000.0));
    REQUIRE (parsed["gr"]["timeline"].size() == 3);
    REQUIRE (static_cast<double> (parsed["gr"]["timeline"][1]["t"])
             == Catch::Approx (0.005));
    REQUIRE (static_cast<double> (parsed["gr"]["timeline"][1]["gr_db"])
             == Catch::Approx (-6.0));

    // tau block: attack/release seconds, valid flag and by-level families.
    REQUIRE (static_cast<double> (parsed["tau"]["attack_sec"])
             == Catch::Approx (0.001));
    REQUIRE (static_cast<double> (parsed["tau"]["release_sec"])
             == Catch::Approx (0.05));
    REQUIRE (parsed["tau"]["valid"].isBool());
    REQUIRE ((bool) parsed["tau"]["valid"]);
    REQUIRE (parsed["tau"]["attack_by_level"].size() == 1);
    REQUIRE (static_cast<double> (parsed["tau"]["attack_by_level"][0]["level_db"])
             == Catch::Approx (-6.0));
    REQUIRE (static_cast<double> (parsed["tau"]["attack_by_level"][0]["tau_sec"])
             == Catch::Approx (0.001));
    REQUIRE (parsed["tau"]["release_by_level"].size() == 2);
    REQUIRE (static_cast<double> (parsed["tau"]["release_by_level"][1]["tau_sec"])
             == Catch::Approx (0.049));
}

//==============================================================================
// Test case L (T4.4): an invalid tau result (no detected edges) still emits a
// well-formed tau block: zeros + valid=false + empty curve families.

TEST_CASE ("grTimelineToJSON emits invalid tau block for edgeless GR", "[export][gr-tau-invalid]")
{
    GainReduction::Result gr;
    gr.sampleRate = 48000.0;
    gr.timeline = { { 0.0, 0.0 }, { 0.005, 0.0 } };
    gr.numPoints = 2;

    TimeConstants::Result tau;  // default: all zeros, valid=false

    Export::Context ctx;
    ctx.pluginName = "UnitTest";

    const auto json = Export::grTimelineToJSON (gr, tau, ctx);
    const auto parsed = juce::JSON::parse (json);

    REQUIRE (! parsed.isUndefined());
    REQUIRE (! (bool) parsed["tau"]["valid"]);
    REQUIRE (static_cast<double> (parsed["tau"]["attack_sec"]) == Catch::Approx (0.0));
    REQUIRE (static_cast<double> (parsed["tau"]["release_sec"]) == Catch::Approx (0.0));
    REQUIRE (parsed["tau"]["attack_by_level"].size() == 0);
    REQUIRE (parsed["tau"]["release_by_level"].size() == 0);
}

//==============================================================================
// Test cases M..R (T5.1/T5.2): datasetToJSON — a modelling data package that
// aggregates the parameter-scan family, the compression-response family and
// the GR timeline into one self-contained JSON document for AI modelling.
// Every measurement is optional; the caller fills only completed ones and
// omitted ones are not emitted.

namespace
{
    /** 2×2 compression grid: levels × speeds, each cell a static curve + GR
     *  timeline + time constants. */
    CompressionFamily::FamilyResult makeFamilyResult()
    {
        CompressionFamily::FamilyResult family;
        family.levelsDB = { -12.0, -6.0 };
        family.speeds = { 1.0, 2.0 };

        for (double level : family.levelsDB)
        {
            for (double speed : family.speeds)
            {
                CompressionFamily::FamilyEntry entry;
                entry.inputLevelDB = level;
                entry.speed = speed;
                entry.curve.curve =
                {
                    { level, level, 0.0 },
                    { level + 6.0, level + 1.0, 5.0 }
                };
                entry.curve.fitted.ratio = 2.0;
                entry.curve.fitted.thresholdDB = level;
                entry.curve.fitted.kneeDB = 0.0;
                entry.gr.sampleRate = 48000.0;
                entry.gr.timeline = { { 0.0, -1.0 }, { 0.005, -2.0 } };
                entry.gr.numPoints = static_cast<int> (entry.gr.timeline.size());
                entry.tau.tauAttackSec = 0.001;
                entry.tau.tauReleaseSec = 0.05;
                entry.tau.valid = true;
                entry.valid = true;
                family.entries.push_back (entry);
            }
        }
        return family;
    }

    /** GR timeline in the GainReduction convention (negative dB = reduction),
     *  the convention carried by grTimelineToJSON. */
    GainReduction::Result makeGRResult()
    {
        GainReduction::Result gr;
        gr.sampleRate = 48000.0;
        gr.timeline = { { 0.0, 0.0 }, { 0.005, -6.0 }, { 0.010, -6.0 } };
        gr.numPoints = static_cast<int> (gr.timeline.size());
        return gr;
    }

    TimeConstants::Result makeTauResult()
    {
        TimeConstants::Result tau;
        tau.tauAttackSec = 0.002;
        tau.tauReleaseSec = 0.04;
        tau.valid = true;
        return tau;
    }
}

//==============================================================================
// Test case M: scan-only dataset — the scan block carries param metadata,
// values, texts and the full family; family[0].result.raw is data-equivalent
// to the standalone scanToJSON export body.

TEST_CASE ("datasetToJSON emits dataset schema with scan family", "[export][dataset-scan-only]")
{
    const auto scan = makeScanResult();
    Export::Context ctx;
    ctx.pluginName = "UnitTest";
    ctx.classId = "com.example.dataset";
    ctx.latencySamples = 64;
    ctx.sampleRate = 48000.0;
    ctx.blockSize = 512;

    Export::Dataset dataset;
    dataset.scan = &scan;
    dataset.scanType = MeasurementSession::Type::frequencyResponse;

    const auto json = Export::datasetToJSON (dataset, ctx);
    const auto parsed = juce::JSON::parse (json);

    REQUIRE (! parsed.isUndefined());
    REQUIRE (parsed["type"].toString() == "dataset");
    REQUIRE (parsed["context"]["plugin"].toString() == ctx.pluginName);
    REQUIRE (parsed["scan"]["param_id"].toString() == scan.paramId);
    REQUIRE (parsed["scan"]["param_name"].toString() == scan.paramName);
    REQUIRE (parsed["scan"]["values"].size() == 3);
    REQUIRE (parsed["scan"]["param_texts"].size() == 3);
    REQUIRE (parsed["scan"]["family"].size() == 3);
    REQUIRE (parsed["scan"]["family"][0]["result"]["raw"].size() == 3);
    REQUIRE (static_cast<double> (parsed["scan"]["family"][1]["result"]["raw"][1]["mag"])
             == Catch::Approx (6.0));  // round 2 peak = 0.5 * 12

    // Body equivalence: the dataset scan block carries exactly the same data
    // as the standalone scanToJSON export.
    const auto standalone = juce::JSON::parse (
        Export::scanToJSON (scan, MeasurementSession::Type::frequencyResponse, ctx));
    REQUIRE (! standalone.isUndefined());
    REQUIRE (parsed["scan"]["family"] == standalone["family"]);
    REQUIRE (parsed["scan"]["values"] == standalone["scan"]["values"]);
    REQUIRE (parsed["scan"]["param_texts"] == standalone["scan"]["param_texts"]);

    // No other measurement keys present.
    REQUIRE (! parsed.hasProperty ("compression_family"));
    REQUIRE (! parsed.hasProperty ("gr_timeline"));
}

//==============================================================================
// Test case N: GR-only dataset — the gr_timeline block carries the gr body
// (timeline round-trips) and the tau body (attack/release sec); no scan key.

TEST_CASE ("datasetToJSON emits gr_timeline block without scan", "[export][dataset-gr-only]")
{
    const auto gr = makeGRResult();
    const auto tau = makeTauResult();
    Export::Context ctx;
    ctx.pluginName = "UnitTest";
    ctx.sampleRate = 48000.0;

    Export::Dataset dataset;
    dataset.grTimeline = &gr;
    dataset.grTau = &tau;

    const auto json = Export::datasetToJSON (dataset, ctx);
    const auto parsed = juce::JSON::parse (json);

    REQUIRE (! parsed.isUndefined());
    REQUIRE (parsed["type"].toString() == "dataset");
    REQUIRE (parsed["gr_timeline"]["gr"]["timeline"].size() == 3);
    REQUIRE (static_cast<double> (parsed["gr_timeline"]["gr"]["timeline"][1]["t"])
             == Catch::Approx (0.005));
    REQUIRE (static_cast<double> (parsed["gr_timeline"]["gr"]["timeline"][1]["gr_db"])
             == Catch::Approx (-6.0));
    REQUIRE (static_cast<double> (parsed["gr_timeline"]["tau"]["attack_sec"])
             == Catch::Approx (0.002));
    REQUIRE (static_cast<double> (parsed["gr_timeline"]["tau"]["release_sec"])
             == Catch::Approx (0.04));
    REQUIRE ((bool) parsed["gr_timeline"]["tau"]["valid"]);
    REQUIRE (! parsed.hasProperty ("scan"));
    REQUIRE (! parsed.hasProperty ("compression_family"));
}

//==============================================================================
// Test case O: compression-family-only dataset — the compression_family block
// carries the full level × speed grid, every entry with a non-empty curve.

TEST_CASE ("datasetToJSON emits compression_family grid", "[export][dataset-compression-only]")
{
    const auto family = makeFamilyResult();
    Export::Context ctx;
    ctx.pluginName = "UnitTest";

    Export::Dataset dataset;
    dataset.compressionFamily = &family;

    const auto json = Export::datasetToJSON (dataset, ctx);
    const auto parsed = juce::JSON::parse (json);

    REQUIRE (! parsed.isUndefined());
    REQUIRE (parsed["type"].toString() == "dataset");
    REQUIRE (parsed["compression_family"]["family"].size() == 4);
    REQUIRE (static_cast<double> (parsed["compression_family"]["family"][0]["input_level_db"])
             == Catch::Approx (-12.0));
    REQUIRE (static_cast<double> (parsed["compression_family"]["family"][0]["speed"])
             == Catch::Approx (1.0));
    REQUIRE (static_cast<double> (parsed["compression_family"]["family"][3]["speed"])
             == Catch::Approx (2.0));

    for (int i = 0; i < 4; ++i)
    {
        INFO ("family entry " << i);
        REQUIRE (parsed["compression_family"]["family"][i]["curve"].size() == 2);
        REQUIRE (parsed["compression_family"]["family"][i]["gr"]["timeline"].size() == 2);
    }

    REQUIRE (! parsed.hasProperty ("scan"));
    REQUIRE (! parsed.hasProperty ("gr_timeline"));
}

//==============================================================================
// Test case P: full dataset — all three measurements plus the note; every key
// is present and the note round-trips exactly.

TEST_CASE ("datasetToJSON with all measurements and note", "[export][dataset-full]")
{
    const auto scan = makeScanResult();
    const auto family = makeFamilyResult();
    const auto gr = makeGRResult();
    const auto tau = makeTauResult();
    Export::Context ctx;
    ctx.pluginName = "UnitTest";
    ctx.classId = "com.example.full";
    ctx.latencySamples = 64;
    ctx.sampleRate = 48000.0;

    Export::Dataset dataset;
    dataset.scan = &scan;
    dataset.scanType = MeasurementSession::Type::frequencyResponse;
    dataset.compressionFamily = &family;
    dataset.grTimeline = &gr;
    dataset.grTau = &tau;
    dataset.measurementNote = "detected peak 993Hz +6.0dB -> likely bell @1k Q1";

    const auto json = Export::datasetToJSON (dataset, ctx);
    const auto parsed = juce::JSON::parse (json);

    REQUIRE (! parsed.isUndefined());
    REQUIRE (parsed["type"].toString() == "dataset");
    REQUIRE (parsed["note"].toString() == dataset.measurementNote);
    REQUIRE (parsed["context"]["class_id"].toString() == ctx.classId);
    REQUIRE (parsed["scan"]["family"].size() == 3);
    REQUIRE (parsed["compression_family"]["family"].size() == 4);
    REQUIRE (parsed["gr_timeline"]["gr"]["timeline"].size() == 3);
    REQUIRE (static_cast<double> (parsed["gr_timeline"]["tau"]["attack_sec"])
             == Catch::Approx (0.002));
}

//==============================================================================
// Test case Q: empty dataset — no measurements provided; only type/context
// survive, and the document is still parseable.

TEST_CASE ("datasetToJSON with no measurements emits only type and context", "[export][dataset-empty]")
{
    Export::Context ctx;
    ctx.pluginName = "UnitTest";
    ctx.classId = "com.example.empty";

    Export::Dataset dataset;

    const auto json = Export::datasetToJSON (dataset, ctx);
    const auto parsed = juce::JSON::parse (json);

    REQUIRE (! parsed.isUndefined());
    REQUIRE (parsed["type"].toString() == "dataset");
    REQUIRE (parsed["context"]["plugin"].toString() == ctx.pluginName);
    REQUIRE (! parsed.hasProperty ("scan"));
    REQUIRE (! parsed.hasProperty ("compression_family"));
    REQUIRE (! parsed.hasProperty ("gr_timeline"));
    REQUIRE (! parsed.hasProperty ("note"));
}

//==============================================================================
// Test case R: regression lock — every dataset body is data-equivalent to the
// corresponding standalone export (scanToJSON / grTimelineToJSON /
// compressionFamilyToJSON), proving the aggregation reuses the exact same
// serialization.

TEST_CASE ("dataset bodies are data-equivalent to standalone exports", "[export][dataset-body-equiv]")
{
    const auto scan = makeScanResult();
    const auto family = makeFamilyResult();
    const auto gr = makeGRResult();
    const auto tau = makeTauResult();
    Export::Context ctx;
    ctx.pluginName = "UnitTest";
    ctx.classId = "com.example.equiv";
    ctx.latencySamples = 32;
    ctx.sampleRate = 48000.0;
    ctx.blockSize = 256;

    Export::Dataset dataset;
    dataset.scan = &scan;
    dataset.scanType = MeasurementSession::Type::frequencyResponse;
    dataset.compressionFamily = &family;
    dataset.grTimeline = &gr;
    dataset.grTau = &tau;

    const auto parsed = juce::JSON::parse (Export::datasetToJSON (dataset, ctx));
    const auto scanStd = juce::JSON::parse (
        Export::scanToJSON (scan, MeasurementSession::Type::frequencyResponse, ctx));
    const auto grStd = juce::JSON::parse (Export::grTimelineToJSON (gr, tau, ctx));
    const auto famStd = juce::JSON::parse (Export::compressionFamilyToJSON (family, ctx));

    REQUIRE (! parsed.isUndefined());
    REQUIRE (! scanStd.isUndefined());
    REQUIRE (! grStd.isUndefined());
    REQUIRE (! famStd.isUndefined());

    REQUIRE (parsed["scan"]["family"] == scanStd["family"]);
    REQUIRE (parsed["gr_timeline"]["gr"] == grStd["gr"]);
    REQUIRE (parsed["gr_timeline"]["tau"] == grStd["tau"]);
    REQUIRE (parsed["compression_family"]["family"] == famStd["family"]);
}

//==============================================================================
// Test case S: regression lock — the frequency_response / harmonic /
// compression dataset blocks (battery measurements aggregated into the
// dataset document) are data-equivalent to the standalone exports
// (freqResponseToJSON / harmonicAnalysisToJSON / compressionCurveToJSON),
// proving the aggregation reuses the exact same body serialization helpers.

TEST_CASE ("dataset freq/harmonic/compression blocks are data-equivalent to standalone exports",
           "[export][dataset-body-equiv]")
{
    const auto freq = makeBasicResult();

    HarmonicAnalysis::Result harmonic;
    harmonic.sampleRate = 48000.0;
    harmonic.tones.push_back (
        { 1000.0, -6.0, 0.5,
          { { 1, 1000.0, -6.0, 100.0 },
            { 2, 2000.0, -24.0, 4.0 } } });

    CompressionCurve::Result compression;
    compression.curve = { { -60.0, -60.0, 0.0 }, { -54.0, -56.0, 2.0 } };
    compression.fitted.ratio = 2.5;
    compression.fitted.thresholdDB = -20.0;
    compression.fitted.kneeDB = 1.0;

    Export::Context ctx;
    ctx.pluginName = "UnitTest";
    ctx.classId = "com.example.equiv";
    ctx.latencySamples = 32;
    ctx.sampleRate = 48000.0;
    ctx.blockSize = 256;

    Export::Dataset dataset;
    dataset.freq = &freq;
    dataset.harmonic = &harmonic;
    dataset.compression = &compression;

    const auto parsed = juce::JSON::parse (Export::datasetToJSON (dataset, ctx));
    const auto freqStd = juce::JSON::parse (Export::freqResponseToJSON (freq, ctx));
    const auto harmStd = juce::JSON::parse (Export::harmonicAnalysisToJSON (harmonic, ctx));
    const auto compStd = juce::JSON::parse (Export::compressionCurveToJSON (compression, ctx));

    REQUIRE (! parsed.isUndefined());
    REQUIRE (! freqStd.isUndefined());
    REQUIRE (! harmStd.isUndefined());
    REQUIRE (! compStd.isUndefined());

    // frequency_response block == standalone frequency-response export body.
    REQUIRE (parsed["frequency_response"]["raw"] == freqStd["raw"]);
    REQUIRE (parsed["frequency_response"]["smoothed_1_12"] == freqStd["smoothed_1_12"]);
    REQUIRE (parsed["frequency_response"]["smoothed_1_3"] == freqStd["smoothed_1_3"]);

    // harmonic block == standalone harmonic-analysis export body.
    REQUIRE (parsed["harmonic"]["tones"] == harmStd["tones"]);

    // compression block == standalone compression-curve export body.
    REQUIRE (parsed["compression"]["curve"] == compStd["curve"]);
    REQUIRE (parsed["compression"]["fitted"] == compStd["fitted"]);

    // No other measurement keys present.
    REQUIRE (! parsed.hasProperty ("scan"));
    REQUIRE (! parsed.hasProperty ("compression_family"));
    REQUIRE (! parsed.hasProperty ("gr_timeline"));
}

//==============================================================================
// Test case T: full battery — freq + harmonic + compression + gr_timeline
// aggregate into a single valid JSON document containing all four blocks.

TEST_CASE ("dataset with freq/harmonic/compression/gr_timeline emits all four blocks",
           "[export][dataset-all-four]")
{
    const auto freq = makeBasicResult();

    HarmonicAnalysis::Result harmonic;
    harmonic.sampleRate = 48000.0;
    harmonic.tones.push_back (
        { 1000.0, -6.0, 0.5,
          { { 1, 1000.0, -6.0, 100.0 } } });

    CompressionCurve::Result compression;
    compression.curve = { { -60.0, -60.0, 0.0 } };
    compression.fitted.ratio = 2.0;
    compression.fitted.thresholdDB = -60.0;
    compression.fitted.kneeDB = 0.0;

    const auto gr = makeGRResult();
    const auto tau = makeTauResult();

    Export::Context ctx;
    ctx.pluginName = "UnitTest";
    ctx.classId = "com.example.four";
    ctx.latencySamples = 32;
    ctx.sampleRate = 48000.0;

    Export::Dataset dataset;
    dataset.freq = &freq;
    dataset.harmonic = &harmonic;
    dataset.compression = &compression;
    dataset.grTimeline = &gr;
    dataset.grTau = &tau;

    const auto json = Export::datasetToJSON (dataset, ctx);
    const auto parsed = juce::JSON::parse (json);

    REQUIRE (! parsed.isUndefined());
    REQUIRE (parsed["type"].toString() == "dataset");
    REQUIRE (parsed["frequency_response"].isObject());
    REQUIRE (parsed["harmonic"].isObject());
    REQUIRE (parsed["compression"].isObject());
    REQUIRE (parsed["gr_timeline"].isObject());

    // Spot-check each block carries its data.
    REQUIRE (parsed["frequency_response"]["raw"].size() == 3);
    REQUIRE (parsed["harmonic"]["tones"].size() == 1);
    REQUIRE (parsed["compression"]["curve"].size() == 1);
    REQUIRE (parsed["gr_timeline"]["gr"]["timeline"].size() == 3);
    REQUIRE (static_cast<double> (parsed["gr_timeline"]["tau"]["attack_sec"])
             == Catch::Approx (0.002));
}
