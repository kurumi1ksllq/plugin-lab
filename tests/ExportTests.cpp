#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "../source/analysis/Export.h"

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
