/**
 * PluginLabReplicaTests (issue #27, T4): end-to-end verification of the
 * spec-driven replica VST3 through the REAL plugin boundary.
 *
 * The test writes a temp chain_doc (one usable_as_spec entry carrying a single
 * plausible EQ section: 1000 Hz / +6 dB / Q 1.0, no dynamics), points
 * PLUGINLAB_REPLICA_SPEC at it, loads the built PluginLabReplica.vst3
 * in-process (VST3PluginFormat, same route as
 * ChildHostParityTests::loadPluginInProcess), and drives a 1000 Hz sine
 * through it. The RBJ peak filter applies exactly dBToGain (6) at its centre
 * frequency (ReplicaChain.h signal model), so
 * outputRMS == inputRMS * dBToGain (6) proves spec -> hosted params -> DSP
 * chain across the VST3 boundary. The parameter surface is asserted against
 * the describe_chain.py classifier name patterns (Band N Used/Frequency/Gain/Q
 * + Threshold/Ratio/Attack/Release/Makeup Gain) so the T5 scan loop classifies
 * the replica correctly.
 *
 * If the artifact is absent (should not happen — add_dependencies guarantees
 * the build order), the test SKIPs with the reason logged, mirroring the
 * ChildHostParityTests real-plugin exception.
 */
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <juce_audio_processors/juce_audio_processors.h>

#include <cmath>
#include <cstdlib>
#include <memory>
#include <vector>

#ifndef REPLICA_PLUGIN
#error "REPLICA_PLUGIN must be defined by tests/CMakeLists.txt"
#endif

namespace
{
    //==============================================================================
    juce::File replicaPluginFile()
    {
        return juce::File (juce::String (REPLICA_PLUGIN));
    }

    /** Make sure a JUCE MessageManager exists and THIS thread is the message
     *  thread: createPluginInstanceAsync posts creation to the message thread,
     *  which we pump with runDispatchLoopUntil while waiting (mirror of
     *  ChildHostParityTests.cpp / PluginHostChild.cpp handleLoad). */
    void ensureMessageManager()
    {
        if (juce::MessageManager::getInstanceWithoutCreating() == nullptr)
            juce::MessageManager::getInstance();
    }

    /** Writes a minimal chain_doc with ONE usable_as_spec entry carrying a
     *  single plausible EQ section (1000 Hz / +6 dB / Q 1.0) and no dynamics —
     *  the same shape ReplicaSpecTests.cpp uses. */
    juce::File writeSpecFile()
    {
        const auto specFile = juce::File::getSpecialLocation (
                                  juce::File::SpecialLocationType::tempDirectory)
                                  .getNonexistentChildFile ("pluginlab_replica_spec_", ".json");
        const juce::String jsonText = juce::String (
            "{ \"generated_at\": \"2026-08-14T00:00:00Z\","
            " \"source\": { \"aggregate_report\": \"report.md\","
            " \"dataset_dir\": null, \"report_generated_at\": \"2026-08-14T00:00:00Z\" },"
            " \"plugins\": [ { \"slug\": \"eq-only\", \"plugin\": \"Unit Test\","
            " \"plugin_type\": { \"kind\": \"eq-dynamics\", \"confidence\": \"high\","
            " \"basis\": [] },"
            " \"eq\": { \"present\": true, \"overall\": \"clean\","
            " \"sections\": [ { \"freq_hz\": 1000, \"gain_db\": 6, \"q\": 1.0,"
            " \"plausible\": true } ], \"notes\": [] },"
            " \"dynamics\": { \"present\": false, \"compression\":"
            " { \"threshold_derived\": null, \"ratio_derived\": null,"
            " \"conflict\": false },"
            " \"gr\": { \"attack_ms\": null, \"release_ms\": null,"
            " \"attack_plausible\": false, \"release_plausible\": false },"
            " \"notes\": [] },"
            " \"nonlinearity\": { \"verdict\": \"clean\" },"
            " \"processing_order\": { \"order\": \"eq-first\","
            " \"confidence\": \"high\", \"basis\": [] },"
            " \"usable_as_spec\": true, \"why_not_spec\": [] } ] }");
        specFile.replaceWithText (jsonText);
        return specFile;
    }

    /** Loads the replica VST3 in-process via VST3PluginFormat (same route as
     *  ChildHostParityTests.cpp loadPluginInProcess). */
    std::unique_ptr<juce::AudioPluginInstance> loadReplicaPlugin (const juce::File& path,
                                                                  double sampleRate,
                                                                  int blockSize)
    {
        juce::AudioPluginFormatManager formatManager;
        formatManager.addFormat (std::make_unique<juce::VST3PluginFormat>());
        auto* format = formatManager.getFormat (0);
        if (format == nullptr)
            return {};

        juce::OwnedArray<juce::PluginDescription> found;
        format->findAllTypesForFile (found, path.getFullPathName());
        if (found.isEmpty())
            return {};

        struct LoadState
        {
            std::unique_ptr<juce::AudioPluginInstance> instance;
            juce::String error;
            juce::WaitableEvent done;
            std::atomic<bool> alive { true };
        };
        auto state = std::make_shared<LoadState>();
        auto onCreated = [state] (std::unique_ptr<juce::AudioPluginInstance> instance,
                                  const juce::String& errorMessage) mutable
        {
            if (! state->alive.load())
                return;
            state->instance = std::move (instance);
            state->error = errorMessage;
            state->done.signal();
        };

        try
        {
            format->createPluginInstanceAsync (*found.getFirst(), sampleRate, blockSize,
                                               std::move (onCreated));
            const auto deadline = juce::Time::getMillisecondCounter() + 30000u;
            while (! state->done.wait (20))
            {
                if (juce::Time::getMillisecondCounter() >= deadline)
                {
                    state->alive = false;
                    return {};
                }
                juce::MessageManager::getInstance()->runDispatchLoopUntil (20);
            }
        }
        catch (...)
        {
            return {};
        }

        if (! state->instance)
            return {};
        return std::move (state->instance);
    }

    juce::AudioProcessorParameter* findParam (juce::AudioPluginInstance& instance,
                                              const juce::String& name)
    {
        for (auto* param : instance.getParameters())
        {
            if (param->getName (64) == name)
                return param;
        }
        return nullptr;
    }
} // namespace

//==============================================================================
// T4 end-to-end: spec -> hosted params -> DSP chain through the real VST3.

TEST_CASE ("PluginLabReplica: spec EQ applies +6 dB at 1 kHz through the real VST3 boundary",
           "[replica][vst3][integration]")
{
    // Arrange: a temp chain_doc with one usable entry (1000 Hz / +6 dB / Q 1.0)
    // pointed at by PLUGINLAB_REPLICA_SPEC (decision A primary route).
    const auto specFile = writeSpecFile();
    REQUIRE (specFile.existsAsFile());
    REQUIRE (_putenv_s ("PLUGINLAB_REPLICA_SPEC", specFile.getFullPathName().toRawUTF8()) == 0);

    const auto pluginFile = replicaPluginFile();
    if (! pluginFile.exists())
        SKIP ("REPLICA_PLUGIN not present: " + pluginFile.getFullPathName());

    ensureMessageManager();

    constexpr double kSr = 48000.0;
    constexpr int kBlockSize = 512;
    constexpr int kNumBlocks = 16;
    constexpr double kFreqHz = 1000.0;
    constexpr double kAmp = 0.5;

    // Act: load the plugin and drive a 1000 Hz sine through it.
    auto instance = loadReplicaPlugin (pluginFile, kSr, kBlockSize);
    REQUIRE (instance != nullptr);

    // The VST3 format ignores the sampleRate/blockSize hints of
    // createPluginInstanceAsync and returns an UNPREPARED instance (the VST3
    // component is inactive until prepareToPlay — the plugin's processBlock is
    // a no-op otherwise). A real host (SweepRunner) prepares explicitly; do
    // the same here so the chain config is built from the spec-seeded defaults.
    instance->prepareToPlay (kSr, kBlockSize);

    juce::MidiBuffer midi;
    juce::AudioBuffer<float> block (2, kBlockSize);
    double outSumSquares = 0.0;
    const int processedSamples = kBlockSize * (kNumBlocks - 1); // first block is warmup
    double sampleIndex = 0.0;
    for (int b = 0; b < kNumBlocks; ++b)
    {
        for (int ch = 0; ch < 2; ++ch)
        {
            auto* data = block.getWritePointer (ch);
            for (int i = 0; i < kBlockSize; ++i)
            {
                const double phase = 2.0 * juce::MathConstants<double>::pi
                                   * kFreqHz * (sampleIndex + static_cast<double> (i)) / kSr;
                data[i] = static_cast<float> (kAmp * std::sin (phase));
            }
        }
        sampleIndex += static_cast<double> (kBlockSize);

        instance->processBlock (block, midi);

        // Skip the first block: the biquad transient settles within a few
        // cycles (Q=1 at 1 kHz: well under one 512-sample block).
        if (b > 0)
        {
            for (int ch = 0; ch < 2; ++ch)
            {
                const auto* data = block.getReadPointer (ch);
                for (int i = 0; i < kBlockSize; ++i)
                    outSumSquares += static_cast<double> (data[i]) * static_cast<double> (data[i]);
            }
        }
    }

    // Assert: the peak filter applies exactly dBToGain (6) at its centre
    // frequency (RBJ design property, ReplicaChain.h), so
    // outputRMS == inputRMS * dBToGain (6) within a small margin.
    const double outRms = std::sqrt (outSumSquares / static_cast<double> (2 * processedSamples));
    const double inRms = kAmp / std::sqrt (2.0);
    const double expectedGain = std::pow (10.0, 6.0 / 20.0);
    CHECK (outRms == Catch::Approx (inRms * expectedGain).margin (0.02));

    // Assert: classifier-pattern parameter surface (describe_chain.py
    // _BAND_KEY_RE + _DYNAMICS_SUBSTRINGS) so T5 classifies the replica.
    // The exact count is JUCE-internal: our 21 params plus the VST3 wrapper's
    // own bypass parameter, so only the lower bound is asserted.
    CHECK (instance->getParameters().size() >= 21);
    const juce::String requiredNames[] = {
        "Band 1 Used", "Band 1 Frequency", "Band 1 Gain", "Band 1 Q",
        "Band 2 Used", "Band 2 Frequency", "Band 2 Gain", "Band 2 Q",
        "Band 3 Used", "Band 3 Frequency", "Band 3 Gain", "Band 3 Q",
        "Band 4 Used", "Band 4 Frequency", "Band 4 Gain", "Band 4 Q",
        "Threshold", "Ratio", "Attack", "Release", "Makeup Gain"
    };
    for (const auto& name : requiredNames)
    {
        INFO ("missing parameter: " + name);
        CHECK (findParam (*instance, name) != nullptr);
    }

    // Assert: spec-derived defaults — band 1 active at 1000 Hz (normalized
    // (1000-20)/(20000-20)), band 2 unused, no compression in the spec so the
    // compressor params keep the identity defaults (threshold -20 dB).
    const auto* used1 = findParam (*instance, "Band 1 Used");
    const auto* freq1 = findParam (*instance, "Band 1 Frequency");
    const auto* used2 = findParam (*instance, "Band 2 Used");
    const auto* threshold = findParam (*instance, "Threshold");
    REQUIRE (used1 != nullptr);
    REQUIRE (freq1 != nullptr);
    REQUIRE (used2 != nullptr);
    REQUIRE (threshold != nullptr);
    CHECK (used1->getDefaultValue() == Catch::Approx (1.0).margin (1e-6));
    CHECK (freq1->getDefaultValue()
           == Catch::Approx ((1000.0 - 20.0) / (20000.0 - 20.0)).margin (1e-4));
    CHECK (used2->getDefaultValue() == Catch::Approx (0.0).margin (1e-6));
    CHECK (threshold->getDefaultValue()
           == Catch::Approx ((-20.0 - -60.0) / (0.0 - -60.0)).margin (1e-4));

    // Cleanup: our own temp spec file and env var (nothing else reads it).
    specFile.deleteFile();
    _putenv_s ("PLUGINLAB_REPLICA_SPEC", "");
}
