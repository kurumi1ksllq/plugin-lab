#include "PluginManager.h"
#include "EditorCrashGuard.h"
#include "../utils/CrashLog.h"
#include <map>
#include <mutex>

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
    dedupeKnownPlugins();
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

bool PluginManager::cacheIsCurrent (const juce::String& fileOrIdentifier) const
{
    // 增量跳过判断（步骤 6，启动卡顿修复）：目录型 VST3 bundle 无
    // moduleinfo.json 时扫描走慢路径，缓存条目用内层 DLL 路径
    // （...\Contents\x86_64-win\X.vst3）+ 内层 DLL 的 mtime；而目录枚举产出
    // bundle 路径（...\X.vst3）。KnownPluginList::getTypeForFile 的精确匹配
    // 对此永不命中 → 这些 bundle 每轮热启动全量重扫（LoadLibrary + InitDll，
    // 每插件 1-3s——31.2s 热扫的主体）。这里把枚举路径同时匹配"精确"与
    // "bundle\Contents 前缀"条目，并按条目自己的 mtime 基准比较：
    //   - 内层/单文件条目：文件 mtime（正确的更新检测基准——touch DLL 即重扫）
    //   - bundle 目录条目：先按目录 mtime（快路径 moduleinfo.json 语义）；
    //     不符则回退 bundle 内第一个 .vst3（normalize 遗留条目的 lastFileModTime
    //     是内层 DLL mtime，目录 mtime 永远不相等）
    auto types = knownPlugins.getTypes();
    for (const auto& d : types)
    {
        const bool samePath = d.fileOrIdentifier == fileOrIdentifier
                           || d.fileOrIdentifier.startsWith (fileOrIdentifier + "\\Contents");
        if (samePath)
        {
            auto probe = juce::File (d.fileOrIdentifier);
            if (probe.isDirectory())
            {
                if (probe.getLastModificationTime() == d.lastFileModTime)
                    return true;
                juce::Array<juce::File> dlls;
                probe.findChildFiles (dlls, juce::File::findFiles, true, "*.vst3");
                for (const auto& f : dlls)
                    if (f.getLastModificationTime() == d.lastFileModTime)
                        return true;
                return false;
            }
            return probe.getLastModificationTime() == d.lastFileModTime;
        }
    }
    return false;
}

void PluginManager::dedupeKnownPlugins()
{
    // 扫描后清理（步骤 6）：KnownPluginList::scanAndAddFile 只 addType、从不删除
    // 旧条目——重扫一个"已存在 legacy bundle 路径条目"的文件会叠加一条新的
    // inner 条目，同一插件在列表/缓存里出现两次。按 bundle 归一化 key 去重，
    // 优先保留 inner 条目（mtime 基准 = 内层 DLL，正确），移除同 key 的
    // bundle 目录条目（normalize 遗留或快路径产物）。
    auto keyOf = [] (const juce::String& id)
    {
        const auto idx = id.indexOf ("\\Contents");
        return idx > 0 ? id.substring (0, idx) : id;
    };

    auto types = knownPlugins.getTypes();
    juce::StringArray innerKeys;
    for (const auto& d : types)
        if (d.fileOrIdentifier.contains ("Contents"))
            innerKeys.add (keyOf (d.fileOrIdentifier));

    juce::StringArray kept;
    for (const auto& d : types)
    {
        const auto key = keyOf (d.fileOrIdentifier);
        const bool isInner = d.fileOrIdentifier.contains ("Contents");
        if (kept.contains (key)
            || (! isInner && innerKeys.contains (key)))
            knownPlugins.removeType (d);
        else
            kept.add (key);
    }
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

    // 步骤 6（启动卡顿修复）：PluginDirectoryScanner 只暴露下一个待扫文件的
    // 名字（getNextPluginFileThatWillBeScanned），而增量跳过（cacheIsCurrent）
    // 需要完整路径。这里用与 scanner 队列同源的 searchPathsForPlugins 枚举
    // 目录，建 名字→路径 映射（getNextPluginFileThatWillBeScanned 内部同样走
    // getNameOfPluginFromIdentifier，key 一致）。
    std::map<juce::String, juce::String> pathByName;
    for (const auto& p : vst3Format->searchPathsForPlugins (
             juce::FileSearchPath (directory.getFullPathName()), true, true))
        pathByName[vst3Format->getNameOfPluginFromIdentifier (p)] = p;

    while (true)
    {
        // 扫描阶段黑名单（步骤 6，启动卡顿修复）：Pianoteq 等宿主杀手插件在扫描
        // 时无 desc.name 可用（还没构造），按待扫文件名拦截——直接 skip，不
        // LoadLibrary/InitDll。此前 prune 把它们从内存列表删除 → getTypeForFile
        // 永不命中 → 每轮热启动都全量重扫（InitDll 授权检查 CPU 密集，实测
        // 单插件 8s + 多核满载）。注意 getNextPluginFileThatWillBeScanned 在
        // nextIndex=0 时越界（JUCE Array[-1]），用 getProgress() < 1.0 保证
        // 队列非空。
        if (scanner.getProgress() < 1.0f)
        {
            const auto nextName = scanner.getNextPluginFileThatWillBeScanned();
            if (isBlacklistedName (nextName))
            {
                scanner.skipNextFile();
                continue;
            }
            // 增量跳过（cacheIsCurrent 用内层 DLL 的 mtime 精确判断；目录型
            // bundle 的 bundle 路径可命中缓存的 inner 前缀条目）。
            const auto it = pathByName.find (nextName);
            if (it != pathByName.end() && cacheIsCurrent (it->second))
            {
                scanner.skipNextFile();
                continue;
            }
        }

        // 看门狗进度（计划步骤 4）：每文件报告 progress + 当前文件路径（bundle
        // key，经 pathByName 映射）——消息线程 Timer 据此检测无进展超时。
        {
            const auto prog = scanner.getProgress();
            juce::String currentPath;
            if (scanner.getProgress() < 1.0f)
            {
                const auto it = pathByName.find (scanner.getNextPluginFileThatWillBeScanned());
                if (it != pathByName.end())
                    currentPath = it->second;
            }
            updateScanProgress (prog, currentPath);
        }

        juce::String name;
        bool ok = false;
        try { ok = scanner.scanNextFile (true, name); }
        catch (...) { CRASH_LOG_ERR ("Scan crash", "stopping scan"); break; }
        if (!ok) break;
        CRASH_LOG_INFO ("Discovered", name);
    }
}

void PluginManager::beginScan()
{
    scanRunning = true;
    scanProgress = 0.0f;
    lastScanProgressMs = juce::Time::getMillisecondCounter();
    {
        std::lock_guard<std::mutex> lock (scanFileLock);
        currentScanFile = {};
    }
}

void PluginManager::updateScanProgress (float progress, const juce::String& currentFile)
{
    scanProgress = progress;
    lastScanProgressMs = juce::Time::getMillisecondCounter();
    {
        std::lock_guard<std::mutex> lock (scanFileLock);
        currentScanFile = currentFile;
    }
}

void PluginManager::endScan()
{
    scanRunning = false;
    scanProgress = 1.0f;
}

juce::String PluginManager::getCurrentScanFile() const
{
    std::lock_guard<std::mutex> lock (scanFileLock);
    return currentScanFile;
}

bool PluginManager::handleScanHang()
{
    // 看门狗（计划步骤 4）：扫描线程卡在某个插件 DLL 里（scanNextFile 永不返回），
    // 无法从进程内终止。把当前文件加入黑名单（bundle key——与扫描枚举路径一致，
    // 见 cacheIsCurrent），并立即持久化（卡死的扫描永远到不了 saveCache，不持久化
    // 则重启会再次挂同一插件）。挂起计数封顶 kMaxScanHangs：每次挂起泄漏一个扫描
    // 线程 + 锁住一个 DLL，必须封顶。
    const auto file = getCurrentScanFile();
    if (file.isNotEmpty())
    {
        auto blacklistKey = file;
        const auto contentsIdx = blacklistKey.indexOf ("\\Contents");
        if (contentsIdx > 0)
            blacklistKey = blacklistKey.substring (0, contentsIdx);
        knownPlugins.addToBlacklist (blacklistKey);
        saveCache();          // 立即持久化——重启不重挂同一插件
    }

    const int hangs = ++scanHangCount;
    CRASH_LOG_WARN ("Scan hang detected",
        file + " - blacklisted (" + juce::String (hangs) + "/"
        + juce::String (kMaxScanHangs) + ")");
    return hangs >= kMaxScanHangs;
}

void PluginManager::scanSystemDirectories()
{
    // P0 (plan step 1)：增量扫描。绝不能 clear() —— 否则缓存增量被抹掉，每次都
    // 全量重扫。先 loadCache（有效缓存 → scanNextFile(true) 对 mtime 未变的文件
    // 零 DLL 加载，热启动从分钟级降到秒级）；缓存缺失/损坏/版本不符则回退全量。
    beginScan();
    try
    {
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

        // 扫描可能叠加了重复条目（scanAndAddFile 只加不删）→ 去重后再落盘。
        dedupeKnownPlugins();
        saveCache();
    }
    catch (...)
    {
        CRASH_LOG_ERR ("Scan", "exception in scanSystemDirectories");
    }
    endScan();
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

    auto* format = formatManager.getFormat (0);
    if (format == nullptr)
    {
        CRASH_LOG_ERR ("Plugin load", "no format");
        return nullptr;
    }

    // 加载超时（计划步骤 3）：显式 createPluginInstanceAsync + WaitableEvent 超时。
    // JUCE 同步 createInstanceFromDescription 从后台线程调用时内部就是
    // postMessage(AsyncCreateMessage) + 无限 wait() —— 插件 initialise 挂起会让
    // 等待线程永久阻塞。这里换成我们自己带超时的 wait：超时→线程脱出 + 预防性
    // 黑名单（下次扫描/加载跳过）。注意真正挂起时消息线程（创建执行处）仍冻结
    // —— 进程内不可恢复（只能杀进程），由黑名单 + 死马踏板预防下次，文档化限制。
    // 迟到回调防护：LoadState 由 shared_ptr 持有（回调侧与调用侧共享），loadPlugin
    // 返回（超时）时置 alive=false —— 之后消息线程才完成的创建回调被丢弃，且不会
    // 触碰任何已析构状态（状态本身由回调的 shared_ptr 保活）。回调不捕获 this。
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
            return;                          // 迟到回调（loadPlugin 已超时返回）→ 忽略
        state->instance = std::move (instance);
        state->error = errorMessage;
        state->done.signal();
    };

    if (asyncCreateOverride)
        asyncCreateOverride (desc, sampleRate, blockSize, std::move (onCreated));
    else
        format->createPluginInstanceAsync (desc, sampleRate, blockSize, std::move (onCreated));

    if (! state->done.wait (loadTimeoutMs))
    {
        state->alive = false;
        // 超时：等待线程脱出。黑名单 key 用 bundle 路径（内层 DLL 路径枚举时
        // 不匹配；bundle 路径与扫描枚举一致，见 cacheIsCurrent）。
        auto blacklistKey = desc.fileOrIdentifier;
        const auto contentsIdx = blacklistKey.indexOf ("\\Contents");
        if (contentsIdx > 0)
            blacklistKey = blacklistKey.substring (0, contentsIdx);
        knownPlugins.addToBlacklist (blacklistKey);

        CRASH_LOG_WARN ("Plugin load timeout",
            desc.name + " - creation did not finish in "
            + juce::String (loadTimeoutMs) + "ms, blacklisted for next run");
        return nullptr;
    }

    if (! state->instance)
    {
        CRASH_LOG_WARN ("Plugin load failed", desc.name + ": " + state->error);
        return nullptr;
    }
    // prepareToPlay is deferred to the measurement thread (SweepRunner::run)
    // to ensure it and processBlock run on the same thread — required by some
    // VST3 plugins (e.g. FabFilter Pro-Q 4).
    CRASH_LOG_INFO ("Plugin load ok", desc.name);
    return std::move (state->instance);
}

juce::AudioProcessorEditor* PluginManager::createEditorSafe (juce::AudioPluginInstance* plugin)
{
    return EditorCrashGuard::createEditor (plugin);
}
