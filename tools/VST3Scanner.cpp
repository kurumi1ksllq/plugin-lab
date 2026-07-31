#include <iostream>
#include <JuceHeader.h>

static juce::String esc (const juce::String& s)
{
    return "\"" + s.replace ("\\", "\\\\").replace ("\"", "\\\"")
                  .replace ("\n", "\\n").replace ("\r", "\\r").replace ("\t", "\\t") + "\"";
}

int main (int argc, char* argv[])
{
    if (argc < 2) return 1;

    juce::ScopedJuceInitialiser_GUI initialiser;

    juce::String argPath (argv[1]);
    argPath = argPath.unquoted();
    const auto pluginFile = juce::File (argPath);
    if (! pluginFile.exists()) return 1;

    juce::AudioPluginFormatManager fmtMgr;
    fmtMgr.addFormat (std::make_unique<juce::VST3PluginFormat>());
    auto* vst3 = fmtMgr.getFormat (0);
    if (vst3 == nullptr) return 1;

    juce::KnownPluginList known;
    juce::OwnedArray<juce::PluginDescription> found;
    if (! known.scanAndAddFile (pluginFile.getFullPathName(), false, found, *vst3))
        return 1;
    if (found.isEmpty()) return 1;

    std::cout << "[\n";
    for (int i = 0; i < found.size(); ++i)
    {
        auto* d = found[i];
        std::cout << "  {\n";
        std::cout << "    \"name\": "       << esc (d->name) << ",\n";
        std::cout << "    \"manufacturer\": " << esc (d->manufacturerName) << ",\n";
        std::cout << "    \"format\": "     << esc (d->pluginFormatName) << ",\n";
        std::cout << "    \"file\": "       << esc (d->fileOrIdentifier) << ",\n";
        std::cout << "    \"category\": "   << esc (d->category) << ",\n";
        std::cout << "    \"version\": "    << esc (d->version) << ",\n";
        std::cout << "    \"descriptive\": " << esc (d->descriptiveName) << ",\n";
        std::cout << "    \"isSynth\": "    << (d->isInstrument ? "true" : "false") << ",\n";
        std::cout << "    \"uniqueId\": "   << d->uniqueId << ",\n";
        std::cout << "    \"numInputs\": "  << d->numInputChannels << ",\n";
        std::cout << "    \"numOutputs\": " << d->numOutputChannels << ",\n";
        std::cout << "    \"hasARA\": "     << (d->hasARAExtension ? "true" : "false") << "\n";
        std::cout << "  }";
        if (i < found.size() - 1) std::cout << ",";
        std::cout << "\n";
    }
    std::cout << "]\n";
    return 0;
}
