# PluginLab 块 C：稳定加固 + 残余小项开发计划

> 2026-08-08 制定。路线图总览见 `docs/roadmap-next.md` 块 0（当前块，7 任务）。块 A（批量采集）已完成（docs/plan-batch-pipeline.md）。
> 执行原则：TDD（RED→GREEN）+ 真机验收（**全 IPC 驱动，不控制鼠标**）+ 每任务原子提交 push（GitHub 存档）+ 收尾代码审查。
> 执行方式：**inline executing-plans**（任务间共享 TestPlugin.h / CommandParserStubs.cpp / CMakeLists，顺序执行避免并发写冲突；每任务检查点）。

## 目标

把测量路径的异常逃逸（LA-2A 静默退出根因）用 /EHa + catch 堵死；EditorCrashGuard 真实实现进测试目标；CGII 0 类型插件预防性黑名单消除热启重扫；schema 与实现最终核对；getParams Band Used 状态验证锁定；观察者生命周期加固。

## 全局约束（每任务隐式包含）

- 构建：`cmake -S . -B build && cmake --build build --config Release`（/W4 /permissive- /WX /utf-8 警告即错误）
- 测试：`cmake -S . -B build -DBUILD_TESTS=ON && cmake --build build --config Release && ctest --test-dir build -C Release --timeout 180`；158+ 全绿，连跑 2 次
- /EHa 只用于指定 TU（PluginManager + EditorCrashGuard + **SweepRunner**——本块授权例外，roadmap 明示"或给 SweepRunner.cpp 开 /EHa"）；根 + tests 两个 CMakeLists 都要 set_source_files_properties
- 改协议/导出：四件套（Protocol.h / CommandParser / tests / docs/data-schema.md）
- 提交：Conventional Commits（英文），每任务一 commit + push origin main
- 真机：`tools/ipc_client.ps1` / `tools/pipe_client.py` 驱动，禁鼠标控制
- 铁律：无失败测试不写生产代码；无新鲜验证证据不宣称完成

---

## 任务 1：测量路径异常保护（最高优先，LA-2A 根因）

**Files:**
- Modify: `source/capture/SweepRunner.cpp`（run() 内 plugin 调用加 try/catch）
- Modify: `CMakeLists.txt`（SweepRunner.cpp 加 /EHa）
- Modify: `tests/CMakeLists.txt`（SweepRunner.cpp 加 /EHa）
- Modify: `tests/TestPlugin.h`（throw 注入）
- Modify: `tests/CommandParserStubs.cpp`（CrashLog stub 改记录型）
- Test: `tests/SweepRunnerTests.cpp`、`tests/CommandParserTests.cpp`
- Docs: `source/AGENTS.md`、`tests/AGENTS.md`（/EHa 例外 + stub 记录说明）

**Interfaces:**
- TestPlugin 新增：`void setThrowOnProcessBlock(bool)` / `void setThrowOnPrepareToPlay(bool)`——processBlock/prepareToPlay 抛 `std::runtime_error("TestPlugin injected ... failure")`
- CommandParserStubs.cpp 新增全局测试辅助（测试文件 `extern` 声明）：`void clearCrashLog()` / `int crashLogErrorCount()` / `bool crashLogContains(const juce::String& substr)`
- SweepRunner::run() 失败语义：返回 false（runAndAnalyze 已映射为 `"error":"measurement failed"` 错误响应）

- [ ] **Step 1（RED）：TestPlugin 加 throw 注入**

```cpp
// TestPlugin.h 新增（成员 + 配置 + 两处抛点）：
//  public:
//      void setThrowOnProcessBlock (bool t) noexcept { throwOnProcessBlock = t; }
//      void setThrowOnPrepareToPlay (bool t) noexcept { throwOnPrepareToPlay = t; }
//  prepareToPlay() 开头:  if (throwOnPrepareToPlay) throw std::runtime_error ("TestPlugin injected prepareToPlay failure");
//  processBlock(float) 开头: if (throwOnProcessBlock) throw std::runtime_error ("TestPlugin injected processBlock failure");
//  private: bool throwOnProcessBlock = false; bool throwOnPrepareToPlay = false;
//  需要 #include <stdexcept>
```

- [ ] **Step 2（RED）：SweepRunner 异常测试**

```cpp
// SweepRunnerTests.cpp 新增：
TEST_CASE ("SweepRunner: plugin throwing in processBlock returns false without crashing",
           "[sweeprunner][exception]")
{
    // Arrange
    TestPlugin plugin;
    SineSweep sweep;
    sweep.setFrequencyRange (20.0, 20000.0);
    sweep.setDuration (0.05);
    sweep.setAmplitude (0.5);
    SweepRunner runner;
    runner.prepare (48000.0, 512);
    runner.setGenerator (&sweep);
    runner.setPlugin (&plugin);
    plugin.setThrowOnProcessBlock (true);

    // Act
    const bool runResult = runner.run();

    // Assert — run() returns false (failed measurement), host alive
    REQUIRE_FALSE (runResult);
    REQUIRE_FALSE (runner.isRunning());
}

TEST_CASE ("SweepRunner: plugin throwing in prepareToPlay returns false",
           "[sweeprunner][exception]")
{
    TestPlugin plugin;
    SineSweep sweep;
    sweep.setFrequencyRange (20.0, 20000.0);
    sweep.setDuration (0.05);
    sweep.setAmplitude (0.5);
    SweepRunner runner;
    runner.prepare (48000.0, 512);
    runner.setGenerator (&sweep);
    runner.setPlugin (&plugin);
    plugin.setThrowOnPrepareToPlay (true);

    const bool runResult = runner.run();

    REQUIRE_FALSE (runResult);
}
```

- [ ] **Step 3（RED）：CrashLog stub 改记录型 + 命令级异常测试**

```cpp
// CommandParserStubs.cpp：CrashLog::write 从 no-op 改为记录（static + mutex），
// 并提供测试辅助（同文件内定义）：
//   static std::mutex gCrashLogMutex;
//   static juce::StringArray gCrashLogErrors;
//   void CrashLog::write (...) { std::lock_guard lock(gCrashLogMutex); if (level == CrashLog::Error) gCrashLogErrors.add (operation + " | " + detail); }
//   void clearCrashLog() { ... clear ... }
//   int crashLogErrorCount() { return gCrashLogErrors.size(); }
//   bool crashLogContains (const juce::String& substr) { ... any entry contains ... }
// 注意：保持 EditorCrashGuard 桩不动（任务 2 才移除）。

// CommandParserTests.cpp 新增（extern 声明三个辅助；参考现有 measure 用例的消息线程同步路径）：
TEST_CASE ("CommandParser: measure returns ok:false when plugin processBlock throws",
           "[commandparser][measure][exception]")
{
    // Arrange
    TestPlugin plugin;
    plugin.setThrowOnProcessBlock (true);
    MeasurementSession session;
    session.setSampleRate (48000.0);
    session.setBlockSize (512);
    session.setPluginInstance (&plugin);
    session.setMeasurementType (MeasurementSession::Type::frequencyResponse);
    CommandParser parser;
    parser.setSession (&session);
    parser.setPluginInstance (&plugin);

    clearCrashLog();

    // Act
    auto response = parser.handleCommand (R"({"cmd":"measure","type":"frequency_response"})");

    // Assert — error response, host alive, crash log recorded the exception
    REQUIRE (response.contains (R"("ok":false)"));
    REQUIRE (crashLogErrorCount() >= 1);
    REQUIRE (crashLogContains ("Sweep"));
}
```

- [ ] **Step 4：跑测试确认 RED**（先看崩溃逃逸——当前 run() 无保护，测试应崩溃/terminate 或无法编译，记录输出）

- [ ] **Step 5（GREEN）：SweepRunner::run() 加保护**

```cpp
// SweepRunner.cpp 重构（保持循环结构不变，只包 plugin 调用）：
// 1) prepare 阶段：
//    try { if (pluginPrepared) plugin->releaseResources();
//          plugin->setNonRealtime (true);
//          plugin->prepareToPlay (sampleRate, blockSize); }
//    catch (const std::exception& e) { CRASH_LOG_ERR ("Sweep prepare", e.what()); running = false; return false; }
//    catch (...) { CRASH_LOG_ERR ("Sweep prepare", "unknown exception"); running = false; return false; }
//    pluginPrepared = true;
// 2) 主循环与 tail 循环的 processBlock 调 point 抽局部 lambda：
//    const auto safeProcessBlock = [&] (juce::AudioBuffer<float>& block) -> bool
//    {
//        try { plugin->processBlock (block, emptyMidi); return true; }
//        catch (const std::exception& e) { CRASH_LOG_ERR ("Sweep process", e.what()); return false; }
//        catch (...) { CRASH_LOG_ERR ("Sweep process", "unknown exception"); return false; }
//    };
//    两处 `plugin->processBlock (wetBlock, emptyMidi);` 改为：
//    if (! safeProcessBlock (wetBlock)) { running = false; return false; }
//    （直接 return false；跳过收尾 releaseResources——插件已异常，尽力而为由下次 prepare 的
//     pluginPrepared 分支处理：pluginPrepared 保持 true，下次 run 先 try releaseResources）
// 3) 正常收尾的 releaseResources/setNonRealtime 也包 try/catch（同类防御）：
//    try { if (pluginPrepared) { plugin->releaseResources(); pluginPrepared = false; }
//          plugin->setNonRealtime (false); }
//    catch (const std::exception& e) { CRASH_LOG_WARN ("Sweep teardown", e.what()); }
//    catch (...) { CRASH_LOG_WARN ("Sweep teardown", "unknown exception"); }
// 注意：取消路径（cancelled）行为不变，仍走正常收尾。
```

- [ ] **Step 6：跑测试确认 GREEN**（3 个新用例 + 相关回归）

- [ ] **Step 7：CMake 加 /EHa**

```cmake
# CMakeLists.txt（根，PluginManager/EditorCrashGuard 旁）：
set_source_files_properties(source/capture/SweepRunner.cpp PROPERTIES
    COMPILE_FLAGS "/EHa"
)
# tests/CMakeLists.txt（同）：
set_source_files_properties(../source/capture/SweepRunner.cpp PROPERTIES
    COMPILE_FLAGS "/EHa"
)
```

- [ ] **Step 8：文档同步**——source/AGENTS.md "/EHa 只用于 host/ 两个 TU" 更新为"host/ 两个 TU + capture/SweepRunner.cpp（块 C 测量路径异常保护，2026-08-08 授权例外）"；tests/AGENTS.md 注明 CrashLog stub 为记录型 + 辅助函数

- [ ] **Step 9：全量构建 + ctest 连跑 2 次**，证据记录

- [ ] **Step 10：真机回归**——IPC 加载 + measure frequency_response 正常返回（证明保护未破坏正常路径）；日志无新 ERROR

- [ ] **Step 11：提交**

```bash
git add source/capture/SweepRunner.cpp CMakeLists.txt tests/CMakeLists.txt tests/TestPlugin.h tests/CommandParserStubs.cpp tests/SweepRunnerTests.cpp tests/CommandParserTests.cpp source/AGENTS.md tests/AGENTS.md
git commit -m "fix(capture): protect measurement path from plugin exceptions (EHa + CRASH_LOG)"
git push origin main
```

---

## 任务 2：EditorCrashGuard 真实实现编入测试目标

**Files:**
- Modify: `tests/CMakeLists.txt`（加 `../source/host/EditorCrashGuard.cpp` + /EHa）
- Modify: `tests/CommandParserStubs.cpp`（移除 EditorCrashGuard 桩，保留 CrashLog 记录桩）
- Modify: `tests/TestPlugin.h`（createEditor 抛注入 + editorBeingDeleted 抛注入）
- Create: `tests/EditorCrashGuardTests.cpp`
- Docs: `tests/AGENTS.md`（覆盖映射更新）

**Interfaces:**
- TestPlugin 新增：`void setThrowOnCreateEditor(bool)`（override `createEditorAndMakeActive()` 抛 `std::runtime_error`）；`void setThrowOnEditorBeingDeleted(bool)`（override `editorBeingDeleted()` 抛）；`int getEditorBeingDeletedCount()`

- [ ] **Step 1（RED）：TestPlugin 扩展 + 新测试文件**

```cpp
// TestPlugin.h 新增 override（AudioProcessor 虚函数）：
//   juce::AudioProcessorEditor* createEditorAndMakeActive() override
//   {
//       if (throwOnCreateEditor) throw std::runtime_error ("TestPlugin injected createEditor failure");
//       return nullptr;
//   }
//   void editorBeingDeleted (juce::AudioProcessorEditor* editor) override
//   {
//       ++editorBeingDeletedCount;
//       if (throwOnEditorBeingDeleted) throw std::runtime_error ("TestPlugin injected editorBeingDeleted failure");
//       AudioProcessor::editorBeingDeleted (editor);
//   }
//   public 配置: setThrowOnCreateEditor(bool) / setThrowOnEditorBeingDeleted(bool) / getEditorBeingDeletedCount()
//   private: bool throwOnCreateEditor = false; bool throwOnEditorBeingDeleted = false; int editorBeingDeletedCount = 0;

// tests/EditorCrashGuardTests.cpp（新建）：
#include <catch2/catch_test_macros.hpp>
#include "TestPlugin.h"
#include "../source/host/EditorCrashGuard.h"

// CrashLog stub 辅助（CommandParserStubs.cpp 定义，此处 extern）
extern void clearCrashLog();
extern int crashLogErrorCount();
extern bool crashLogContains (const juce::String& substr);

TEST_CASE ("EditorCrashGuard: createEditor returns null when plugin has no editor",
           "[editorcrashguard]")
{
    TestPlugin plugin;
    clearCrashLog();
    auto* editor = EditorCrashGuard::createEditor (&plugin);
    REQUIRE (editor == nullptr);
}

TEST_CASE ("EditorCrashGuard: createEditor catches plugin exception and returns null",
           "[editorcrashguard][exception]")
{
    TestPlugin plugin;
    plugin.setThrowOnCreateEditor (true);
    clearCrashLog();
    auto* editor = EditorCrashGuard::createEditor (&plugin);
    REQUIRE (editor == nullptr);
    REQUIRE (crashLogErrorCount() >= 1);
}

TEST_CASE ("EditorCrashGuard: deleteEditor is a no-op for null editor",
           "[editorcrashguard]")
{
    TestPlugin plugin;
    clearCrashLog();
    EditorCrashGuard::deleteEditor (&plugin, nullptr);   // must not crash
}

TEST_CASE ("EditorCrashGuard: deleteEditor notifies processor before deleting",
           "[editorcrashguard]")
{
    TestPlugin plugin;
    auto* editor = new juce::AudioProcessorEditor (&plugin);   // minimal concrete editor
    EditorCrashGuard::deleteEditor (&plugin, editor);
    REQUIRE (plugin.getEditorBeingDeletedCount() == 1);
}

TEST_CASE ("EditorCrashGuard: deleteEditor catches editorBeingDeleted exception",
           "[editorcrashguard][exception]")
{
    TestPlugin plugin;
    plugin.setThrowOnEditorBeingDeleted (true);
    auto* editor = new juce::AudioProcessorEditor (&plugin);
    clearCrashLog();
    EditorCrashGuard::deleteEditor (&plugin, editor);   // must not crash
    REQUIRE (crashLogErrorCount() >= 1);
}
```

- [ ] **Step 2：跑测试确认 RED**（EditorCrashGuard 桩无行为 → 异常用例失败/无法断言）

- [ ] **Step 3（GREEN）：编译真实 EditorCrashGuard + 移除桩**

```cpp
// tests/CMakeLists.txt target_sources 加：
//     ../source/host/EditorCrashGuard.cpp
// 并加：
// set_source_files_properties(../source/host/EditorCrashGuard.cpp PROPERTIES
//     COMPILE_FLAGS "/EHa")
// CommandParserStubs.cpp 删除 EditorCrashGuard 桩两函数（namespace EditorCrashGuard 整块），保留 CrashLog 记录桩
```

- [ ] **Step 4：跑测试确认 GREEN**

- [ ] **Step 5：tests/AGENTS.md 更新**——"不编译进测试" 列表移除 EditorCrashGuard.cpp；覆盖映射补 editor crash guard → EditorCrashGuardTests

- [ ] **Step 6：全量 ctest 连跑 2 次 + 提交**

```bash
git add tests/CMakeLists.txt tests/CommandParserStubs.cpp tests/TestPlugin.h tests/EditorCrashGuardTests.cpp tests/AGENTS.md
git commit -m "test(host): compile real EditorCrashGuard into unit tests with exception coverage"
git push origin main
```

---

## 任务 3：Generic 编辑器兜底验证（验证型，无代码）

**Files:** 无（审查 + 文档）

- [ ] **Step 1：审查确认**——`source/Main.cpp:1614-1628`（openEditorWindowFor）已有 GenericAudioProcessorEditor 兜底 + try/catch（1dcb013 实现），无编辑器/创建失败 → generic → 仍失败则 "Loaded (no editor)" 状态。记录审查结论
- [ ] **Step 2：roadmap/STATUS 标记完成**（任务 3 已实现，无需改动）
- [ ] **Step 3：提交文档**

```bash
git add STATUS.md docs/roadmap-next.md
git commit -m "docs: mark generic editor fallback verified (implemented in 1dcb013)"
git push origin main
```

---

## 任务 4：观察者指针生命周期加固

**Files:**
- Modify: `source/ui/PluginEditorWindow.cpp`（closeButtonPressed 加固）
- Modify: `source/Main.cpp`（如审查发现其他风险）
- Test: `tests/ComponentVisibilityTests.cpp`（如可测）

**审查清单（先审查后动手）：**
1. `PluginEditorWindow::closeButtonPressed` → `onWindowClosed()` → Main 回调 → `unloadCurrentPlugin()` → `editorWindow.reset()` —— **成员函数中 delete this**（回调栈内销毁窗口对象）。加固：先 move 出回调再调用，杜绝回调后访问本对象
2. `MainContentComponent` ChangeListener（KnownPluginList）——构造 add / 析构 remove 已对称（248/326），确认析构顺序（remove 先于成员析构 ✔）
3. `CommandParser` 回调（load/status/measurementComplete/scanComplete）捕获 this —— 析构时 pipeServer shutdown 先 join，确认无窗口期
4. `MeasurementSession` blockCallback 捕获 this（appendLiveBlock）——cancel + unload 顺序已处理
5. 扫描/加载专用线程 alive 标志 —— 已实现（scanAlive/loadAlive + callAsync 串行化）

- [ ] **Step 1（GREEN 即现）：加固 closeButtonPressed**

```cpp
// PluginEditorWindow.cpp：
void PluginEditorWindow::closeButtonPressed()
{
    // Move the callback out first: Main's handler destroys this window
    // (unloadCurrentPlugin → editorWindow.reset()), so no member access is
    // allowed after the call — including this->onWindowClosed itself.
    auto cb = std::move (onWindowClosed);
    onWindowClosed = nullptr;
    if (cb)
        cb();
    // Do NOT call systemRequestedQuit — Main owns the unique_ptr and
    // resets it via onWindowClosed.
}
```

- [ ] **Step 2：审查其余清单项**，发现缺口即修（记录每项结论）
- [ ] **Step 3：真机验证**——IPC 加载插件 → 关闭窗口（窗口关闭经 IPC 不可达？GUI 关闭路径无法用 IPC 触发——**改用进程退出验证**：加载后直接退出应用无崩溃/无 UAF 日志；或 GUI 按钮路径以代码审查为准，标注验证边界）
- [ ] **Step 4：构建 + ctest + 提交**

```bash
git add source/ui/PluginEditorWindow.cpp
git commit -m "fix(ui): harden editor window close against self-destruction in callback"
git push origin main
```

---

## 任务 5：CGII.vst3 0 类型预防性黑名单

**Files:**
- Modify: `source/host/PluginManager.cpp`（新增 `blacklistUnregistered` + scanSystemDirectories 接线）
- Modify: `source/host/PluginManager.h`（公开声明 + 注释）
- Test: `tests/PluginManagerTests.cpp`
- Docs: `STATUS.md`（已知残留更新）

**Interfaces:**
- `int PluginManager::blacklistUnregistered (const juce::File& directory)`——枚举目录顶层 `*.vst3`（文件或 bundle 目录），对"文件存在 && knownPlugins 无匹配条目 && 未黑名单"者 addToBlacklist + saveCache + CRASH_LOG_WARN；返回新增黑名单数。匹配复用 cacheIsCurrent 的 samePath 语义（精确或 `bundle\Contents` 前缀）；黑名单判定用 getBlacklistedFiles()（normalized）
- scanSystemDirectories：两个扫描目录后各调一次（`scanDirectory(...)` 之后、`dedupeKnownPlugins(); saveCache();` 之前）

- [ ] **Step 1（RED）：PluginManagerTests 新用例**

```cpp
TEST_CASE ("PluginManager: blacklistUnregistered blacklists existing zero-type files",
           "[pluginmanager][zero-type]")
{
    // Arrange — temp dir with a fake .vst3 bundle directory (never scanned)
    auto dir = juce::File::getSpecialLocation (juce::File::SpecialLocationType::tempDirectory)
                   .getChildFile ("pluginlab_blacklist_test_" + juce::String (juce::Time::currentTimeMillis()));
    dir.createDirectory();
    auto fakePlugin = dir.getChildFile ("Zero.vst3");
    fakePlugin.createDirectory();
    auto cacheFile = dir.getChildFile ("cache.xml");

    PluginManager pm;
    pm.setCacheFile (cacheFile);

    // Act
    const int added = pm.blacklistUnregistered (dir);

    // Assert — the fake zero-type plugin was blacklisted and persisted
    REQUIRE (added == 1);
    REQUIRE (pm.isBlacklisted (fakePlugin.getFullPathName()));   // 需公开判定辅助或经快照

    // Persistence round-trip
    pm.saveCache();
    PluginManager pm2;
    pm2.setCacheFile (cacheFile);
    REQUIRE (pm2.loadCache());
    // blacklisted files survived the round-trip (KnownPluginList XML carries BLACKLISTED)

    // Cleanup
    dir.deleteRecursively();
}
```

- [ ] **Step 2：跑测试确认 RED**（blacklistUnregistered 不存在 → 编译失败）
- [ ] **Step 3（GREEN）：实现**

```cpp
// PluginManager.h public:
//     /** Blacklist existing plugin files that produced no types (zero-type
//      *  plugins like CGII.vst3 are never cached, so every hot start rescans
//      *  them). Enumerates `directory`'s top-level *.vst3 entries and
//      *  blacklists those neither known nor already blacklisted. Returns the
//      *  number of newly blacklisted entries. */
//     int blacklistUnregistered (const juce::File& directory);

// PluginManager.cpp:
int PluginManager::blacklistUnregistered (const juce::File& directory)
{
    int added = 0;
    if (! directory.isDirectory())
        return 0;

    juce::Array<juce::File> entries;
    directory.findChildFiles (entries, juce::File::findFilesAndDirectories, false, "*.vst3");

    const auto types = knownPlugins.getTypes();
    for (const auto& file : entries)
    {
        const auto key = file.getFullPathName();
        const auto contentsIdx = key.indexOf ("\\Contents");
        const auto bundleKey = contentsIdx > 0 ? key.substring (0, contentsIdx) : key;

        bool known = false;
        for (const auto& d : types)
        {
            if (d.fileOrIdentifier == bundleKey
                || d.fileOrIdentifier.startsWith (bundleKey + "\\Contents"))
            {
                known = true;
                break;
            }
        }
        if (known)
            continue;

        bool blacklisted = false;
        {
            std::lock_guard<std::mutex> lock (knownListGuard);
            const auto blacklist = knownPlugins.getBlacklistedFiles();
            blacklisted = blacklist.contains (bundleKey);
        }
        if (blacklisted)
            continue;

        addToBlacklistLocked (bundleKey);
        ++added;
        CRASH_LOG_WARN ("Zero-type plugin blacklisted", bundleKey);
    }

    if (added > 0)
        saveCache();          // 立即持久化——下次热启不再重扫

    return added;
}

// scanSystemDirectories：scanDirectory 两处调用之后加：
//     blacklistUnregistered (juce::File ("C:\\Program Files\\Common Files\\VST3"));
//     if (localDir.isDirectory()) blacklistUnregistered (localDir);
// （放在 dedupeKnownPlugins/saveCache 之前；saveCache 由 blacklistUnregistered 内部 + 末尾双保险）
```

- [ ] **Step 4：跑测试确认 GREEN**（可能需补 `isBlacklisted` 公开辅助或用 getScanStatusSnapshot().blacklisted 断言；以编译为准）
- [ ] **Step 5：全量 ctest + 真机验收**

真机（IPC 驱动，无鼠标）：
1. 启动应用 → `getScanStatus` 轮询至 done → 读取 `%APPDATA%/PluginLab/pluginlist.xml` 确认 CGII.vst3 在黑名单（`<BLACKLISTED>` 元素）
2. 日志含 "Zero-type plugin blacklisted ... CGII"
3. 重启应用 → 热启扫描不重扫 CGII（日志无 "Discovered" CGII；`getScanStatus.count` 与缓存一致）

- [ ] **Step 6：STATUS.md 已知残留更新 + 提交**

```bash
git add source/host/PluginManager.cpp source/host/PluginManager.h tests/PluginManagerTests.cpp STATUS.md
git commit -m "feat(host): preemptively blacklist zero-type plugins to skip rescans on hot start"
git push origin main
```

---

## 任务 6：data-schema.md scan 结构描述与实现最终核对

**Files:**
- Modify: `docs/data-schema.md`（如有差异）
- Test: `tests/ExportTests.cpp`（锁定缺失字段断言）

**核对清单（先逐项比对，记录结果）：**
1. §5 scan 顶层：`scan{param_id, param_name, values, param_texts}` + `family[{param_value_normalized, param_value_text, latency_samples, result}]` —— vs scanToJSON（已读源码：一致 ✔）
2. context 结构：plugin/class_id/latency_samples/sample_rate/measurement{}/parameter_snapshot/source{} —— vs appendContextFields（已读：一致 ✔；schema 示例含顶层 latency_samples + sample_rate，实现亦输出 ✔）
3. `scan.param_texts` 与 family[i] 对齐语义 —— 实现：values 与 family 各自独立循环，param_texts 用 `scan.family[i].paramValueText` 逐个转义（已读：一致 ✔）
4. family[].result 按类型 body：frequencyResponse → raw/smoothed_1_12/smoothed_1_3；harmonicAnalysis → tones；compressionCurve → curve/fitted（已读：一致 ✔）
5. dataset §8 内嵌 scan.family —— appendDatasetScanFamily 与 scanToJSON 布局一致（已读：一致 ✔）
6. 精度：values 6 位、param_value_normalized 6 位（fmtDouble(v,6) ✔）
7. 现有测试覆盖：ExportTests `[export][scan-schema]` 已锁 scan+family+context 关键字段 —— **需读该用例确认断言完整度**，缺什么补什么

- [ ] **Step 1：读 ExportTests scan-schema 用例**，比对核对清单 1-6，列出差异或确认一致
- [ ] **Step 2：如有差异**——先补测试（RED）再修文档/实现（GREEN）；如无差异——补 1-2 条锁定断言（如 param_texts 逐项对齐、values 精度 6 位）作为最终核对的测试证据
- [ ] **Step 3：data-schema.md 加"核对状态"注释**（2026-08-08 最终核对一致）
- [ ] **Step 4：ctest + 提交**

```bash
git add tests/ExportTests.cpp docs/data-schema.md
git commit -m "test(export): lock scan schema fields after final data-schema reconciliation"
git push origin main
```

---

## 任务 7：getParams 响应带 Band Used 状态验证

**Files:**
- Test: `tests/CommandParserTests.cpp`（如现状已满足则加锁定用例）
- Docs: `STATUS.md`（待改进项 3 解决记录）

**背景核实：**
- getParams 已返回全部参数（index/name/value/param_id），Pro-Q 4 真机快照（out/pro-q-4/dataset.json parameter_snapshot）含 `"Band 1 Used": 0.0` 等 605 键 → **Band Used 状态已在响应中**（name + value + param_id）
- 待改进项 3（STATUS.md:170）"Pro-Q 4 Band 1 Used 状态"：反推场景需确认 Band 1 Used=0（未激活）时 AI 可读该状态，避免把未激活 band 的反推结果当有效
- 待改进项 4 "getParams 不带 param_id" —— 已修（3332e3d 后 getParams 带 param_id，测试 `[getParams-param-id]` 锁定）

- [ ] **Step 1（RED）：锁定 Band Used 参数响应测试**

```cpp
// CommandParserTests.cpp 新增：
TEST_CASE ("CommandParser: getParams exposes band-used style parameters with value and id",
           "[commandparser][getParams][band-used]")
{
    // Arrange — TestPlugin with a Pro-Q-style "Band 1 Used" toggle parameter
    TestPlugin plugin;
    plugin.addTestParameter ("band1used", "Band 1 Used", 0.0f);
    plugin.addTestParameter ("band1gain", "Band 1 Gain", 0.5f);
    CommandParser parser;
    parser.setPluginInstance (&plugin);

    // Act
    auto response = parser.handleCommand (R"({"cmd":"getParams"})");

    // Assert — the used-state parameter is addressable by stable id with its value
    REQUIRE (response.contains (R"("name":"Band 1 Used")"));
    REQUIRE (response.contains (R"("param_id":"band1used")"));
    REQUIRE (response.contains (R"("name":"Band 1 Gain")"));
    REQUIRE (response.contains (R"("param_id":"band1gain")"));
}
```

- [ ] **Step 2：跑测试确认 RED**（addTestParameter 存在 ✔ 应直接失败于断言）
- [ ] **Step 3：确认实现已满足 → GREEN**（无生产代码改动，仅测试锁定；若意外 RED 则修 getParams 并记录）
- [ ] **Step 4：真机验证**——IPC 加载 Pro-Q 4 → getParams → 确认响应含 "Band 1 Used" 名称/值/param_id；`setParam` 用 param_id 切换 Band 1 Used 成功

```powershell
# 真机（tools/ipc_client.ps1 或 pipe_client.py）：
# {"cmd":"loadPlugin","name":"Pro-Q 4"} → ok
# {"cmd":"getParams"} → 响应含 "Band 1 Used" + param_id
# {"cmd":"setParam","param_id":"<Band1Used的id>","value":1} → ok
```

- [ ] **Step 5：STATUS.md 待改进项 3/4 标记解决 + 提交**

```bash
git add tests/CommandParserTests.cpp STATUS.md
git commit -m "test(ipc): lock band-used parameter exposure in getParams (issue 3 verified)"
git push origin main
```

---

## 收尾：全量回归 + 审查 + push

- [x] **Step 1：全量 ctest 连跑 2 次**（186/186 全绿，2026-08-08）
- [x] **Step 2：代码审查（requesting-code-review）**——双轴（Standards + Spec）并行子代理；Standards 2 硬（文档不同步）+ 2 smell、Spec 无硬缺失；修复见 commit 0a75786
- [x] **Step 3：真机冒烟**——热启无 CGII 重扫；measure 正常（任务 1 回归）
- [x] **Step 4：文档同步**（tests/AGENTS.md / source/capture/AGENTS.md / STATUS.md / roadmap 执行状态）
- [x] **Step 5：最终 push origin main**

## 执行状态（2026-08-08 完成）

> 7 任务全部交付：每任务 TDD（RED→GREEN）+ 真机验收（全 IPC 驱动，无鼠标）+ 原子提交 push；审查修复 0a75786。

| 任务 | Commit | 验收证据 |
|------|--------|----------|
| 1 测量路径异常保护 | ea1ebe2 | 5 测试（SweepRunner + CommandParser 级）+ 真机 Pro-Q 4 measure 回归 |
| 2 EditorCrashGuard 入测试 | 6a77fb5 | 5 测试含 SEH 崩溃保护 |
| 3 Generic 兜底验证 | 4ce050e | 1dcb013 实现审查 + 真机不误触发 |
| 4 观察者加固 | add971c | closeButtonPressed 加固 + 真机干净退出 |
| 5 CGII 黑名单 | 560dd86 | 4 测试 + 真机二次热启无重扫 |
| 6 schema 核对 | f44e7a0 | 30 断言锁定 |
| 7 getParams Band Used | 678495b | 1 测试 + 真机切换验证 |

## 风险

| # | 风险 | 等级 | 缓解 |
|---|------|------|------|
| R1 | SweepRunner 是冻结模块，改动违反边界 | P1 | roadmap 明示授权 /EHa 选项；只加保护不改测量语义；AGENTS.md 记录例外 |
| R2 | /EHa 与 /WX 冲突警告 | P2 | 现有 PluginManager/EditorCrashGuard 同配置零警告，先例可循 |
| R3 | 异常后插件状态不可信，后续测量连锁失败 | P2 | run() 返回 false → IPC 错误响应；插件实例仍可卸载重载；文档化 |
| R4 | blacklistUnregistered 误伤（插件路径不匹配规则） | P1 | samePath 语义复用 cacheIsCurrent 先例；真机验收 CGII 确认 |
| R5 | closeButtonPressed 加固遗漏其他 self-destroy 路径 | P2 | 审查清单逐项记录 |
