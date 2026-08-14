#include "ReplicaSpec.h"

namespace
{
//==============================================================================
/** True for any JSON number (int / int64 / double); false for bool, string,
 *  null and missing values — the defensive gate for num|None contract slots.
 */
bool isNumber (const juce::var& v)
{
    return v.isDouble() || v.isInt() || v.isInt64();
}

/** True when the var is an explicit JSON null (or absent) value. */
bool isNullOrMissing (const juce::var& v)
{
    return v.isVoid() || v.isUndefined();
}
} // namespace

//==============================================================================
std::optional<ReplicaSpec> ReplicaSpec::fromChainDoc (const juce::String& jsonText)
{
    // Parse failure or a non-object document -> no spec. Never throws:
    // juce::JSON::parse reports failure by returning var::undefined().
    const juce::var parsed = juce::JSON::parse (jsonText);
    if (! parsed.isObject())
        return std::nullopt;

    const juce::var plugins = parsed["plugins"];
    if (! plugins.isArray())
        return std::nullopt;

    // Decision U1: the FIRST plugins[] entry with usable_as_spec == true
    // becomes the replica blueprint; entries with usable_as_spec false are
    // skipped. No usable entry -> no spec.
    juce::var selected;
    for (const auto& entry : *plugins.getArray())
    {
        if (! entry.isObject())
            continue;
        const juce::var usable = entry["usable_as_spec"];
        if (usable.isBool() && static_cast<bool> (usable))
        {
            selected = entry;
            break;
        }
    }
    if (! selected.isObject())
        return std::nullopt;

    ReplicaSpec spec;

    // -- EQ: keep plausible sections whose freq_hz/gain_db/q are all numbers.
    //    Filtering is per-section; a bad section never rejects the entry.
    const juce::var eq = selected["eq"];
    if (eq.isObject())
    {
        const juce::var sections = eq["sections"];
        if (sections.isArray())
        {
            for (const auto& sectionVar : *sections.getArray())
            {
                if (! sectionVar.isObject())
                    continue;
                const juce::var plausible = sectionVar["plausible"];
                if (! plausible.isBool() || ! static_cast<bool> (plausible))
                    continue;
                const juce::var freqVar = sectionVar["freq_hz"];
                const juce::var gainVar = sectionVar["gain_db"];
                const juce::var qVar = sectionVar["q"];
                if (isNullOrMissing (freqVar) || isNullOrMissing (gainVar)
                    || isNullOrMissing (qVar)
                    || ! isNumber (freqVar) || ! isNumber (gainVar)
                    || ! isNumber (qVar))
                    continue;

                ReplicaEQBand band;
                band.freqHz = static_cast<double> (freqVar);
                band.gainDB = static_cast<double> (gainVar);
                band.q = static_cast<double> (qVar);
                spec.bands.push_back (band);
            }
        }
    }
    spec.hasEq = ! spec.bands.empty();

    // -- Dynamics: threshold/ratio come from the *_derived slots (both must
    //    be present numbers). A conflict flag alone never rejects the entry.
    const juce::var dynamics = selected["dynamics"];
    if (dynamics.isObject())
    {
        const juce::var compression = dynamics["compression"];
        if (compression.isObject())
        {
            const juce::var thresholdVar = compression["threshold_derived"];
            const juce::var ratioVar = compression["ratio_derived"];
            if (isNumber (thresholdVar) && isNumber (ratioVar))
            {
                spec.hasCompression = true;
                spec.thresholdDB = static_cast<double> (thresholdVar);
                spec.ratio = static_cast<double> (ratioVar);
            }
        }

        // GR attack/release: derived tau values in ms, converted to seconds.
        const juce::var gr = dynamics["gr"];
        if (gr.isObject())
        {
            const juce::var attackMsVar = gr["attack_ms"];
            const juce::var releaseMsVar = gr["release_ms"];
            if (isNumber (attackMsVar) && isNumber (releaseMsVar))
            {
                spec.attackSec = static_cast<double> (attackMsVar) / 1000.0;
                spec.releaseSec = static_cast<double> (releaseMsVar) / 1000.0;
            }
        }
    }

    return spec;
}
