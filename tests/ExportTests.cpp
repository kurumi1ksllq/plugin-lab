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
    REQUIRE (parsed["context"]["measurement"]["block_size"].equals (512));
    REQUIRE (parsed["context"]["parameter_snapshot"]["gain"].equals (0.5));

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
