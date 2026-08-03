#include "PluginManager.h"
#include "EditorCrashGuard.h"
#include "../utils/CrashLog.h"

PluginManager::PluginManager()
{
    formatManager.addFormat (std::make_unique<juce::VST3PluginFormat>());

    // Default cache location: %APPDATA%/PluginLab/pluginlist.xml (tests override
    // it via setCacheFile). The dead-man's-pedal lives next to it.
    cacheFile = juce::File::getSpecialLocation (
        juce::File::SpecialLocationType::userApplicationDataDirectory)
        .getChildFile ("PluginLab").getChildFile ("pluginlist.xml");
}

PluginManager::~PluginManager() {}

bool PluginManager::isBlacklistedName (const juce::String& name)
{
    // 已知会直接终止宿主进程的插件黑名单（Pianoteq 9/8/7 在 createPluginInstance
    // 内部调用 ExitProcess/TerminateProcess，绕过所有 try/catch 与 SEH 保护，
    // 导致整个宿主进程无征兆退出）。扫描（缓存剪枝）与加载（loadPlugin 拦截）
    // 共用此纯函数，杜绝两处名单漂移。
    static const juce::StringArray blacklistedPlugins
    {
        "Pianoteq 9",
        "Pianoteq 8",
        "Pianoteq 7"
    };
    for (const auto& bl : blacklistedPlugins)
        if (name.containsIgnoreCase (bl))
            return true;
    return false;
}

bool PluginManager::loadCache()
{
    if (! cacheFile.existsAsFile())
        return false;
    auto xml = juce::XmlDocument::parse (cacheFile);
    if (xml == nullptr || ! xml->hasTagName ("KNOWNPLUGINS"))
        return false;                       // 损坏缓存 → 调用方回退全量重扫
    if (xml->getIntAttribute ("version", -1) != kCacheVersion)
        return false;                       // 旧版缓存 → 调用方回退全量重扫

    knownPlugins.recreateFromXml (*xml);
    pruneKnownPlugins();
    return true;
}

void PluginManager::saveCache()
{
    if (! cacheFile.getParentDirectory().exists())
        cacheFile.getParentDirectory().createDirectory();

    auto xml = knownPlugins.createXml();
    if (xml == nullptr)
        return;
    xml->setAttribute ("version", kCacheVersion);

    // 原子写：先写临时文件再原子替换（ReplaceFile），扫描/写入中途崩溃不会
    // 留下半截缓存文件。残留的 .tmp 由下次 saveCache 覆盖。
    auto temp = cacheFile.withFileExtension (".xml.tmp");
    if (temp.replaceWithText (xml->toString()))
        temp.replaceFileIn (cacheFile);
}

void PluginManager::pruneKnownPlugins()
{
    // 移除幽灵条目（插件文件已不存在——卸载/删除后残留在缓存里）与宿主杀手
    // 插件（Pianoteq 家族）。getTypes() 返回副本，迭代安全；removeType 同步
    // 从原列表移除。
    auto types = knownPlugins.getTypes();
    for (const auto& d : types)
    {
        if (! juce::File (d.fileOrIdentifier).exists()
            || isBlacklistedName (d.name))
            knownPlugins.removeType (d);
    }
}

void PluginManager::scanDirectory (const juce::File& directory, const juce::File& deadMansPedalFile)
{
    if (! directory.isDirectory()) return;
    auto* vst3Format = formatManager.getFormat (0);
    if (vst3Format == nullptr) return;

    juce::PluginDirectoryScanner scanner (knownPlugins, *vst3Format,
        juce::FileSearchPath (directory.getFullPathName()),
        true, deadMansPedalFile, true);

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
    // P0 (plan step 1)：增量扫描。绝不能 clear() —— 否则缓存增量被抹掉，每次都
    // 全量重扫。先 loadCache（有效缓存 → scanNextFile(true) 对 mtime 未变的文件
    // 零 DLL 加载，热启动从分钟级降到秒级）；缓存缺失/损坏/版本不符则回退全量。
    if (! loadCache())
        knownPlugins.clear();

    // 确保缓存目录存在——它同时承载死马踏板文件（PluginDirectoryScanner 在
    // scanNextFile 前把当前插件路径写进踏板，成功后移除；挂起/崩溃 = 路径残留 →
    // 下次构造 scanner 时自动 addToBlacklist 并移到队尾，见
    // juce_PluginDirectoryScanner.cpp:73-78,107-117）。
    cacheFile.getParentDirectory().createDirectory();
    const auto pedal = cacheFile.getSiblingFile ("deadMansPedal");

    scanDirectory (juce::File ("C:\\Program Files\\Common Files\\VST3"), pedal);
    auto localDir = juce::File::getSpecialLocation (
        juce::File::SpecialLocationType::userApplicationDataDirectory)
        .getChildFile ("Programs\\Common\\VST3");
    if (localDir.isDirectory()) scanDirectory (localDir, pedal);

    saveCache();
}

std::unique_ptr<juce::AudioPluginInstance> PluginManager::loadPlugin (
    const juce::PluginDescription& desc, double sampleRate, int blockSize)
{
    CRASH_LOG_INFO ("Plugin load start", desc.name);

    if (isBlacklistedName (desc.name))
    {
        CRASH_LOG_WARN ("Plugin blacklisted",
            desc.name + " - known to terminate host process, skipped");
        return nullptr;
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
        // prepareToPlay is deferred to the measurement thread (SweepRunner::run)
        // to ensure it and processBlock run on the same thread — required by some
        // VST3 plugins (e.g. FabFilter Pro-Q 4).
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
    return EditorCrashGuard::createEditor (plugin);
}
