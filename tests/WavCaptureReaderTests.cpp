// tests/WavCaptureReaderTests.cpp
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "../source/analysis/WavCaptureReader.h"
#include "../source/analysis/ChildWavAnalyzer.h"
#include "../source/analysis/FreqResponse.h"
#include "../source/signal/SineSweep.h"
#include "../source/signal/Impulse.h"
#include "../source/signal/MultiTone.h"

#include <cstring>
#include <cmath>
#include <vector>

namespace
{
//==============================================================================
// Hand-written WAV writer mirroring the 44-byte RIFF header + interleaved
// 24-bit PCM layout of CaptureBuffer::flush / WavExporter::exportTracks:
// 2*numChannels channels [dry ch0..N-1, wet ch0..N-1], little-endian.

void writeU16LE (uint8_t* dest, uint16_t value)
{
    dest[0] = static_cast<uint8_t> (value & 0xFF);
    dest[1] = static_cast<uint8_t> ((value >> 8) & 0xFF);
}

void writeU32LE (uint8_t* dest, uint32_t value)
{
    dest[0] = static_cast<uint8_t> (value & 0xFF);
    dest[1] = static_cast<uint8_t> ((value >> 8) & 0xFF);
    dest[2] = static_cast<uint8_t> ((value >> 16) & 0xFF);
    dest[3] = static_cast<uint8_t> ((value >> 24) & 0xFF);
}

/** Quantize a float sample exactly like the project writers do. */
int32_t quantize24 (float sample)
{
    return static_cast<int32_t> (juce::jlimit (-1.0f, 1.0f, sample) * 8388607.0f);
}

/** The exact float the reader must produce for a source sample: the same
 *  clamp + 1/8388607 scale on both sides, so the round trip is bit-exact. */
float expectedReadBack (float sample)
{
    return static_cast<float> (quantize24 (sample)) * (1.0f / 8388607.0f);
}

/** Write a 2*numChannels interleaved WAV ([dry ch0..N-1, wet ch0..N-1]) with
 *  the given bit depth (24 or 16 — 16 only for error-path fixtures). */
bool writeTestWav (const juce::File& path,
                   const juce::AudioBuffer<float>& dry,
                   const juce::AudioBuffer<float>& wet,
                   double sampleRate,
                   int bitsPerSample)
{
    const int numChannels = dry.getNumChannels();
    const int totalChannels = 2 * numChannels;
    const int numSamples = dry.getNumSamples();
    const int bytesPerSample = bitsPerSample / 8;
    const uint32_t dataSize = static_cast<uint32_t> (numSamples)
                            * static_cast<uint32_t> (totalChannels)
                            * static_cast<uint32_t> (bytesPerSample);
    const uint32_t bytesPerFrame = static_cast<uint32_t> (totalChannels)
                                 * static_cast<uint32_t> (bytesPerSample);

    path.deleteFile();
    juce::FileOutputStream stream (path, 0);
    if (stream.failedToOpen())
        return false;

    uint8_t header[44] = {};
    std::memcpy (header, "RIFF", 4);
    writeU32LE (header + 4, dataSize + 36);
    std::memcpy (header + 8, "WAVE", 4);
    std::memcpy (header + 12, "fmt ", 4);
    writeU32LE (header + 16, static_cast<uint32_t> (16));
    writeU16LE (header + 20, static_cast<uint16_t> (1));   // PCM
    writeU16LE (header + 22, static_cast<uint16_t> (totalChannels));
    writeU32LE (header + 24, static_cast<uint32_t> (sampleRate));
    writeU32LE (header + 28, static_cast<uint32_t> (sampleRate * static_cast<double> (bytesPerFrame)));
    writeU16LE (header + 32, static_cast<uint16_t> (bytesPerFrame));
    writeU16LE (header + 34, static_cast<uint16_t> (bitsPerSample));
    std::memcpy (header + 36, "data", 4);
    writeU32LE (header + 40, dataSize);

    if (! stream.write (header, sizeof (header)))
        return false;

    std::vector<uint8_t> pcm (static_cast<size_t> (dataSize));
    size_t offset = 0;
    for (int s = 0; s < numSamples; ++s)
    {
        for (int c = 0; c < totalChannels; ++c)
        {
            const float sample = (c < numChannels)
                ? dry.getSample (c, s)
                : wet.getSample (c - numChannels, s);
            if (bitsPerSample == 24)
            {
                const int32_t v = quantize24 (sample);
                pcm[offset++] = static_cast<uint8_t> (v & 0xFF);
                pcm[offset++] = static_cast<uint8_t> ((v >> 8) & 0xFF);
                pcm[offset++] = static_cast<uint8_t> ((v >> 16) & 0xFF);
            }
            else
            {
                const int16_t v = static_cast<int16_t> (
                    juce::jlimit (-1.0f, 1.0f, sample) * 32767.0f);
                pcm[offset++] = static_cast<uint8_t> (v & 0xFF);
                pcm[offset++] = static_cast<uint8_t> ((v >> 8) & 0xFF);
            }
        }
    }

    const bool ok = stream.write (pcm.data(), pcm.size());
    stream.flush();
    return ok;
}

//==============================================================================
// Sweep / MLS fixtures mirroring FreqResponseTests.cpp.

juce::AudioBuffer<float> generateSweep (double sr, double durationSec, double amplitude)
{
    SineSweep sw;
    sw.setFrequencyRange (20.0, 20000.0);
    sw.setDuration (durationSec);
    sw.setAmplitude (amplitude);
    sw.prepare (sr, 512);

    const int totalSamples = static_cast<int> (sr * durationSec);
    juce::AudioBuffer<float> buf (1, totalSamples);
    buf.clear();
    sw.generate (buf, 0, totalSamples);
    return buf;
}

juce::AudioBuffer<float> delayCopy (const juce::AudioBuffer<float>& src, int delaySamples)
{
    const int totalSamples = src.getNumSamples();
    juce::AudioBuffer<float> dst (1, totalSamples);
    dst.clear();
    const float* srcData = src.getReadPointer (0);
    float* dstData = dst.getWritePointer (0);

    for (int i = delaySamples; i < totalSamples; ++i)
        dstData[i] = srcData[i - delaySamples];

    return dst;
}

juce::AudioBuffer<float> generateMLS (double sr, int mlsLength, double amplitude)
{
    Impulse imp;
    imp.useMLS (true);
    imp.setMLSLength (mlsLength);
    imp.setAmplitude (amplitude);
    imp.prepare (sr, 512);

    juce::AudioBuffer<float> buf (1, mlsLength * 2);
    buf.clear();
    imp.generate (buf, 0, mlsLength);         // period 1 (plugin warm-up)
    imp.reset();
    imp.generate (buf, mlsLength, mlsLength); // period 2 (steady state)
    return buf;
}

juce::File tempWav (const juce::String& prefix)
{
    return juce::File::getSpecialLocation (juce::File::SpecialLocationType::tempDirectory)
               .getNonexistentChildFile (prefix, ".wav");
}

/** MultiTone fixture mirroring the host harmonic generator config
 *  (MeasurementSession.cpp Type::harmonicAnalysis branch): 8 octave
 *  fundamentals 100..12800 Hz, `durationSec`, `amplitude`. The child
 *  (PluginHostChild.cpp handleMeasure) and ChildWavAnalyzer::analyzeChildHarmonic
 *  hardcode the same list — keep the three in lockstep. */
juce::AudioBuffer<float> generateMultiTone (double sr, double durationSec, double amplitude)
{
    MultiTone mt;
    mt.setDuration (durationSec);
    mt.setAmplitude (amplitude);
    mt.setFrequencies ({ 100.0, 200.0, 400.0, 800.0, 1600.0, 3200.0, 6400.0, 12800.0 });
    mt.prepare (sr, 512);

    const int totalSamples = static_cast<int> (sr * durationSec);
    juce::AudioBuffer<float> buf (1, totalSamples);
    buf.clear();
    mt.generate (buf, 0, totalSamples);
    return buf;
}

/** Find a tone entry in a parsed harmonic_analysis "tones" array by
 *  fundamental frequency (±10 Hz — the export reports the FFT-bin
 *  frequency, not the nominal one). */
const juce::var* findTone (const juce::Array<juce::var>& tones, double fundamentalHz)
{
    for (const auto& t : tones)
    {
        if (std::abs (static_cast<double> (t["fundamental_hz"]) - fundamentalHz) < 10.0)
            return &t;
    }
    return nullptr;
}

/** Find a harmonic entry within a tone by its measured frequency (±10 Hz). */
const juce::var* findHarmonic (const juce::var& tone, double freqHz)
{
    const auto harmonics = tone["harmonics"].getArray();
    if (harmonics == nullptr)
        return nullptr;
    for (const auto& h : *harmonics)
    {
        if (std::abs (static_cast<double> (h["freq"]) - freqHz) < 10.0)
            return &h;
    }
    return nullptr;
}
} // namespace

//==============================================================================
TEST_CASE ("WavCaptureReader: splits 2N-channel 24-bit WAV into dry/wet exactly",
           "[wavcapturereader]")
{
    // Arrange — 2-channel plugin (4 interleaved channels), 1000 samples of
    // deterministic values covering negatives, near-full-scale and
    // byte-order-sensitive fractions.
    constexpr int kNumChannels = 2;
    constexpr int kNumSamples = 1000;
    constexpr double kSr = 48000.0;

    juce::AudioBuffer<float> dry (kNumChannels, kNumSamples);
    juce::AudioBuffer<float> wet (kNumChannels, kNumSamples);
    for (int i = 0; i < kNumSamples; ++i)
    {
        dry.setSample (0, i, static_cast<float> (
            0.7 * std::sin (2.0 * juce::MathConstants<double>::pi * 440.0 * i / kSr)));
        dry.setSample (1, i, (i % 2 == 0) ? -0.9f : 0.9f);
        wet.setSample (0, i, static_cast<float> (
            -0.3 * std::cos (2.0 * juce::MathConstants<double>::pi * 880.0 * i / kSr)));
        wet.setSample (1, i, static_cast<float> (i - 500) / 500.0f);  // ramp -1.0..1.0
    }

    const auto tmp = tempWav ("pluginlab_wcr_split_");
    REQUIRE (writeTestWav (tmp, dry, wet, kSr, 24));

    // Act
    juce::AudioBuffer<float> readDry, readWet;
    REQUIRE (WavCaptureReader::readDryWet (tmp, kNumChannels, readDry, readWet));

    // Assert — channel/sample layout and per-sample equality
    REQUIRE (readDry.getNumChannels() == kNumChannels);
    REQUIRE (readWet.getNumChannels() == kNumChannels);
    REQUIRE (readDry.getNumSamples() == kNumSamples);
    REQUIRE (readWet.getNumSamples() == kNumSamples);

    for (int c = 0; c < kNumChannels; ++c)
    {
        for (int i = 0; i < kNumSamples; ++i)
        {
            REQUIRE (readDry.getSample (c, i) == expectedReadBack (dry.getSample (c, i)));
            REQUIRE (readWet.getSample (c, i) == expectedReadBack (wet.getSample (c, i)));
            REQUIRE (readDry.getSample (c, i) == Catch::Approx (dry.getSample (c, i)).margin (1e-6));
            REQUIRE (readWet.getSample (c, i) == Catch::Approx (wet.getSample (c, i)).margin (1e-6));
        }
    }

    tmp.deleteFile();
}

//==============================================================================
TEST_CASE ("WavCaptureReader: 24-bit quantization keeps in-band analysis error negligible",
           "[wavcapturereader][parity]")
{
    // A WAV transit quantizes every sample to 24 bits. This test proves at
    // the analysis level (no export rounding involved) that the quantization
    // alone moves the response negligibly. Comparison is restricted to
    // 100 Hz – 10 kHz: past the last FFT window (~18 kHz) the sweep's H1
    // estimate is leakage-dominated and BOTH paths degrade there — out-of-band
    // differences are analysis artifacts, not transit errors. Measured
    // in-band maxima: mag 1.8e-6 dB, phase 1.2e-5 deg.
    constexpr double kSr = 48000.0;
    constexpr int kLatency = 97;

    const auto dry = generateSweep (kSr, 5.0, 0.5);
    const auto wet = delayCopy (dry, kLatency);

    // Reference: direct analysis on the original floats
    FreqResponse refFr;
    refFr.setLatencySamples (kLatency);
    const auto reference = refFr.analyze (dry, wet, kSr);
    REQUIRE (! reference.raw.empty());

    // The same signal after 24-bit quantization — exactly what the reader
    // reconstructs (compare against expectedReadBack)
    juce::AudioBuffer<float> dryQ (1, dry.getNumSamples());
    juce::AudioBuffer<float> wetQ (1, wet.getNumSamples());
    for (int i = 0; i < dry.getNumSamples(); ++i)
    {
        dryQ.setSample (0, i, expectedReadBack (dry.getSample (0, i)));
        wetQ.setSample (0, i, expectedReadBack (wet.getSample (0, i)));
    }

    FreqResponse qFr;
    qFr.setLatencySamples (kLatency);
    const auto quantized = qFr.analyze (dryQ, wetQ, kSr);

    REQUIRE (quantized.raw.size() == reference.raw.size());
    int inBandCount = 0;
    for (size_t i = 0; i < reference.raw.size(); ++i)
    {
        const double f = reference.raw[i].frequency;
        if (f < 100.0 || f > 10000.0)
            continue;
        ++inBandCount;
        REQUIRE (std::abs (quantized.raw[i].magnitudeDB - reference.raw[i].magnitudeDB) < 1e-5);
        REQUIRE (std::abs (quantized.raw[i].phaseDeg - reference.raw[i].phaseDeg) < 1e-4);
    }
    REQUIRE (inBandCount > 1000);
}

//==============================================================================
TEST_CASE ("WavCaptureReader: WAV-transit entry matches direct FreqResponse::analyze",
           "[wavcapturereader][parity]")
{
    // Arrange — 5 s sweep through a pure 97-sample latency (all-pass): the
    // expected response is 0 dB / 0° after latency compensation.
    constexpr double kSr = 48000.0;
    constexpr int kLatency = 97;
    constexpr int kBlockSize = 512;

    const auto dryMono = generateSweep (kSr, 5.0, 0.5);
    const auto wetMono = delayCopy (dryMono, kLatency);

    // 2-channel plugin (4 interleaved channels): ch1 carries a scaled copy so
    // the multi-channel layout is exercised end to end (analysis reads ch0).
    juce::AudioBuffer<float> dry (2, dryMono.getNumSamples());
    juce::AudioBuffer<float> wet (2, wetMono.getNumSamples());
    for (int i = 0; i < dryMono.getNumSamples(); ++i)
    {
        dry.setSample (0, i, dryMono.getSample (0, i));
        dry.setSample (1, i, dryMono.getSample (0, i) * 0.5f);
        wet.setSample (0, i, wetMono.getSample (0, i));
        wet.setSample (1, i, wetMono.getSample (0, i) * 0.5f);
    }

    const auto tmp = tempWav ("pluginlab_wcr_parity_");
    REQUIRE (writeTestWav (tmp, dry, wet, kSr, 24));

    // Direct path (reference)
    FreqResponse fr;
    fr.setLatencySamples (kLatency);
    const auto direct = fr.analyze (dryMono, wetMono, kSr);
    REQUIRE (! direct.raw.empty());

    // Act — transit through the WAV + host-side child-analysis entry
    const auto json = ChildWavAnalyzer::analyzeChildFrequencyResponse (
        tmp, 2, kSr, kBlockSize, "sweep", 0,
        "TestPlugin", "CLSID-D2B-TEST", kLatency);
    REQUIRE (json.isNotEmpty());

    // Assert — context built from the entry parameters (ADR-D-6)
    const auto doc = juce::JSON::parse (json);
    REQUIRE (doc.isObject());
    REQUIRE (doc["type"].toString() == "frequency_response");
    REQUIRE (doc["plugin"].toString() == "TestPlugin");
    REQUIRE (doc["class_id"].toString() == "CLSID-D2B-TEST");
    REQUIRE (doc["latency_samples"] == juce::var (kLatency));
    REQUIRE (doc["sample_rate"] == juce::var (kSr));
    REQUIRE (doc["measurement"]["block_size"] == juce::var (kBlockSize));
    REQUIRE (doc["source"]["type"].toString() == "signal");
    REQUIRE (json.contains ("\"parameter_snapshot\": {}"));
    REQUIRE (! json.contains ("excitation"));   // sweep is the default → omitted

    // Assert — point-by-point parity with the direct analysis, restricted to
    // 100 Hz – 10 kHz (the band where the sweep H1 estimate is meaningful;
    // see the analysis-level test above). The export writes mag/phase via
    // juce::String(v, 2), which TRUNCATES to 2 decimals (e.g. 0.535174 →
    // "0.53"), so a JSON value can be up to 0.01 away from the underlying
    // analysis value; the 24-bit transit adds < 1e-5 in-band. Margin 0.011 =
    // 0.01 truncation + 1e-3 quantization headroom; freq (1 decimal) → 0.101.
    const auto raw = doc["raw"];
    REQUIRE (raw.isArray());
    REQUIRE (raw.size() == static_cast<int> (direct.raw.size()));
    int inBandCount = 0;
    for (int i = 0; i < raw.size(); ++i)
    {
        const double f = static_cast<double> (raw[i]["f"]);
        if (f < 100.0 || f > 10000.0)
            continue;
        ++inBandCount;
        REQUIRE (std::abs (f - direct.raw[i].frequency) < 0.101);
        REQUIRE (std::abs (static_cast<double> (raw[i]["mag"]) - direct.raw[i].magnitudeDB) < 0.011);
        REQUIRE (std::abs (static_cast<double> (raw[i]["phase"]) - direct.raw[i].phaseDeg) < 0.011);
    }
    REQUIRE (inBandCount > 1000);

    tmp.deleteFile();
}

//==============================================================================
TEST_CASE ("WavCaptureReader: MLS WAV-transit entry matches direct analyzeMLS",
           "[wavcapturereader][parity][mls]")
{
    constexpr double kSr = 48000.0;
    constexpr int kMlsLength = 16383;
    constexpr int kLatency = 31;
    constexpr int kBlockSize = 512;

    const auto dryMono = generateMLS (kSr, kMlsLength, 0.5);
    const auto wetMono = delayCopy (dryMono, kLatency);

    // 1-channel plugin → 2 interleaved channels in the file
    const auto tmp = tempWav ("pluginlab_wcr_mls_");
    REQUIRE (writeTestWav (tmp, dryMono, wetMono, kSr, 24));

    // Direct path (reference)
    FreqResponse fr;
    fr.setLatencySamples (kLatency);
    const auto direct = fr.analyzeMLS (dryMono, wetMono, kSr, kMlsLength);
    REQUIRE (! direct.raw.empty());

    // Act — transit through the WAV + child-analysis entry (MLS branch)
    const auto json = ChildWavAnalyzer::analyzeChildFrequencyResponse (
        tmp, 1, kSr, kBlockSize, "mls", kMlsLength,
        "TestPlugin", "CLSID-D2B-TEST", kLatency);
    REQUIRE (json.isNotEmpty());

    const auto doc = juce::JSON::parse (json);
    REQUIRE (doc.isObject());
    REQUIRE (doc["measurement"]["excitation"].toString() == "mls");

    // Same truncation-based margins as the sweep parity test (Export writes
    // mag/phase truncated to 2 decimals → 0.01 + quantization headroom). The
    // full band is compared: every MLS harmonic carries full energy, so no
    // leakage-dominated bins exist.
    const auto raw = doc["raw"];
    REQUIRE (raw.isArray());
    REQUIRE (raw.size() == static_cast<int> (direct.raw.size()));
    for (int i = 0; i < raw.size(); ++i)
    {
        REQUIRE (std::abs (static_cast<double> (raw[i]["f"]) - direct.raw[i].frequency) < 0.101);
        REQUIRE (std::abs (static_cast<double> (raw[i]["mag"]) - direct.raw[i].magnitudeDB) < 0.0101);
        REQUIRE (std::abs (static_cast<double> (raw[i]["phase"]) - direct.raw[i].phaseDeg) < 0.0101);
    }

    tmp.deleteFile();
}

//==============================================================================
TEST_CASE ("WavCaptureReader: error paths return false without crashing",
           "[wavcapturereader]")
{
    const auto tmp = juce::File::getSpecialLocation (juce::File::SpecialLocationType::tempDirectory);

    // 1. Nonexistent file — and the output buffers are emptied on failure
    const auto missing = tmp.getNonexistentChildFile ("pluginlab_wcr_missing_", ".wav");
    juce::AudioBuffer<float> dry (2, 16), wet (2, 16);
    dry.clear();
    wet.clear();
    REQUIRE (! WavCaptureReader::readDryWet (missing, 2, dry, wet));
    REQUIRE (dry.getNumSamples() == 0);
    REQUIRE (wet.getNumSamples() == 0);

    // 2. Bad RIFF/WAVE magic — a file that is not a WAV at all
    const auto badMagic = tmp.getNonexistentChildFile ("pluginlab_wcr_badmagic_", ".wav");
    badMagic.replaceWithText (juce::String::repeatedString ("x", 64));
    REQUIRE (! WavCaptureReader::readDryWet (badMagic, 2, dry, wet));

    // 3. Channel mismatch: a valid 4-channel (2-plugin) WAV requested as 1
    //    or 3 channels (2*1=2 != 4, 2*3=6 != 4)
    juce::AudioBuffer<float> twoChDry (2, 32), twoChWet (2, 32);
    twoChDry.clear();
    twoChWet.clear();
    const auto fourCh = tmp.getNonexistentChildFile ("pluginlab_wcr_4ch_", ".wav");
    REQUIRE (writeTestWav (fourCh, twoChDry, twoChWet, 48000.0, 24));
    REQUIRE (! WavCaptureReader::readDryWet (fourCh, 1, dry, wet));
    REQUIRE (! WavCaptureReader::readDryWet (fourCh, 3, dry, wet));

    // 4. Bit depth 16 — valid WAV, wrong depth
    const auto sixteenBit = tmp.getNonexistentChildFile ("pluginlab_wcr_16bit_", ".wav");
    REQUIRE (writeTestWav (sixteenBit, twoChDry, twoChWet, 48000.0, 16));
    REQUIRE (! WavCaptureReader::readDryWet (sixteenBit, 2, dry, wet));

    // 5. The analysis entry reports failure as an empty JSON string
    REQUIRE (ChildWavAnalyzer::analyzeChildFrequencyResponse (
                 missing, 2, 48000.0, 512, "sweep", 0, "TestPlugin", "CLSID", 0).isEmpty());

    badMagic.deleteFile();
    fourCh.deleteFile();
    sixteenBit.deleteFile();
}

//==============================================================================
// Child harmonic analysis entry (T1): deterministic WAV → harmonic_analysis
// JSON. The dry excitation mirrors the host harmonic config (MultiTone, 8
// octave fundamentals 100..12800 Hz, 3 s, amplitude 0.4); wet applies a
// KNOWN injection (wet = dry + 0.1·dry², second-order distortion). The
// octave spacing means every fundamental's H2/H4 IS another fundamental
// (known tradeoff, analysis/AGENTS.md) — those percents read ~100%+ even
// with zero distortion. Note also that for octave fundamentals the H3 of
// every f except the top two cancels in dry² (pairs f+2f and 4f−f both
// exist and cancel), so the distortion assertions target H5 of 1600 Hz
// (8000 Hz) and H3 of 6400 Hz (19200 Hz) — both non-colliding, non-cancelling.
//==============================================================================

TEST_CASE ("ChildWavAnalyzer: harmonic identity WAV exports 7 clean tones",
           "[childharmonic][wavcapturereader]")
{
    // Arrange — clean multi-tone excitation, wet identical to dry (no
    // distortion): the only harmonics present are the octave collisions.
    constexpr double kSr = 48000.0;
    const auto dry = generateMultiTone (kSr, 3.0, 0.4);
    juce::AudioBuffer<float> wet (dry);

    const auto wav = tempWav ("pluginlab_childharmonic_id_");
    REQUIRE (writeTestWav (wav, dry, wet, kSr, 24));

    // Act — the child-WAV analysis entry (read WAV → analyze → export).
    const auto jsonText = ChildWavAnalyzer::analyzeChildHarmonic (
        wav, 1, kSr, 512, "TestPlugin", "CLSID", 0);
    REQUIRE_FALSE (jsonText.isEmpty());

    // Assert — structural: 7 tones (the 12800 Hz fundamental is dropped —
    // its H2 25600 Hz exceeds Nyquist, so its harmonics list is empty and
    // the tone is omitted); fundamentals land on the expected frequencies.
    const auto doc = juce::JSON::parse (jsonText);
    REQUIRE (doc.isObject());
    REQUIRE (doc["type"].toString() == "harmonic_analysis");

    const auto tones = doc["tones"].getArray();
    REQUIRE (tones != nullptr);
    REQUIRE (tones->size() == 7);

    const std::vector<double> expectedFundamentals =
        { 100.0, 200.0, 400.0, 800.0, 1600.0, 3200.0, 6400.0 };
    for (int i = 0; i < tones->size(); ++i)
    {
        const double f = static_cast<double> ((*tones)[i]["fundamental_hz"]);
        REQUIRE (std::abs (f - expectedFundamentals[static_cast<size_t> (i)]) < 10.0);
    }

    // Assert — no distortion: the non-colliding harmonics (H5 of 1600 Hz at
    // 8000 Hz, H3 of 6400 Hz at 19200 Hz) sit at the noise floor.
    auto* tone1600 = findTone (*tones, 1600.0);
    REQUIRE (tone1600 != nullptr);
    auto* h5 = findHarmonic (*tone1600, 8000.0);
    REQUIRE (h5 != nullptr);
    REQUIRE (static_cast<double> ((*h5)["percent"]) < 0.2);

    auto* tone6400 = findTone (*tones, 6400.0);
    REQUIRE (tone6400 != nullptr);
    auto* h36400 = findHarmonic (*tone6400, 19200.0);
    REQUIRE (h36400 != nullptr);
    REQUIRE (static_cast<double> ((*h36400)["percent"]) < 0.2);

    wav.deleteFile();
}

TEST_CASE ("ChildWavAnalyzer: harmonic distortion WAV shows the injected harmonics",
           "[childharmonic][wavcapturereader]")
{
    // Arrange — same excitation, wet = dry + 0.1·dry²: a known second-order
    // distortion. Its H5 of 1600 Hz lands at 8000 Hz and H3 of 6400 Hz at
    // 19200 Hz — neither collides with a fundamental (H3 of the lower
    // fundamentals cancels by pair symmetry, see the header comment).
    constexpr double kSr = 48000.0;
    const auto dry = generateMultiTone (kSr, 3.0, 0.4);

    juce::AudioBuffer<float> wet (1, dry.getNumSamples());
    for (int i = 0; i < dry.getNumSamples(); ++i)
    {
        const float d = dry.getSample (0, i);
        wet.setSample (0, i, d + 0.1f * d * d);
    }

    const auto wav = tempWav ("pluginlab_childharmonic_dist_");
    REQUIRE (writeTestWav (wav, dry, wet, kSr, 24));

    // Act
    const auto jsonText = ChildWavAnalyzer::analyzeChildHarmonic (
        wav, 1, kSr, 512, "TestPlugin", "CLSID", 0);
    REQUIRE_FALSE (jsonText.isEmpty());

    // Assert — the injected distortion is measured: percents are clearly
    // above the identity-case noise floor (1600 Hz H5 ~0.56%, 6400 Hz H3
    // ~0.45% for 0.1·dry² — thresholds stay far below to absorb FFT-bin /
    // window scalloping differences).
    const auto doc = juce::JSON::parse (jsonText);
    REQUIRE (doc.isObject());
    const auto tones = doc["tones"].getArray();
    REQUIRE (tones != nullptr);
    REQUIRE (tones->size() == 7);

    auto* tone1600 = findTone (*tones, 1600.0);
    REQUIRE (tone1600 != nullptr);
    auto* h5 = findHarmonic (*tone1600, 8000.0);
    REQUIRE (h5 != nullptr);
    REQUIRE (static_cast<double> ((*h5)["percent"]) > 0.3);

    auto* tone6400 = findTone (*tones, 6400.0);
    REQUIRE (tone6400 != nullptr);
    auto* h36400 = findHarmonic (*tone6400, 19200.0);
    REQUIRE (h36400 != nullptr);
    REQUIRE (static_cast<double> ((*h36400)["percent"]) > 0.25);

    wav.deleteFile();
}

TEST_CASE ("ChildWavAnalyzer: harmonic missing WAV returns empty JSON",
           "[childharmonic][wavcapturereader]")
{
    const auto missing = tempWav ("pluginlab_childharmonic_missing_");
    REQUIRE (ChildWavAnalyzer::analyzeChildHarmonic (
                 missing, 2, 48000.0, 512, "TestPlugin", "CLSID", 0).isEmpty());
    missing.deleteFile();
}
