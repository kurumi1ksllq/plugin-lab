# PluginLab 扫描优化开发计划（卡死 + 慢）

> 2026-08-03 制定。来源：explore 团队 3 路源码级调查（持久化 API / 增量 UI / 消息线程）+ plan 团队 3 路评审（架构 / 完整性 / 可行性）+ lead 源码定案分歧。
> 范围：用户要求的 5 项优化 + 评审发现的 P0 关窗死锁。覆盖扫描（`PluginManager`/`ScanJob`）与加载（`LoadJob`）两条路径。
> 确认后按步骤 0→5 顺序执行，每步独立可验证（TDD + 构建 + ctest）。

> **执行状态（2026-08-04 更新）**：
>
> - 步骤 0 已完成并提交（e2d45c0，关窗死锁修复；WM_CLOSE 消息验收通过：扫描中关窗 10s 内退出）。
> - 步骤 1 代码+测试完成（135/135 绿；冷扫缓存写入成功），热启动 31.2s 未达目标——根因由步骤 6 调查定案并修复。
> - **步骤 6（启动性能专项）已完成**（8f68234）：explore 团队 3 路调查 + lead 运行时证据定位根因（目录型 bundle 缓存路径不匹配 → 12 插件每轮重扫 + 单插件重扫 CPU 多核满载）。修复：`cacheIsCurrent()` 增量跳过（内层 DLL mtime 基准）+ `dedupeKnownPlugins()` + Pianoteq 扫描阶段文件名拦截 + 扫描/加载线程降优先级。**实测热扫 31.2s → 0.5s，CPU 峰值 521% → 0-3%，0 重扫**。
> - **步骤 2-5 已完成**（fa74d45 / 4fb74b1 / cb83c79 / c7f7f9f）：增量 UI 刷新（ChangeListener）→ 加载超时+预防性黑名单（30s WaitableEvent）→ 扫描看门狗（60s 无进展 → 黑名单+abandon，上限 3 次）+ Clear BL 按钮 → IPC `getScanStatus` 快照。**155/155 测试绿**。
> - **约束：全程禁止鼠标/GUI 人工操作**（见「十、无 GUI 鼠标约束」）——本计划全部手动验收均按此执行。

---

## 一、问题定义

- **慢**：无磁盘缓存，每次启动 `knownPlugins.clear()` 后全量重扫 108 个 VST3（串行 LoadLibrary + InitDll + 工厂枚举），含 iLok/PACE 授权插件（单个 5-30s），总耗时分钟级。
- **卡死**：插件 DLL 在 `DllMain`/`InitDll`/`GetPluginFactory` 内挂起（授权网络超时、坏 DLL），`try/catch` 与 /EHa **无法捕获挂起** → 扫描线程永久阻塞 → `scanRunning` 永远 true → UI 锁定在 "Scanning VST3 plugins..."、插件列表空、应用功能死亡。108 插件命中 ≥1 挂起是大概率事件（"有概率"）。
- **关窗死锁（评审发现，P0 现有 bug）**：`Main.cpp:308-310` 析构 `threadPool=nullptr` join 所有 job；挂起 ScanJob/LoadJob 永不结束 → 析构永久阻塞 → 进程不退。
- **启动性能（2026-08-03 用户报告，高优先级）**：应用启动即自动扫描期间，整机严重卡顿——高配电脑（运行大型软件无此现象）仍卡。步骤 1 实测：冷扫（全量 108 插件）耗时分钟级，**热扫（缓存命中）实测 31.2s，未达 ≤5s 目标**——增量跳过未完全生效。疑似根因（**待 explore 调查团队 3 路确认，见步骤 6**）：
  1. 插件 DLL 在扫描初始化（`GetPluginFactory`/授权校验）阶段 busy-wait 或高 CPU 旋转，占满 CPU 核心（iLok/PACE 网络等待若为自旋而非阻塞，症状吻合）；
  2. 增量跳过失效——`pluginNeedsRescanning` 对部分插件恒返回 true 导致重扫（热扫 31.2s 的最直接解释）；
  3. UI 侧 `paintListBoxItem` 每次绘制全量 `getTypes()` 拷贝（108 个 `PluginDescription` 的 String 拷贝）+ 扫描期间高频 repaint，造成内存分配抖动与卡顿。

## 二、调查结论（源码级，已核实）

| #   | 结论                                                                                                                                                                                                                                                                | 位置                                                                                  |
| --- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------- |
| 1   | `KnownPluginList::createXml()/recreateFromXml()` 支持跨重启增量；`fileTime` 属性保存 lastFileModTime(hex)                                                                                                                                                           | `juce_KnownPluginList.h:171-174`                                                      |
| 2   | `scanNextFile(true)` 对 mtime 未变文件经 `isListingUpToDate` 跳过（**零 DLL 加载**）；路径字符串精确匹配                                                                                                                                                            | `juce_PluginDirectoryScanner.cpp:92-126`、`juce_VST3PluginFormatHeadless.cpp:128-131` |
| 3   | JUCE 9 `scanNextFile` 第一参数是 `dontRescanIfAlreadyInList`（旧版是 `dontThrottle`）——项目基于旧 API 写死 `true`                                                                                                                                                   | `juce_PluginDirectoryScanner.h:102-103`                                               |
| 4   | `KnownPluginList : ChangeBroadcaster`，每新增插件发一次 change；`sendChangeMessage` 经内部 AsyncUpdater **自动投递消息线程 + 合并**                                                                                                                                 | `juce_KnownPluginList.h:49`、`juce_ChangeBroadcaster.h`                               |
| 5   | 挂起线程**不可终止**（进程内无 kill API），只能丢弃线程 + `addToBlacklist` + 重建 scanner；`scanAndAddFile` 对黑名单文件开头 `blacklist.contains` 拦截（:185）                                                                                                      | `juce_KnownPluginList.cpp:156-217`                                                    |
| 6   | **死马踏板对挂起天然生效**：scanNextFile 扫描前写踏板文件、成功后移除；挂起=永不移除→下次构造自动黑名单+移队尾先扫。当前 `PluginManager.cpp:20` 传 `File()` **整套禁用**                                                                                            | `juce_PluginDirectoryScanner.cpp:107-117,140-146`                                     |
| 7   | **加载线程真相**：同步 `createInstanceFromDescription` 从后台线程调用 → 非消息线程分支 → `createPluginInstanceAsync`（postMessage）→ **创建在消息线程执行** + 后台线程 `wait()`。VST3 加载构造必然在消息线程（JUCE 硬要求）→ 挂起=GUI 冻结=进程内不可恢复，只能预防 | `juce_AudioPluginFormat.cpp:71-76,93-105`                                             |
| 8   | **扫描线程真相**：`findDescriptionsSlow` 在扫描线程直调 `component->initialize()`（不跳消息线程）→ 扫描挂起=扫描线程卡死、GUI 仍响应 → **看门狗可恢复**                                                                                                             | `juce_VST3PluginFormatImpl.h:965-1059`                                                |
| 9   | 进度：`getProgress()`(0..1) + `getNextPluginFileThatWillBeScanned()`；官方 UI=消息线程 20ms Timer 轮询                                                                                                                                                              | `juce_PluginDirectoryScanner.h:110-119`、`juce_PluginListComponent.cpp:386-405`       |
| 10  | `AudioPluginFormatManager() = default` 无 pedal；`addDefaultFormats()` 已 `= delete` 需用 `addHeadlessDefaultFormatsToManager()`                                                                                                                                    | `juce_AudioPluginFormatManager.h:50`                                                  |

## 三、评审结论（plan 团队三批评家，要点）

- **架构**：模块边界放行（扫描状态机留在 host/PluginManager，Main 保持薄壳）；**弃用 `createPluginInstanceAsync`**（会把加载弹到消息线程=UI 冻结、消息线程 wait=必死锁）；死马踏板接线即白赚挂起恢复；ChangeBroadcaster 增量无需手写跳转；**线程隔离**：扫描/加载各专用单槽，不与 ThreadPool(2) 共享；子进程扫描 + CustomScanner 明确排除。
- **完整性**：P0 关窗死锁；缓存需**原子写**（temp+rename）+ version 字段 + 扫描目录列表缓存 + 剪枝（exists 过滤幽灵条目）；挂起泄漏需**上限**（≤3 次放弃剩余）；`scanRunning` 归属权（挂起 job 无法复位，需看门狗复位）；黑名单需**持久化**且与缓存**同文件合并设计**；loadPlugin 超时救不了消息线程（需代次计数器忽略迟到回调）；measure 期间 load 排队（伪超时需处理）；`makeProgress` 全库无调用者、PipeServer 同步 read→write → 方案 5 是**新增推送能力** + `getScanStatus` 快照命令（纯推送漏中途连接）。
- **可行性**：顺序 1→4→3→2→5 成立（4 先建黑名单基础设施给 2 用；3 的 change 事件流给 2 提供"无进展"信号）；丢弃线程=设计内泄漏须封顶；`/WX` 门禁 + 双编译提醒；JUCE 拉 master 未钉 tag 另立任务；子进程扫描写决策记录排除。

## 四、关键设计决策（定稿）

| 决策点   | 结论                                                                                                                                 |
| -------- | ------------------------------------------------------------------------------------------------------------------------------------ |
| 加载线程 | VST3 构造必在消息线程（JUCE 硬要求）。挂起=消息线程冻结，进程内不可恢复 → 预防优先（黑名单 + 死马踏板），加载侧超时仅让等待线程脱出  |
| 扫描线程 | 扫描挂起在扫描线程 → 看门狗可恢复（丢弃线程 + 黑名单 + 重建 scanner）                                                                |
| 死马踏板 | 接线真实路径，挂起/崩溃插件下次自动黑名单，免手写黑名单持久化                                                                        |
| 缓存     | `createXml/recreateFromXml` + 根 `version` 属性 + 原子写 + 剪枝 + 黑名单随缓存往返                                                   |
| 增量刷新 | `addChangeListener` 原生回调（JUCE 自动消息线程+合并），不手写 AsyncUpdater 转发                                                     |
| 加载超时 | 保持同步 `createPluginInstance` + 专用线程 + `WaitableEvent(Seconds(30))` + 放弃 + 黑名单 + 代次计数器                               |
| 线程模型 | 扫描/加载各专用一次性线程（`unique_ptr<std::thread>` 显式放弃），**不 join 挂起线程**（关窗退出关键）；generation token 防旧代写覆盖 |
| 看门狗   | `getProgress()` 无进展超时（可配，默认 60s）+ 挂起上限 3 次 + 置 `scanRunning=false` + generation token                              |

## 五、实施步骤（每步独立可验证）

### 步骤 0 — P0：关窗 + 挂起 = 进程不退死锁修复

- **根因**：`Main.cpp:308-310` 析构 join 所有 job；挂起 job 永不结束。
- **改动**：`ScanJob`/`LoadJob` 从 `ThreadPool(2)` 改为专用一次性线程（`unique_ptr<std::thread>`，析构显式放弃不 join）+ generation token。ThreadPool 不再承担扫描/加载。
- **验收**：挂起扫描中关窗 N 秒内进程退出（当前必死锁）。

### 步骤 1 — P0：XML 缓存 + 死马踏板接线（根治「慢」）

- **改动** `source/host/PluginManager.cpp/.h`：
  - `loadCache()`：`XmlDocument::parse` → 校验根 `version` 属性 → `recreateFromXml` → 剪枝（`exists()` 过滤幽灵条目 + Pianoteq 黑名单过滤）
  - `saveCache()`：`createXml` → 根加 `version` 属性 → 原子写（temp + rename）
  - `scanSystemDirectories` 重写：**去掉 `knownPlugins.clear()`**（:35，否则增量被抹掉）→ 先 loadCache → 增量扫描（`scanNextFile(true)` 对 mtime 未变文件零 DLL 加载）→ saveCache
  - `PluginDirectoryScanner` 构造第 5 参改传真实踏板路径（`%APPDATA%/PluginLab/` 下）
- **验收**：热启动 ≤5s（当前分钟级）；触摸 1 个插件文件→仅它重扫；损坏缓存→全量重扫不崩溃；删插件文件→无幽灵条目。

### 步骤 2 — P1：增量 UI 刷新（方案 3）

- **改动** `source/Main.cpp`：`pluginListBox` 监听 KnownPluginList `addChangeListener` → 回调内 `updateContent()`（JUCE 自动消息线程+合并，≤50ms 天然节流）；**退订必须在析构 join 前**；扫描中即可加载已发现插件（去掉 scanDone 门控，`getPluginDescription` 已有 listLock 保护）。
- **验收**：插件逐条出现（非一次性）；关窗即停；扫描中可点已发现插件加载。

### 步骤 3 — P1：加载超时 + 预防性黑名单（方案 4）

- **改动** `source/host/PluginManager.cpp`：`loadPlugin` 改专用一次性线程 + `WaitableEvent(Seconds(30))`；超时→线程脱出 + `addToBlacklist` + 返回 `{ok:false,"error":"load timeout"}`；**代次计数器**忽略迟到回调；文档化限制（真挂起=消息线程冻结只能杀进程，靠死马踏板+黑名单下次跳过）。
- **验收**：挂起 load→30s→错误返回→后续操作可用；迟到回调被忽略；正常加载无回归。

### 步骤 4 — P1：扫描看门狗（方案 2）

- **改动** `PluginManager` + `Main.cpp`：看门狗（`getProgress()` 无进展超时 60s）→ 超时：generation token 使旧线程写无效 + `addToBlacklist` + **挂起上限 3 次后放弃剩余**（每次挂起泄漏一线程+锁一 DLL，必须封顶）+ 置 `scanRunning=false`；扫描阶段拦 Pianoteq（文件名匹配，此刻无 desc.name）；UI 提供清除黑名单并全量重扫入口。
- **验收**：挂起插件跳过、其余全部扫出；挂起后 `scanRunning=false`、UI 可操作、可 rescan；Pianoteq 不进列表；重启不重挂同一插件。

### 步骤 5 — P2：扫描进度 + IPC（方案 5）

- **改动** `PluginManager`（进度回调）+ `Main.cpp`（20ms Timer 轮询 `getProgress()` + 当前插件名）+ `source/ipc/`：新增 `getScanStatus` 快照命令（`{ok, running, done, count, currentFile, progress, blacklisted}`），快照+推送双轨（纯推送漏中途连接）；命名避开参数扫描 `scan` 语义；推送 ≤20ms/次防阻塞 pipe 线程；遵循 `source/ipc/AGENTS.md` 四件套规则（Protocol.h 常量 + CommandParser case + Stubs/命令测试 + 文档）。
- **验收**：三态（pre/mid/post）正确；progress 单调 0→1；currentFile 准确；中途连接拿得到快照。

### 步骤 6 — P1：启动性能专项（2026-08-03 用户报告新增）

- **前置流程（强制）**：**先派 explore 调查团队（3 路并行）调研根因，拿到调查产出后才允许制定修复方案与实施**。禁止无调查结论直接改代码（根因未明前的改动=盲改）。
- **调查路线 A — 扫描线程 CPU**：定位扫描期间占 CPU 的线程/插件/阶段；检测 busy-wait（授权网络等待是自旋还是阻塞）；产出可量化的采样证据（CPU 占用曲线、占用插件名单）。
- **调查路线 B — 增量失效**：热启动 31.2s 的构成（本次实际重扫了多少插件？哪些？`pluginNeedsRescanning` 对它们为何返回 true？mtime 比较失效的具体机制）；`scanNextFile` 遍历本身的开销。
- **调查路线 C — UI/内存**：`paintListBoxItem` 全量 `getTypes()` 拷贝的频率与触发源；扫描期间 repaint 风暴；`KnownPluginList::addType` change 消息连锁；内存分配抖动。
- **修复方向（待调查确认后定稿）**：CPU 旋转插件预防性黑名单/跳过；增量跳过修复；UI 侧按需取行（`getTypes()` 拷贝改为索引访问或缓存）。
- **约束**：全程无 GUI 鼠标操作（见「十、无 GUI 鼠标约束」）；验收不抢鼠标。
- **验收**：热启动 ≤5s；启动扫描期间整机无感知卡顿（CPU/内存采样可量化对比修复前后）；全程无鼠标人工操作。

## 六、测试策略

- **Catch2 可测**（无需真 DLL，`PluginManager.cpp` 已在 `unit_tests` 编译）：缓存往返/损坏 XML/version 不匹配/原子写/剪枝（临时目录）；黑名单过滤纯函数（Pianoteq 变体/大小写）；看门狗注入时钟模拟挂起；超时代次/迟到回调；getScanStatus 响应形状（CommandParserStubs）。
- **手动验收**（无法自动化）：真机 108 插件冷/热启动耗时对比；真挂起模拟（隔离进程）；关窗+挂起退出；loadPlugin 超时后 GUI 响应。

## 七、文档同步清单（每步随改）

- `STATUS.md`：决策记录（缓存格式 / 看门狗阈值 / 黑名单语义 / 子进程扫描排除理由）
- `source/ipc/AGENTS.md`：协议表 + ADDING A COMMAND 四件套（getScanStatus）
- `docs/data-schema.md`：变更记录提一句（scan_progress 属协议非导出 schema，不必加 §9）
- `DESIGN.md`：扫描架构节（方案 1-5 改变扫描流程 + 关窗语义）
- 根 `AGENTS.md`：WHERE-TO-LOOK（IPC 行）+ NOTES（缓存路径 `%APPDATA%/PluginLab/`、黑名单持久化位置）

## 八、明确排除（防过度设计）

- **子进程扫描**（AudioPluginHost 根治方案）——ExitProcess 型杀进程插件的唯一根治，工程量/架构影响大，本轮用「黑名单 + 踏板 + 看门狗」覆盖，延期理由写入 STATUS.md
- **`KnownPluginList::CustomScanner`**——接口面大，与 /EHa + 踏板机制职责重叠
- **JUCE 版本钉 tag**（现拉 master）——另立任务

## 九、风险清单

| 风险                                | 等级 | 缓解                                                        |
| ----------------------------------- | ---- | ----------------------------------------------------------- |
| R1 消息线程 wait 回调 = 必死锁      | P0   | 禁止在消息线程对加载/扫描结果 `WaitableEvent.wait()`        |
| R2 加载挂起 = 消息线程冻结          | P0   | 预防优先（黑名单+死马踏板），超时仅脱出等待线程，文档化限制 |
| R3 看门狗丢弃线程永久减容           | P1   | 专用线程隔离 + 挂起上限 3 次 + 文档化泄漏                   |
| R4 黑名单永久卡住已修复插件         | P1   | UI 清除黑名单并全量重扫入口                                 |
| R5 缓存损坏/旧版                    | P1   | 原子写 + version 属性 + parse 失败回退全量                  |
| R6 增量被 clear 破坏                | P1   | 去掉 `knownPlugins.clear()`（步骤 1 强制）                  |
| R7 一次性挂起即入黑名单             | P1   | 同 R4 清除入口                                              |
| R8 measure 期间 load 排队（伪超时） | P2   | 禁止 load/measure 并发或文档化                              |

## 十、无 GUI 鼠标约束（用户要求，2026-08-03 生效）

- **所有手动验收禁止使用鼠标/键盘操作 GUI**——用户需同时使用电脑，代理不得抢占鼠标/键盘。
- 验收一律**脚本驱动**：WM_CLOSE 窗口消息（步骤 0 关窗验收已用，`PostMessage` 无鼠标）、进程/文件监控（缓存 mtime、crash log）、Named Pipe IPC 命令、性能计数器采样（CPU/内存）。
- 已按此约束完成的验收：步骤 0 关窗（WM_CLOSE 消息）；步骤 1 冷/热启动计时（缓存 mtime 轮询）。
- 步骤 6 的 CPU 占用采样同样禁止 GUI 工具，用性能计数器/脚本采集。
