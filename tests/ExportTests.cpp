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
