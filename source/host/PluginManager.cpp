#include "PluginManager.h"
#include "../utils/CrashLog.h"

PluginManager::PluginManager()
{
    formatManager.addFormat (std::make_unique<juce::VST3PluginFormat>());
}

PluginManager::~PluginManager() {}

void PluginManager::scanDirectory (const juce::File& directory)
{
    if (! directory.isDirectory()) return;
    auto* vst3Format = formatManager.getFormat (0);
    if (vst3Format == nullptr) return;

    juce::PluginDirectoryScanner scanner (knownPlugins, *vst3Format,
        juce::FileSearchPath (directory.getFullPathName()),
        true, juce::File(), true);

    while (true)
    {
        juce::String name;
        bool ok = false;
        try { ok = scanner.scanNextFile (true, name); }
        catch (...) { CRASH_LOG_ERR ("Scan crash", "stopping scan"); break; }
        if (!ok) break;
        CRASH_LOG_INFO ("Discovered", name);
    }
}

void PluginManager::scanSystemDirectories()
{
    knownPlugins.clear();
    scanDirectory (juce::File ("C:\\Program Files\\Common Files\\VST3"));
    auto localDir = juce::File::getSpecialLocation (
        juce::File::SpecialLocationType::userApplicationDataDirectory)
        .getChildFile ("Programs\\Common\\VST3");
    if (localDir.isDirectory()) scanDirectory (localDir);
}

std::unique_ptr<juce::AudioPluginInstance> PluginManager::loadPlugin (
    const juce::PluginDescription& desc, double sampleRate, int blockSize)
{
    CRASH_LOG_INFO ("Plugin load start", desc.name);

    // 已知会直接终止宿主进程的插件黑名单（Pianoteq 9 在 createPluginInstance 内部
    // 调用 ExitProcess/TerminateProcess，绕过所有 try/catch 与 SEH 保护，导致整个
    // 宿主进程无征兆退出）。在进入插件 DLL 之前拦截，避免宿主被杀。
    static const juce::StringArray blacklistedPlugins
    {
        "Pianoteq 9",
        "Pianoteq 8",
        "Pianoteq 7"
    };
    for (const auto& bl : blacklistedPlugins)
    {
        if (desc.name.containsIgnoreCase (bl))
        {
            CRASH_LOG_WARN ("Plugin blacklisted",
                desc.name + " - known to terminate host process, skipped");
            return nullptr;
        }
    }

    juce::String errorMessage;
    try
    {
        auto instance = formatManager.createPluginInstance (desc, sampleRate, blockSize, errorMessage);
        if (!instance)
        {
            CRASH_LOG_WARN ("Plugin load failed", desc.name + ": " + errorMessage);
            return nullptr;
        }
        instance->prepareToPlay (sampleRate, blockSize);
        CRASH_LOG_INFO ("Plugin load ok", desc.name);
        return instance;
    }
    catch (...)
    {
        CRASH_LOG_ERR ("Plugin load crash", "caught");
        return nullptr;
    }
}

juce::AudioProcessorEditor* PluginManager::createEditorSafe (juce::AudioPluginInstance* plugin)
{
    if (!plugin) return nullptr;
    try { return plugin->createEditorAndMakeActive(); }
    catch (...) { CRASH_LOG_ERR ("Editor crash", plugin->getName()); return nullptr; }
}
