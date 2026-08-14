#pragma once

#include <JuceHeader.h>

#include <optional>
#include <vector>

//==============================================================================
/** One plausible EQ band from a chain_doc eq.sections entry. */
struct ReplicaEQBand
{
    double freqHz = 0.0;
    double gainDB = 0.0;
    double q = 1.0;
};

//==============================================================================
/**
 * Spec-driven replica plugin configuration, parsed from a chain_doc
 * (describe_chain.py, CONTRACT_VERSION="2").
 *
 * fromChainDoc picks the FIRST plugins[] entry with usable_as_spec == true
 * (decision U1) and maps:
 *   - eq.sections  -> bands (only plausible sections with numeric
 *                     freq_hz/gain_db/q are kept, individually)
 *   - dynamics.compression.threshold_derived / ratio_derived -> thresholdDB /
 *                     ratio (both must be present numbers)
 *   - dynamics.gr.attack_ms / release_ms -> attackSec / releaseSec, converted
 *                     ms -> s (both must be present numbers)
 *
 * Returns nullopt only when the JSON is unparseable, has no plugins array,
 * or no entry is usable_as_spec. A usable entry without eq/dynamics content
 * maps to an identity spec (hasEq=false, hasCompression=false). Never throws.
 */
struct ReplicaSpec
{
    bool hasEq = false;
    std::vector<ReplicaEQBand> bands;

    bool hasCompression = false;
    double thresholdDB = 0.0;
    double ratio = 1.0;

    double attackSec = 0.0;
    double releaseSec = 0.0;

    static std::optional<ReplicaSpec> fromChainDoc (const juce::String& jsonText);
};
