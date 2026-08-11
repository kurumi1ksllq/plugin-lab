#include "SuicidePlugin.h"

//==============================================================================
// This creates new instances of the plugin. Required by the JUCE VST3 wrapper
// (juce_audio_plugin_client_VST3.cpp -> juce_CreatePluginFilter.h:44 calls
// the global ::createPluginFilter()).
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new SuicidePlugin();
}
