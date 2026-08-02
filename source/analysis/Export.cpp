#include "Export.h"

namespace Export
{
static juce::String fmtDouble (double v, int precision = 2)
{
    return juce::String (v, precision);
}

/** Escape a string so it is safe to embed inside a JSON string literal. */
static juce::String escapeJsonString (const juce::String& s)
{
    juce::String out;
    out.preallocateBytes (s.getNumBytesAsUTF8() + 16);

    for (auto c : s)
    {
        switch (c)
        {
            case '\\': out += "\\\\"; break;
            case '"':  out += "\\\""; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (c < 32)
                    out += juce::String::formatted ("\\u%04x", (int) c);
                else
                    out += c;
        }
    }

    return out;
}

/** Append the "source": {...} metadata block (with a trailing comma so more
 *  members can follow). Only non-default fields are emitted. */
static void appendSourceBlock (juce::String& json, const Export::Context& ctx,
                               const juce::String& indent)
{
    json += indent + "\"source\": {\n";
    json += indent + "  \"type\": \"" + escapeJsonString (ctx.source.type) + "\"";

    if (ctx.source.filePath.isNotEmpty())
        json += ",\n" + indent + "  \"file_path\": \"" + escapeJsonString (ctx.source.filePath) + "\"";
    if (ctx.source.sourceSampleRate > 0.0)
        json += ",\n" + indent + "  \"sample_rate\": " + juce::String (ctx.source.sourceSampleRate);
    if (ctx.source.resampleRatio > 0.0)
        json += ",\n" + indent + "  \"resample_ratio\": " + juce::String (ctx.source.resampleRatio);
    if (ctx.source.durationSec > 0.0)
        json += ",\n" + indent + "  \"duration_sec\": " + juce::String (ctx.source.durationSec);
    if (ctx.source.noiseType.isNotEmpty())
        json += ",\n" + indent + "  \"noise_type\": \"" + escapeJsonString (ctx.source.noiseType) + "\"";
    if (ctx.source.seed != 0)
        json += ",\n" + indent + "  \"seed\": " + juce::String ((int64_t) ctx.source.seed);

    json += "\n" + indent + "},\n";
}

/** Append the measurement-context fields shared by every exporter (plugin,
 *  class_id, latency, sample_rate, measurement, parameter_snapshot, source).
 *  Each member is emitted with a trailing comma so more members can follow. */
static void appendContextFields (juce::String& json, const Export::Context& ctx,
                                 const juce::String& indent)
{
    json += indent + "\"plugin\": \"" + escapeJsonString (ctx.pluginName) + "\",\n";
    json += indent + "\"class_id\": \"" + escapeJsonString (ctx.classId) + "\",\n";
    json += indent + "\"latency_samples\": " + juce::String (ctx.latencySamples) + ",\n";
    json += indent + "\"sample_rate\": " + juce::String (ctx.sampleRate) + ",\n";
    json += indent + "\"measurement\": {\n";
    json += indent + "  \"sample_rate\": " + juce::String (ctx.sampleRate) + ",\n";
    json += indent + "  \"block_size\": " + juce::String (ctx.blockSize) + "\n";
    json += indent + "},\n";
    json += indent + "\"parameter_snapshot\": "
            + (ctx.paramSnapshot.isNotEmpty() ? ctx.paramSnapshot : juce::String ("{}")) + ",\n";
    appendSourceBlock (json, ctx, indent);
}

/** Serialize the frequency-response analysis body (raw/smoothed point
 *  arrays). Ends with a trailing ",\n" so the caller can drop it or
 *  continue with more members. */
static void appendFreqArrays (juce::String& json, const FreqResponse::Result& result,
                              const juce::String& indent)
{
    auto writePoints = [&](const juce::String& name,
                            const std::vector<FreqResponse::Point>& points)
    {
        json += indent + "\"" + name + "\": [\n";
        for (size_t i = 0; i < points.size(); ++i)
        {
            json += indent + "  {\"f\": " + fmtDouble (points[i].frequency, 1)
                    + ", \"mag\": " + fmtDouble (points[i].magnitudeDB)
                    + ", \"phase\": " + fmtDouble (points[i].phaseDeg) + "}";
            if (i < points.size() - 1) json += ",";
            json += "\n";
        }
        json += indent + "],\n";
    };

    writePoints ("raw", result.raw);
    writePoints ("smoothed_1_12", result.smoothed_1_12);
    writePoints ("smoothed_1_3", result.smoothed_1_3);
}

juce::String freqResponseToJSON (const FreqResponse::Result& result,
                                  const Export::Context& context)
{
    juce::String json;
    json += "{\n";
    json += "  \"type\": \"frequency_response\",\n";
    appendContextFields (json, context, "  ");
    appendFreqArrays (json, result, "  ");

    // Remove trailing comma of the last array, then close the object.
    json = json.dropLastCharacters (2);  // remove ",\n"
    json += "\n";
    json += "}\n";
    return json;
}

juce::String freqResponseToJSON (const FreqResponse::Result& result,
                                  const juce::String& pluginName)
{
    Context context;
    context.pluginName = pluginName;
    return freqResponseToJSON (result, context);
}

/** Serialize the harmonic-analysis body ("tones"). If trailingComma is set,
 *  the emitted block ends with ",\n" instead of "\n". */
static void appendHarmonicTones (juce::String& json, const HarmonicAnalysis::Result& result,
                                 const juce::String& indent, bool trailingComma)
{
    json += indent + "\"tones\": [\n";

    for (size_t t = 0; t < result.tones.size(); ++t)
    {
        auto& tone = result.tones[t];
        json += indent + "  {\n";
        json += indent + "    \"fundamental_hz\": " + fmtDouble (tone.fundamentalFreq, 1) + ",\n";
        json += indent + "    \"fundamental_db\": " + fmtDouble (tone.fundamentalDB) + ",\n";
        json += indent + "    \"thd_percent\": " + fmtDouble (tone.thdPercent, 4) + ",\n";
        json += indent + "    \"harmonics\": [\n";

        for (size_t h = 0; h < tone.harmonics.size(); ++h)
        {
            auto& harm = tone.harmonics[h];
            json += indent + "      {\"order\": " + juce::String (harm.order)
                    + ", \"freq\": " + fmtDouble (harm.frequency, 1)
                    + ", \"mag_db\": " + fmtDouble (harm.magnitudeDB)
                    + ", \"percent\": " + fmtDouble (harm.percent, 4) + "}";
            if (h < tone.harmonics.size() - 1) json += ",";
            json += "\n";
        }

        json += indent + "    ]\n";
        json += indent + "  }";
        if (t < result.tones.size() - 1) json += ",";
        json += "\n";
    }

    json += indent + "]";
    if (trailingComma) json += ",";
    json += "\n";
}

juce::String harmonicAnalysisToJSON (const HarmonicAnalysis::Result& result,
                                      const Export::Context& context)
{
    juce::String json;
    json += "{\n";
    json += "  \"type\": \"harmonic_analysis\",\n";
    appendContextFields (json, context, "  ");
    appendHarmonicTones (json, result, "  ", false);
    json += "}\n";
    return json;
}

juce::String harmonicAnalysisToJSON (const HarmonicAnalysis::Result& result,
                                      const juce::String& pluginName)
{
    Context context;
    context.pluginName = pluginName;
    return harmonicAnalysisToJSON (result, context);
}

/** Serialize the compression-curve body ("curve" + "fitted"). If trailingComma
 *  is set, the emitted block ends with ",\n" instead of "\n". */
static void appendCompressionBody (juce::String& json, const CompressionCurve::Result& result,
                                   const juce::String& indent, bool trailingComma)
{
    json += indent + "\"curve\": [\n";

    for (size_t i = 0; i < result.curve.size(); ++i)
    {
        auto& p = result.curve[i];
        json += indent + "  {\"input_db\": " + fmtDouble (p.inputDB)
                + ", \"output_db\": " + fmtDouble (p.outputDB)
                + ", \"gr_db\": " + fmtDouble (p.gainReductionDB) + "}";
        if (i < result.curve.size() - 1) json += ",";
        json += "\n";
    }

    json += indent + "],\n";
    json += indent + "\"fitted\": {\n";
    json += indent + "  \"ratio\": " + fmtDouble (result.fitted.ratio) + ",\n";
    json += indent + "  \"threshold_db\": " + fmtDouble (result.fitted.thresholdDB) + ",\n";
    json += indent + "  \"knee_db\": " + fmtDouble (result.fitted.kneeDB) + "\n";
    json += indent + "}";
    if (trailingComma) json += ",";
    json += "\n";
}

/** Serialize the gain-reduction timeline body ("gr": sample_rate, num_points
 *  and the timeline point array). If trailingComma is set, the emitted block
 *  ends with ",\n" instead of "\n". */
static void appendGRBody (juce::String& json, const GainReduction::Result& result,
                          const juce::String& indent, bool trailingComma)
{
    json += indent + "\"gr\": {\n";
    json += indent + "  \"sample_rate\": " + fmtDouble (result.sampleRate, 1) + ",\n";
    json += indent + "  \"num_points\": " + juce::String (result.numPoints) + ",\n";
    json += indent + "  \"timeline\": [\n";

    for (size_t i = 0; i < result.timeline.size(); ++i)
    {
        json += indent + "    {\"t\": " + fmtDouble (result.timeline[i].timeSec, 4)
                + ", \"gr_db\": " + fmtDouble (result.timeline[i].grDB) + "}";
        if (i < result.timeline.size() - 1) json += ",";
        json += "\n";
    }

    json += indent + "  ]\n";
    json += indent + "}";
    if (trailingComma) json += ",";
    json += "\n";
}

/** Serialize the time-constant body ("tau": attack_sec, release_sec). If
 *  trailingComma is set, the emitted block ends with ",\n" instead of "\n". */
static void appendTauBody (juce::String& json, const TimeConstants::Result& result,
                           const juce::String& indent, bool trailingComma)
{
    json += indent + "\"tau\": {\n";
    json += indent + "  \"attack_sec\": " + fmtDouble (result.tauAttackSec, 6) + ",\n";
    json += indent + "  \"release_sec\": " + fmtDouble (result.tauReleaseSec, 6) + "\n";
    json += indent + "}";
    if (trailingComma) json += ",";
    json += "\n";
}

juce::String compressionCurveToJSON (const CompressionCurve::Result& result,
                                      const Export::Context& context)
{
    juce::String json;
    json += "{\n";
    json += "  \"type\": \"compression_curve\",\n";
    appendContextFields (json, context, "  ");
    appendCompressionBody (json, result, "  ", false);
    json += "}\n";
    return json;
}

juce::String compressionFamilyToJSON (const CompressionFamily::FamilyResult& result,
                                      const Export::Context& context)
{
    juce::String json;
    json += "{\n";
    json += "  \"type\": \"compression_family\",\n";
    json += "  \"context\": {\n";
    appendContextFields (json, context, "    ");
    json = json.dropLastCharacters (2);  // remove trailing ",\n" after source
    json += "\n";
    json += "  },\n";

    json += "  \"family\": [\n";
    for (size_t i = 0; i < result.entries.size(); ++i)
    {
        const auto& entry = result.entries[i];
        json += "    {\n";
        json += "      \"input_level_db\": " + fmtDouble (entry.inputLevelDB) + ",\n";
        json += "      \"speed\": " + fmtDouble (entry.speed) + ",\n";
        appendCompressionBody (json, entry.curve, "      ", true);
        appendGRBody (json, entry.gr, "      ", true);
        appendTauBody (json, entry.tau, "      ", false);
        json += "    }";
        if (i < result.entries.size() - 1) json += ",";
        json += "\n";
    }
    json += "  ]\n";
    json += "}\n";
    return json;
}

juce::String scanToJSON (const ScanEngine::ScanResult& scan,
                         MeasurementSession::Type type,
                         const Export::Context& context)
{
    juce::String json;
    json += "{\n";
    json += "  \"type\": \"scan\",\n";
    json += "  \"context\": {\n";
    appendContextFields (json, context, "    ");
    json = json.dropLastCharacters (2);  // remove trailing ",\n" after source
    json += "\n";
    json += "  },\n";

    json += "  \"scan\": {\n";
    json += "    \"param_id\": \"" + escapeJsonString (scan.paramId) + "\",\n";
    json += "    \"param_name\": \"" + escapeJsonString (scan.paramName) + "\",\n";
    json += "    \"values\": [";
    for (size_t i = 0; i < scan.values.size(); ++i)
    {
        if (i > 0) json += ", ";
        json += fmtDouble (scan.values[i], 6);
    }
    json += "],\n";
    json += "    \"param_texts\": [";
    for (size_t i = 0; i < scan.family.size(); ++i)
    {
        if (i > 0) json += ", ";
        json += "\"" + escapeJsonString (scan.family[i].paramValueText) + "\"";
    }
    json += "]\n";
    json += "  },\n";

    json += "  \"family\": [\n";
    for (size_t i = 0; i < scan.family.size(); ++i)
    {
        const auto& entry = scan.family[i];
        json += "    {\n";
        json += "      \"param_value_normalized\": " + fmtDouble (entry.paramValue, 6) + ",\n";
        json += "      \"param_value_text\": \"" + escapeJsonString (entry.paramValueText) + "\",\n";
        json += "      \"latency_samples\": " + juce::String (entry.latencySamples) + ",\n";
        json += "      \"result\": {\n";

        switch (type)
        {
            case MeasurementSession::Type::frequencyResponse:
                appendFreqArrays (json, entry.freq, "        ");
                break;
            case MeasurementSession::Type::harmonicAnalysis:
                appendHarmonicTones (json, entry.harmonic, "        ", true);
                break;
            case MeasurementSession::Type::compressionCurve:
                appendCompressionBody (json, entry.compression, "        ", true);
                break;
        }

        json = json.dropLastCharacters (2);  // remove trailing ",\n"
        json += "\n";
        json += "      }\n";
        json += "    }";
        if (i < scan.family.size() - 1) json += ",";
        json += "\n";
    }
    json += "  ]\n";
    json += "}\n";
    return json;
}

juce::String rawCaptureToJSON (int64_t samples, double rate, int blockSize,
                               const Export::Context& context)
{
    juce::String json;
    json += "{\n";
    json += "  \"type\": \"raw_capture\",\n";
    json += "  \"plugin\": \"" + escapeJsonString (context.pluginName) + "\",\n";
    json += "  \"class_id\": \"" + escapeJsonString (context.classId) + "\",\n";
    json += "  \"latency_samples\": " + juce::String (context.latencySamples) + ",\n";
    json += "  \"parameter_snapshot\": "
            + (context.paramSnapshot.isNotEmpty() ? context.paramSnapshot : juce::String ("{}")) + ",\n";
    appendSourceBlock (json, context, "  ");
    json += "  \"samples\": " + juce::String (samples) + ",\n";
    json += "  \"sample_rate\": " + juce::String (rate) + ",\n";
    json += "  \"block_size\": " + juce::String (blockSize) + "\n";
    json += "}\n";
    return json;
}

bool writeToFile (const juce::String& json, const juce::File& file)
{
    return file.replaceWithText (json);
}

}  // namespace Export
