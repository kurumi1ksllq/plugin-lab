#include "PluginLabReplica.h"

//==============================================================================
// Spec-driven replica VST3 (issue #27, T4). Design decisions are documented
// in PluginLabReplica.h; this file only wires them together:
//   - constructor: 21 hosted params, then resolveSpec() seeds the defaults
//     from the chain_doc (decision A/B/U3)
//   - prepareToPlay: initial chain config from the (spec-seeded) defaults
//   - processBlock: consume the dirty flag, rebuild the chain at block start,
//     then delegate to ReplicaChain (decision U6)

PluginLabReplica::PluginLabReplica()
    : AudioPluginInstance (AudioProcessor::BusesProperties()
                               .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                               .withOutput ("Output", juce::AudioChannelSet::stereo(), true))
{
    // EQ band slots (decision B/U3): 4 fixed slots x (Used / Frequency / Gain
    // / Q). Identity defaults (Used=0) — resolveSpec() re-seeds the slots the
    // spec fills.
    for (int i = 0; i < kNumBandSlots; ++i)
    {
        const juce::String slot = juce::String (i + 1);

        auto addSlotParam = [this] (const juce::String& id, const juce::String& name,
                                    const juce::String& label, float minReal, float maxReal,
                                    float defaultReal) -> ReplicaParameter*
        {
            auto param = std::make_unique<ReplicaParameter> (
                id, name, label, minReal, maxReal, defaultReal, configDirty);
            auto* raw = param.get();
            addHostedParameter (std::move (param));
            return raw;
        };

        bandUsed[static_cast<size_t> (i)] = addSlotParam (
            "band" + slot + "_used", "Band " + slot + " Used", {}, 0.0f, 1.0f, 0.0f);
        bandFreq[static_cast<size_t> (i)] = addSlotParam (
            "band" + slot + "_freq", "Band " + slot + " Frequency", "Hz",
            20.0f, 20000.0f, 1000.0f);
        bandGain[static_cast<size_t> (i)] = addSlotParam (
            "band" + slot + "_gain", "Band " + slot + " Gain", "dB", -24.0f, 24.0f, 0.0f);
        bandQ[static_cast<size_t> (i)] = addSlotParam (
            "band" + slot + "_q", "Band " + slot + " Q", {}, 0.1f, 20.0f, 1.0f);
    }

    // Compressor + make-up params (decision B/U3). Attack/Release are exposed
    // in ms; the chain consumes seconds (converted in applyParamConfig). The
    // touched flag arms the compressor when a value moves off the default
    // (decision U6).
    auto addTouchedParam = [this] (const juce::String& id, const juce::String& name,
                                   const juce::String& label, float minReal, float maxReal,
                                   float defaultReal) -> ReplicaParameter*
    {
        auto param = std::make_unique<ReplicaParameter> (
            id, name, label, minReal, maxReal, defaultReal, configDirty, &compressionTouched);
        auto* raw = param.get();
        addHostedParameter (std::move (param));
        return raw;
    };

    thresholdParam = addTouchedParam (
        "threshold", "Threshold", "dB", -60.0f, 0.0f, kDefaultThresholdDB);
    ratioParam = addTouchedParam ("ratio", "Ratio", {}, 1.0f, 20.0f, kDefaultRatio);
    attackParam = addTouchedParam ("attack", "Attack", "ms", 1.0f, 1000.0f, kDefaultAttackMs);
    releaseParam = addTouchedParam ("release", "Release", "ms", 1.0f, 2000.0f, kDefaultReleaseMs);

    auto makeupParam = std::make_unique<ReplicaParameter> (
        "makeup_gain", "Makeup Gain", "dB", -24.0f, 24.0f, 0.0f, configDirty);
    makeupGainParam = makeupParam.get();
    addHostedParameter (std::move (makeupParam));

    resolveSpec();
}

//==============================================================================

void PluginLabReplica::resolveSpec()
{
    // Decision A: env var (primary) -> %APPDATA%/PluginLab/replica_spec.json.
    // SystemStats reads the wide environment on Windows (GetEnvironmentVariableW),
    // so non-ASCII paths round-trip correctly.
    const juce::String envPath = juce::SystemStats::getEnvironmentVariable (
        "PLUGINLAB_REPLICA_SPEC", {});
    if (envPath.isNotEmpty())
    {
        specFilePath = envPath;
    }
    else
    {
        specFilePath = juce::File::getSpecialLocation (
                           juce::File::SpecialLocationType::userApplicationDataDirectory)
                           .getChildFile ("PluginLab")
                           .getChildFile ("replica_spec.json")
                           .getFullPathName();
    }

    const juce::File specFile (specFilePath);
    if (! specFile.existsAsFile())
    {
        CRASH_LOG_WARN ("Replica spec", "file missing: " + specFilePath);
        applySpec (ReplicaSpec{}); // identity fallback (decision U2)
        return;
    }

    const auto spec = ReplicaSpec::fromChainDoc (specFile.loadFileAsString());
    if (! spec.has_value())
    {
        CRASH_LOG_WARN ("Replica spec",
                        "no usable entry (unparseable or no usable_as_spec): " + specFilePath);
        applySpec (ReplicaSpec{}); // identity fallback (decision U2)
        return;
    }

    applySpec (*spec);
}

void PluginLabReplica::applySpec (const ReplicaSpec& spec)
{
    hasCompressionFromSpec = spec.hasCompression;

    // Band slots: the first min(4, spec.bands.size()) spec bands fill slots
    // 1..N; extra bands are dropped (fixed surface). Empty slots stay
    // bypassed (Used=0).
    const auto numFilled = std::min (static_cast<size_t> (spec.bands.size()),
                                     static_cast<size_t> (kNumBandSlots));
    for (size_t i = 0; i < static_cast<size_t> (kNumBandSlots); ++i)
    {
        if (i < numFilled)
        {
            bandUsed[i]->setDefaultReal (1.0f);
            bandFreq[i]->setDefaultReal (static_cast<float> (spec.bands[i].freqHz));
            bandGain[i]->setDefaultReal (static_cast<float> (spec.bands[i].gainDB));
            bandQ[i]->setDefaultReal (static_cast<float> (spec.bands[i].q));
        }
        else
        {
            bandUsed[i]->setDefaultReal (0.0f);
            bandFreq[i]->setDefaultReal (1000.0f);
            bandGain[i]->setDefaultReal (0.0f);
            bandQ[i]->setDefaultReal (1.0f);
        }
    }

    // Compressor defaults: spec values when the spec has compression (and the
    // touched flag is armed so the compressor starts enabled), else the
    // identity defaults (rule U6: disabled until a param moves off default).
    if (spec.hasCompression)
    {
        thresholdParam->setDefaultReal (static_cast<float> (spec.thresholdDB));
        ratioParam->setDefaultReal (static_cast<float> (spec.ratio));
        attackParam->setDefaultReal (static_cast<float> (spec.attackSec * 1000.0));
        releaseParam->setDefaultReal (static_cast<float> (spec.releaseSec * 1000.0));
        compressionTouched.store (true);
    }
    else
    {
        thresholdParam->setDefaultReal (kDefaultThresholdDB);
        ratioParam->setDefaultReal (kDefaultRatio);
        attackParam->setDefaultReal (kDefaultAttackMs);
        releaseParam->setDefaultReal (kDefaultReleaseMs);
    }
    makeupGainParam->setDefaultReal (0.0f);
}

void PluginLabReplica::applyParamConfig()
{
    ReplicaSpec spec;

    // Active band slot: Used > 0.5 AND Frequency > 1.0 (guard against an
    // automation-armed slot whose 0 Hz default would make a degenerate
    // biquad; see decision U6 in the header).
    for (size_t i = 0; i < static_cast<size_t> (kNumBandSlots); ++i)
    {
        if (bandUsed[i]->getRealValue() > 0.5f && bandFreq[i]->getRealValue() > 1.0f)
        {
            ReplicaEQBand band;
            band.freqHz = bandFreq[i]->getRealValue();
            band.gainDB = bandGain[i]->getRealValue();
            band.q = bandQ[i]->getRealValue();
            spec.bands.push_back (band);
        }
    }
    spec.hasEq = ! spec.bands.empty();

    // Compressor rule U6: enabled iff the spec had compression OR any of the
    // four compressor params was explicitly moved off its default. When
    // enabled, the CURRENT param values are used (ms -> s for the taus).
    const bool compressorOn = hasCompressionFromSpec || compressionTouched.load();
    if (compressorOn)
    {
        spec.hasCompression = true;
        spec.thresholdDB = thresholdParam->getRealValue();
        spec.ratio = ratioParam->getRealValue();
        spec.attackSec = attackParam->getRealValue() / 1000.0;
        spec.releaseSec = releaseParam->getRealValue() / 1000.0;
    }

    chain_.configure (spec);
    chain_.setMakeupGainDB (makeupGainParam->getRealValue());
}

void PluginLabReplica::rebuildChain()
{
    chain_.prepare (sampleRate);
    applyParamConfig();
    chain_.reset();
}

//==============================================================================
// AudioProcessor
//==============================================================================

void PluginLabReplica::prepareToPlay (double sampleRateToUse,
                                      int maximumExpectedSamplesPerBlock)
{
    juce::ignoreUnused (maximumExpectedSamplesPerBlock);

    sampleRate = sampleRateToUse;
    rebuildChain();          // initial config from the (spec-seeded) defaults
    configDirty.store (false);
}

void PluginLabReplica::reset()
{
    chain_.reset();
}

void PluginLabReplica::processBlock (juce::AudioBuffer<float>& buffer,
                                     juce::MidiBuffer& midiMessages)
{
    // Decision U6: params write atomics on any thread; the audio thread reads
    // them exactly once per block, at the block start. No locks.
    if (configDirty.exchange (false))
        rebuildChain();

    chain_.processBlock (buffer, midiMessages);
}

void PluginLabReplica::processBlock (juce::AudioBuffer<double>& buffer,
                                     juce::MidiBuffer& midiMessages)
{
    juce::AudioBuffer<float> floatBuffer (buffer.getNumChannels(), buffer.getNumSamples());
    for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
    {
        const auto* src = buffer.getReadPointer (ch);
        auto* dst = floatBuffer.getWritePointer (ch);
        for (int i = 0; i < buffer.getNumSamples(); ++i)
            dst[i] = static_cast<float> (src[i]);
    }

    processBlock (floatBuffer, midiMessages);

    for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
    {
        const auto* src = floatBuffer.getReadPointer (ch);
        auto* dst = buffer.getWritePointer (ch);
        for (int i = 0; i < buffer.getNumSamples(); ++i)
            dst[i] = static_cast<double> (src[i]);
    }
}

//==============================================================================
// AudioPluginInstance
//==============================================================================

void PluginLabReplica::fillInPluginDescription (juce::PluginDescription& description) const
{
    description.name = getName();
    description.pluginFormatName = "PluginLabReplica";
    description.version = "1.0.0";
    description.fileOrIdentifier = "PluginLabReplica";
    description.uniqueId = 0x5245504C; // 'REPL'
    description.numInputChannels = 2;
    description.numOutputChannels = 2;
}

//==============================================================================
// This creates new instances of the plugin. Required by the JUCE VST3 wrapper
// (juce_audio_plugin_client_VST3.cpp -> juce_CreatePluginFilter.h:44 calls
// the global ::createPluginFilter()).
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new PluginLabReplica();
}
