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

juce::String freqResponseToJSON (const FreqResponse::Result& result,
                                  const Export::Context& context)
{
    juce::String json;
    json += "{\n";
    json += "  \"type\": \"frequency_response\",\n";
    json += "  \"plugin\": \"" + escapeJsonString (context.pluginName) + "\",\n";
    json += "  \"class_id\": \"" + escapeJsonString (context.classId) + "\",\n";
    json += "  \"latency_samples\": " + juce::String (context.latencySamples) + ",\n";
    json += "  \"sample_rate\": " + juce::String (context.sampleRate) + ",\n";
    json += "  \"measurement\": {\n";
    json += "    \"sample_rate\": " + juce::String (context.sampleRate) + ",\n";
    json += "    \"block_size\": " + juce::String (context.blockSize) + "\n";
    json += "  },\n";
    // paramSnapshot is a JSON object string already; keep it valid even if empty.
    json += "  \"parameter_snapshot\": "
            + (context.paramSnapshot.isNotEmpty() ? context.paramSnapshot : juce::String ("{}")) + ",\n";

    auto writePoints = [&](const juce::String& name,
                            const std::vector<FreqResponse::Point>& points)
    {
        json += "  \"" + name + "\": [\n";
        for (size_t i = 0; i < points.size(); ++i)
        {
            json += "    {\"f\": " + fmtDouble (points[i].frequency, 1)
                    + ", \"mag\": " + fmtDouble (points[i].magnitudeDB)
                    + ", \"phase\": " + fmtDouble (points[i].phaseDeg) + "}";
            if (i < points.size() - 1) json += ",";
            json += "\n";
        }
        json += "  ],\n";
    };

    writePoints ("raw", result.raw);
    writePoints ("smoothed_1_12", result.smoothed_1_12);
    writePoints ("smoothed_1_3", result.smoothed_1_3);

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

juce::String harmonicAnalysisToJSON (const HarmonicAnalysis::Result& result,
                                      const juce::String& pluginName)
{
    juce::String json;
    json += "{\n";
    json += "  \"type\": \"harmonic_analysis\",\n";
    json += "  \"plugin\": \"" + escapeJsonString (pluginName) + "\",\n";
    json += "  \"sample_rate\": " + juce::String (result.sampleRate) + ",\n";
    json += "  \"tones\": [\n";

    for (size_t t = 0; t < result.tones.size(); ++t)
    {
        auto& tone = result.tones[t];
        json += "    {\n";
        json += "      \"fundamental_hz\": " + fmtDouble (tone.fundamentalFreq, 1) + ",\n";
        json += "      \"fundamental_db\": " + fmtDouble (tone.fundamentalDB) + ",\n";
        json += "      \"thd_percent\": " + fmtDouble (tone.thdPercent, 4) + ",\n";
        json += "      \"harmonics\": [\n";

        for (size_t h = 0; h < tone.harmonics.size(); ++h)
        {
            auto& harm = tone.harmonics[h];
            json += "        {\"order\": " + juce::String (harm.order)
                    + ", \"freq\": " + fmtDouble (harm.frequency, 1)
                    + ", \"mag_db\": " + fmtDouble (harm.magnitudeDB)
                    + ", \"percent\": " + fmtDouble (harm.percent, 4) + "}";
            if (h < tone.harmonics.size() - 1) json += ",";
            json += "\n";
        }

        json += "      ]\n";
        json += "    }";
        if (t < result.tones.size() - 1) json += ",";
        json += "\n";
    }

    json += "  ]\n";
    json += "}\n";
    return json;
}

juce::String compressionCurveToJSON (const CompressionCurve::Result& result,
                                      const juce::String& pluginName)
{
    juce::String json;
    json += "{\n";
    json += "  \"type\": \"compression_curve\",\n";
    json += "  \"plugin\": \"" + escapeJsonString (pluginName) + "\",\n";
    json += "  \"curve\": [\n";

    for (size_t i = 0; i < result.curve.size(); ++i)
    {
        auto& p = result.curve[i];
        json += "    {\"input_db\": " + fmtDouble (p.inputDB)
                + ", \"output_db\": " + fmtDouble (p.outputDB)
                + ", \"gr_db\": " + fmtDouble (p.gainReductionDB) + "}";
        if (i < result.curve.size() - 1) json += ",";
        json += "\n";
    }

    json += "  ],\n";
    json += "  \"fitted\": {\n";
    json += "    \"ratio\": " + fmtDouble (result.fitted.ratio) + ",\n";
    json += "    \"threshold_db\": " + fmtDouble (result.fitted.thresholdDB) + ",\n";
    json += "    \"knee_db\": " + fmtDouble (result.fitted.kneeDB) + "\n";
    json += "  }\n";
    json += "}\n";
    return json;
}

bool writeToFile (const juce::String& json, const juce::File& file)
{
    return file.replaceWithText (json);
}

}  // namespace Export
