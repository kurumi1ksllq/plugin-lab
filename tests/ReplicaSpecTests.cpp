#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <optional>

#include "../source/replica/ReplicaSpec.h"

//==============================================================================
// Tests for ReplicaSpec::fromChainDoc — the chain_doc (describe_chain.py,
// CONTRACT_VERSION="2") -> ReplicaSpec parser that feeds the spec-driven VST3
// replica plugin (issue #27 T4).
//
// Parser semantics: the FIRST plugins[] entry with usable_as_spec == true
// wins; eq.sections are kept individually when plausible == true and
// freq_hz/gain_db/q are all present non-None numbers; compression maps from
// threshold_derived/ratio_derived; GR maps from attack_ms/release_ms with
// ms -> s conversion. nullopt is reserved for unparseable JSON or no usable
// entry; a usable entry without eq/dynamics content yields an identity spec
// (hasEq=false, hasCompression=false), never nullopt. Malformed input never
// throws.

namespace
{
//==============================================================================
/** Wraps one or more plugin entries into a minimal valid chain_doc. */
juce::String makeChainDoc (const juce::String& pluginEntries)
{
    return juce::String ("{ \"generated_at\": \"2026-08-14T00:00:00Z\","
                         " \"source\": { \"aggregate_report\": \"report.md\","
                         " \"dataset_dir\": null,"
                         " \"report_generated_at\": \"2026-08-14T00:00:00Z\" },"
                         " \"plugins\": [ ")
         + pluginEntries + " ] }";
}

/**
 * Builds one full plugin entry. eqSectionsJson is the raw JSON of the
 * "sections" array; compressionJson / grJson are the raw objects of
 * dynamics.compression / dynamics.gr.
 */
juce::String makePluginEntry (const juce::String& slug, bool usableAsSpec,
                              const juce::String& eqSectionsJson,
                              const juce::String& compressionJson,
                              const juce::String& grJson)
{
    return juce::String ("{ \"slug\": \"") + slug
         + "\", \"plugin\": \"Unit Test\","
         + " \"plugin_type\": { \"kind\": \"eq-dynamics\","
         + " \"confidence\": \"high\", \"basis\": [] },"
         + " \"eq\": { \"present\": true, \"overall\": \"clean\","
         + " \"sections\": " + eqSectionsJson + ", \"notes\": [] },"
         + " \"dynamics\": { \"present\": true, \"compression\": "
         + compressionJson + ", \"gr\": " + grJson + ", \"notes\": [] },"
         + " \"nonlinearity\": { \"verdict\": \"clean\" },"
         + " \"processing_order\": { \"order\": \"eq-first\","
         + " \"confidence\": \"high\", \"basis\": [] },"
         + " \"usable_as_spec\": "
         + (usableAsSpec ? juce::String ("true") : juce::String ("false"))
         + ", \"why_not_spec\": [] }";
}

/** Plugin entry with no usable content: empty sections, null dynamics. */
juce::String makeBareEntry (const juce::String& slug, bool usableAsSpec)
{
    return makePluginEntry (slug, usableAsSpec, "[ ]",
        "{ \"threshold_derived\": null, \"ratio_derived\": null,"
        " \"conflict\": false }",
        "{ \"attack_ms\": null, \"release_ms\": null,"
        " \"attack_plausible\": false, \"release_plausible\": false }");
}
} // namespace

//==============================================================================
// Test case (a): a full chain_doc maps every field, including ms -> s.

TEST_CASE ("ReplicaSpec: full chain_doc maps to complete spec", "[replica][spec]")
{
    // Arrange
    const auto json = makeChainDoc (makePluginEntry ("full", true,
        "[ { \"freq_hz\": 1000, \"gain_db\": 6, \"q\": 1.0,"
        " \"plausible\": true } ]",
        "{ \"threshold_derived\": -20, \"ratio_derived\": 4,"
        " \"conflict\": false }",
        "{ \"attack_ms\": 5, \"release_ms\": 50,"
        " \"attack_plausible\": true, \"release_plausible\": true }"));

    // Act
    const auto spec = ReplicaSpec::fromChainDoc (json);

    // Assert
    REQUIRE (spec.has_value());
    REQUIRE (spec->hasEq);
    REQUIRE (spec->bands.size() == 1);
    CHECK (spec->bands[0].freqHz == Catch::Approx (1000.0).margin (1e-9));
    CHECK (spec->bands[0].gainDB == Catch::Approx (6.0).margin (1e-9));
    CHECK (spec->bands[0].q == Catch::Approx (1.0).margin (1e-9));
    REQUIRE (spec->hasCompression);
    CHECK (spec->thresholdDB == Catch::Approx (-20.0).margin (1e-9));
    CHECK (spec->ratio == Catch::Approx (4.0).margin (1e-9));
    CHECK (spec->attackSec == Catch::Approx (0.005).margin (1e-9));
    CHECK (spec->releaseSec == Catch::Approx (0.05).margin (1e-9));
}

//==============================================================================
// Test case (b): implausible, None-valued and wrongly-typed sections are
// skipped individually; the remaining good section is kept.

TEST_CASE ("ReplicaSpec: implausible and None eq sections are filtered individually", "[replica][spec][eq]")
{
    // Arrange
    const auto json = makeChainDoc (makePluginEntry ("partial", true,
        "[ { \"freq_hz\": 1000, \"gain_db\": 6, \"q\": 1.0,"
        " \"plausible\": true },"
        "  { \"freq_hz\": 2000, \"gain_db\": 3, \"q\": 1.5,"
        " \"plausible\": false },"
        "  { \"freq_hz\": null, \"gain_db\": 3, \"q\": 1.5,"
        " \"plausible\": true },"
        "  { \"freq_hz\": \"abc\", \"gain_db\": 3, \"q\": 1.5,"
        " \"plausible\": true } ]",
        "{ \"threshold_derived\": null, \"ratio_derived\": null,"
        " \"conflict\": false }",
        "{ \"attack_ms\": null, \"release_ms\": null,"
        " \"attack_plausible\": false, \"release_plausible\": false }"));

    // Act
    const auto spec = ReplicaSpec::fromChainDoc (json);

    // Assert
    REQUIRE (spec.has_value());
    REQUIRE (spec->hasEq);
    REQUIRE (spec->bands.size() == 1);
    CHECK (spec->bands[0].freqHz == Catch::Approx (1000.0).margin (1e-9));
    CHECK (spec->bands[0].gainDB == Catch::Approx (6.0).margin (1e-9));
    CHECK (spec->bands[0].q == Catch::Approx (1.0).margin (1e-9));
    REQUIRE_FALSE (spec->hasCompression);
    CHECK (spec->thresholdDB == 0.0);
    CHECK (spec->ratio == 1.0);
    CHECK (spec->attackSec == 0.0);
    CHECK (spec->releaseSec == 0.0);
}

//==============================================================================
// Test case (c): all entries unusable -> nullopt.

TEST_CASE ("ReplicaSpec: no usable_as_spec entry yields nullopt", "[replica][spec][selection]")
{
    // Arrange
    const auto json = makeChainDoc (makeBareEntry ("a", false) + ", "
                                    + makeBareEntry ("b", false));

    // Act
    const auto spec = ReplicaSpec::fromChainDoc (json);

    // Assert
    REQUIRE_FALSE (spec.has_value());
}

//==============================================================================
// Test case (d): malformed JSON, missing plugins key and an empty plugins
// array all yield nullopt without throwing.

TEST_CASE ("ReplicaSpec: malformed or plugin-less chain_doc yields nullopt without throwing", "[replica][spec][defensive]")
{
    // Act + Assert
    const auto malformed = ReplicaSpec::fromChainDoc ("not json{{{");
    REQUIRE_FALSE (malformed.has_value());

    const auto noPlugins = ReplicaSpec::fromChainDoc (
        "{ \"generated_at\": \"2026-08-14T00:00:00Z\","
        " \"source\": { \"aggregate_report\": \"report.md\","
        " \"dataset_dir\": null,"
        " \"report_generated_at\": \"2026-08-14T00:00:00Z\" } }");
    REQUIRE_FALSE (noPlugins.has_value());

    const auto emptyPlugins = ReplicaSpec::fromChainDoc (makeChainDoc (""));
    REQUIRE_FALSE (emptyPlugins.has_value());
}

//==============================================================================
// Test case (e): the FIRST usable_as_spec entry wins, even when a later
// entry is also usable; an earlier unusable entry is skipped.

TEST_CASE ("ReplicaSpec: first usable entry wins over later usable entries", "[replica][spec][selection]")
{
    // Arrange
    const auto json = makeChainDoc (
        makePluginEntry ("first", true,
            "[ { \"freq_hz\": 1000, \"gain_db\": 6, \"q\": 1.0,"
            " \"plausible\": true } ]",
            "{ \"threshold_derived\": -20, \"ratio_derived\": 4,"
            " \"conflict\": false }",
            "{ \"attack_ms\": 5, \"release_ms\": 50,"
            " \"attack_plausible\": true, \"release_plausible\": true }")
        + ", "
        + makePluginEntry ("second", true,
            "[ { \"freq_hz\": 500, \"gain_db\": 2, \"q\": 0.7,"
            " \"plausible\": true } ]",
            "{ \"threshold_derived\": -30, \"ratio_derived\": 8,"
            " \"conflict\": false }",
            "{ \"attack_ms\": 10, \"release_ms\": 200,"
            " \"attack_plausible\": true, \"release_plausible\": true }"));

    // Act
    const auto spec = ReplicaSpec::fromChainDoc (json);

    // Assert
    REQUIRE (spec.has_value());
    REQUIRE (spec->bands.size() == 1);
    CHECK (spec->bands[0].freqHz == Catch::Approx (1000.0).margin (1e-9));
    CHECK (spec->thresholdDB == Catch::Approx (-20.0).margin (1e-9));
    CHECK (spec->ratio == Catch::Approx (4.0).margin (1e-9));
    CHECK (spec->attackSec == Catch::Approx (0.005).margin (1e-9));
    CHECK (spec->releaseSec == Catch::Approx (0.05).margin (1e-9));
}

TEST_CASE ("ReplicaSpec: unusable first entry is skipped for the usable second entry", "[replica][spec][selection]")
{
    // Arrange
    const auto json = makeChainDoc (
        makeBareEntry ("first", false)
        + ", "
        + makePluginEntry ("second", true,
            "[ { \"freq_hz\": 500, \"gain_db\": 2, \"q\": 0.7,"
            " \"plausible\": true } ]",
            "{ \"threshold_derived\": -30, \"ratio_derived\": 8,"
            " \"conflict\": false }",
            "{ \"attack_ms\": 10, \"release_ms\": 200,"
            " \"attack_plausible\": true, \"release_plausible\": true }"));

    // Act
    const auto spec = ReplicaSpec::fromChainDoc (json);

    // Assert
    REQUIRE (spec.has_value());
    REQUIRE (spec->bands.size() == 1);
    CHECK (spec->bands[0].freqHz == Catch::Approx (500.0).margin (1e-9));
    CHECK (spec->thresholdDB == Catch::Approx (-30.0).margin (1e-9));
    CHECK (spec->ratio == Catch::Approx (8.0).margin (1e-9));
    CHECK (spec->attackSec == Catch::Approx (0.01).margin (1e-9));
    CHECK (spec->releaseSec == Catch::Approx (0.2).margin (1e-9));
}

//==============================================================================
// Test case (f): a usable entry without eq/dynamics content yields an
// identity spec (hasEq=false, hasCompression=false), not nullopt.

TEST_CASE ("ReplicaSpec: usable entry without content yields identity spec, not nullopt", "[replica][spec]")
{
    // Arrange
    const auto json = makeChainDoc (makeBareEntry ("bare", true));

    // Act
    const auto spec = ReplicaSpec::fromChainDoc (json);

    // Assert
    REQUIRE (spec.has_value());
    REQUIRE_FALSE (spec->hasEq);
    REQUIRE (spec->bands.empty());
    REQUIRE_FALSE (spec->hasCompression);
    CHECK (spec->thresholdDB == 0.0);
    CHECK (spec->ratio == 1.0);
    CHECK (spec->attackSec == 0.0);
    CHECK (spec->releaseSec == 0.0);
}
