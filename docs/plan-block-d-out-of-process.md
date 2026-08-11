# PluginLab 块 4：D 进程外托管工作文档（设计门 → 实施中）

> 2026-08-08 制定（v2 细化版）。路线图总览见 `docs/roadmap-next.md` 块 4。执行顺序：E（块 1，**已完成**）→ B（块 3，**已完成**）→ D（块 4，**当前块，实施中**）。
> 背景：Pianoteq 类插件（宿主杀手）在加载/测量时调用 `ExitProcess`/abort，进程内任何保护（/EHa、catch(...)、看门狗）都无法拦截——唯一的根治是把 VST3 托管进**子进程**：插件崩溃/自杀只杀子进程，宿主存活并可重启续测。

## D0 设计门定案（2026-08-10 全部确认）

6 个设计问题经 brainstorming 逐条确认，**全部按推荐方案定案**，拆票实施开始：

| # | 问题 | 定案 |
|---|------|------|
| 1 | 子进程边界 | **B+**：load+measure 进子进程，扫描留宿主（死马踏板/看门狗/黑名单机制不动）；**仅黑名单插件走子进程**，白名单宿主直载零开销 |
| 2 | 编辑器跨进程托管 | **c**：子进程加载实例 + GenericAudioProcessorEditor，SetParent 显示到宿主（v1）；a（双实例原生编辑器）为 v2 备选 |
| 3 | 与既有机制关系 | 宿主**唯一黑名单写者**（子进程崩溃经 IPC 上报 → 宿主 addToBlacklistLocked + saveCache）；扫描留宿主踏板不动；新增子进程看门狗（心跳 3s + 退出码双条件）；宿主侧 /EHa 保留；崩溃日志宿主统一记录 |
| 4 | 崩溃恢复语义 | 心跳超时(3s)/退出码非 0 → 记录崩溃（黑名单联动 + CrashLog）→ 自动重启 → 参数快照恢复 → 续测当前命令；连续崩溃 ≥3 次（kMaxScanHangs 语义）停重启返回错误；测量中崩溃宿主存活 |
| 5 | IPC 协议 | **stdin/stdout JSON 行协议**，命令 start/load/measure/stop/heartbeat/result/crash，与宿主↔AI Named Pipe 协议同风格（cmd 判别 + {"ok":...} + snake_case + escapeJsonString） |
| 6 | 音频回传 | **子进程内录制 + 结果一次性回传**（复用 SweepRunner/CaptureBuffer；分析留宿主，结果 JSON 回传） |

**ADR-D-1（实施前修正）**：计划初稿的「从 captureParameterSnapshot 恢复参数」不可行——该 API 是 name 键控的导出用 JSON 快照（MeasurementSession.cpp:83-107），**无回灌路径**。D3 改为新增 **stable-id 键控全量参数快照/恢复 API**（复用块 B 的 R2 `timelineRestore` 模式：`ParameterTimeline::findParam` + `setValueNotifyingHost`，MeasurementSession.cpp:15-19,53-81），快照经 IPC 随 `result`/`crash` 消息在宿主↔子进程间传递。

**ADR-D-2（崩溃注入测试手段）**：`/EHa` 的 `catch(...)` 能拦截 SEH（空指针解引用不产生真实崩溃，SweepRunner.cpp:53-109 已包 try/catch）——D3 测试的「自杀插件」必须用 `ExitProcess`/`abort()`（穿透 C++ 异常机制的直接进程终止），且**必须运行在子进程内**（unit_tests 是单进程，进程内自杀会杀死整个测试）。方案：TestPlugin（或新 TestCrashPlugin.h）加 `setExitProcessOnProcessBlock`/`setAbortOnPrepareToPlay` 类 setter，经子进程加载执行；协调器测试 spawn 真子进程断言检测/重启/参数恢复/3 次上限。

**ADR-D-3（类名冲突，2026-08-10 D1c 实施中）**：协调器类名定为 **`PluginHostChildCoordinator`**（文件仍名 `ChildProcessCoordinator.h/.cpp`）——JUCE 9 自身声明 `juce::ChildProcessCoordinator`（modules/juce_events/interprocess/juce_ConnectedChildProcess.h:152），且生成的 JuceHeader.h 有 `using namespace juce;`，票据名 `ChildProcessCoordinator` 在任何 TU 中都歧义。子进程侧与宿主侧实现均已按此命名。

**ADR-D-4（子进程技术选型，2026-08-10 D1c 实施中）**：宿主侧 spawn 用**原生 Win32 `CreateProcess` + 匿名管道**，不用 `juce::ChildProcess`——JUCE 9 ChildProcess 头**无 stdin 写 API**（仅 start/isRunning/readProcessOutput/readAllProcessOutput/waitForProcessToFinish/getExitCode/kill），而协议需要向子进程 stdin 写 JSON 行。看门狗与 stdout 读取融合在**单一专用一次性 reader 线程**（`PeekNamedPipe` 非阻塞 → stop() 必可 join；弃置走 `unique_ptr<std::thread>::release()`，Main.cpp 模式）。崩溃上报**恰好一次**：`crashReported.exchange` + `stopRequested` 门控（故意 stop/析构 kill 不上报；看门狗与退出码竞态不双报）；心跳时钟自 spawn 起，任何收到行刷新活性。

**ADR-D-5（测量数据回传 = WAV 中转，2026-08-10 D2 定案）**：子进程 `measure` 录制 dry/wet 后**不回传原始音频数据**，而是复用 `CaptureBuffer::setFlushConfig`（AudioBuffer.h:70）的增量 WAV 崩溃镜像（[dry ch0..N-1, wet ch0..N-1] 交织 24-bit PCM）——`SweepRunner::run()` 末尾 `result.trim()`（SweepRunner.cpp:226）自动 finaliseWav 回填尺寸，WAV 完整可读。子进程只回传 `wav_path` + 元数据（samples/rate/name/class_id/latency_samples）。理由：① 设计 Q6 明示「崩溃安全由子进程内 flush 承担」；② IPC 只传路径，避免 ~4MB 原始音频跨进程；③ 复用现成机制，SweepRunner 冻结不动。宿主侧新增 WAV 读取 + 拆 dry/wet 的分析入口。

**ADR-D-6（宿主无 plugin 的 Context 构造，2026-08-10 D2 定案）**：`buildExportContext(plugin, ...)`（CommandParser.cpp:307-339）依赖宿主 plugin 实例（getName/fillInPluginDescription/getLatencySamples）——但 B+ 决策下黑名单插件宿主**不加载**，无 plugin。D2b 新增独立分析入口：子进程 `measure` 结果回传 `name`/`class_id`/`latency_samples`（子进程有实例，直接取），宿主用这些元数据构造 `Export::Context`（pluginName/classId/latencySamples 从回传 JSON 填，sampleRate/blockSize/excitation/source 从 measure 请求参数填）。宿主 plugin 直测路径（buildExportContext）**不变**——两路径并存，D6 路由分流。

**ADR-D-7（非 freq 黑名单插件安全降级，2026-08-10 D6 定案）**：ChildWavAnalyzer 仅支持 frequency_response（D2 票「harmonic/compression 留扩展」）。D6 路由下，黑名单插件的 harmonic/compression/gr_timeline 测量返回 `{"ok":false,"error":"child measurement not implemented for type '<t>'"}`——**绝不禁用黑名单隔离兜底回宿主直载**（黑名单即宿主杀手，兜底违背 B+ 初衷）。白名单插件无此限制（宿主直载全类型）。未来子进程扩展 harmonic/compression 后自然解锁。

## 设计问题 1：子进程边界——scan / load / measure 哪些进子进程？

**推荐：方案 B+（load + measure 进子进程，扫描留宿主）**

| 候选 | 内容 | 优点 | 缺点 | 决策 |
|------|------|------|------|------|
| A. 全量进子进程 | scan + load + measure 全在子进程 | 隔离最彻底 | 扫描（本就脆弱）与编辑器（GUI）都跨进程，复杂度最高；现有扫描看门狗/黑名单机制全部重写 | ✗ |
| **B+. load + measure 进子进程** | 扫描留宿主（现有机制不动）；子进程负责加载实例 + 测量；**仅在插件进黑名单时才走子进程**（白名单插件仍在宿主进程，零开销） | 最小侵入；Pianoteq 场景（黑名单内）正是"加载即自杀"，必须子进程；常规插件无性能损失 | 两条加载路径并存（宿主直载 + 子进程载） | ✅ **推荐** |
| C. 按需动态决定 | 运行时判断插件类型决定是否进子进程 | 灵活 | 判断依据不明确（黑名单已是最佳信号） | 并入 B+ |

**决策标准**：① 不重写既有扫描/黑名单/看门狗；② 常规插件路径零开销；③ 黑名单插件（已知宿主杀手）彻底隔离。→ B+ 全满足。

## 设计问题 2：编辑器跨进程托管（R3 公认难点）

**推荐：方案 c（编辑器降级 GenericAudioProcessorEditor，留宿主进程）**

| 候选 | 内容 | 优点 | 缺点 | 决策 |
|------|------|------|------|------|
| a. 编辑器留宿主 + 测量进子进程 | 宿主独立加载实例显示原生编辑器，子进程再加载实例测量 | 原生 UI 保留 | 双实例内存开销；参数同步复杂（两实例状态需双向镜像） | 备选（人工使用场景需要时） |
| b. 子进程窗口 SetParent 跨进程嵌入 | 子进程创建 HWND 嵌入宿主 | 原生 UI | 消息循环/焦点/DPI 脆弱，公认难点 | ✗ |
| **c. 子进程内 Generic 编辑器（宿主显示）** | 子进程加载实例 + GenericAudioProcessorEditor，窗口经 SetParent 显示在宿主 | 单实例；AI 驱动主路径不需要原生编辑器；Generic 编辑器无插件 DLL 自绘，跨进程嵌入安全 | 无插件原生 UI（人工微调体验降级） | ✅ **推荐** |
| d. 子进程独立顶层窗口 | 子进程自建窗口 | 简单 | 用户交互割裂 | ✗ |

**决策标准**：AI 反推主路径（加载→测量→导出）**不需要原生编辑器**——块 A 批量采集已证明。原生编辑器仅人工使用场景，可后置为 a（双实例）方案。→ c 为 v1，a 为 v2 可选。

## 设计问题 3：与既有机制的关系（黑名单 / 死马踏板 / 看门狗 / 缓存）

| 机制 | 现状 | 子进程化后 | 决策 |
|------|------|-----------|------|
| 黑名单（addToBlacklistLocked + saveCache） | 宿主进程内，扫描/加载超时触发 | 子进程崩溃 → 子进程通过 IPC 上报崩溃事件 → 宿主 addToBlacklistLocked + saveCache（**宿主是黑名单唯一写者**） | ✅ 宿主保持唯一写者 |
| 死马踏板（deadMansPedal） | JUCE scanner 内部（扫描留宿主 → **不动**） | 子进程不再参与扫描，踏板语义仅服务宿主扫描 | ✅ 扫描留宿主即零改动 |
| 看门狗（scan/load 超时） | 宿主消息线程 Timer | 新增**子进程看门狗**：心跳超时（如 3s 无心跳）+ 进程退出码非 0 → 判定崩溃 | ✅ 双条件（心跳 + 退出码） |
| 测量路径异常保护（块 C 任务 1） | 宿主内 /EHa + catch | 子进程内同样需要（子进程内 C++ 异常逃逸 → 子进程 abort → 宿主检测崩溃重启）——但宿主侧保护保留（白名单插件仍宿主直载） | ✅ 两边都保留 |
| CrashLog / minidump | 宿主 | 子进程崩溃日志经 IPC 上报宿主统一记录（崩溃现场：exit code + 最后心跳前日志） | ✅ 宿主统一日志 |

## 设计问题 4：进程崩溃恢复语义

**推荐**：
- 崩溃检测：心跳超时（3s）或进程退出码非 0（管道断开兜底）
- 恢复：宿主记录崩溃（黑名单联动 + CrashLog）→ **自动重启子进程** → 从最后一次 `captureParameterSnapshot` 恢复参数 → 续测当前命令（重新 load + measure）
- 崩溃上限：连续崩溃 ≥ 3 次（复用 `kMaxScanHangs=3` 语义）→ 停止自动重启，命令返回错误响应（防崩溃循环）
- 测量中的崩溃：本次测量返回 `{"ok":false,"error":"child process crashed (restarting)"}`，宿主继续存活

## 设计问题 5：IPC 协议扩展

**推荐：stdin/stdout JSON 行协议（子进程无 GUI 依赖，比 Named Pipe 更轻）**

- 方向：宿主 ↔ 子进程双向 JSON 行（与宿主 ↔ AI 的 Named Pipe 协议同风格）
- 命令：`start` / `load {path}` / `measure {type,...}` / `stop` / `heartbeat` / `result {json}` / `crash {exitCode, lastLog}`
- 批量采集（块 A）透明：宿主 CommandParser 不动，`measure` 内部路由——插件在黑名单 → 子进程；否则宿主直载（B+ 决策）

## 设计问题 6：音频路径（回传带宽）

**推荐：子进程内录制 + 结果一次性回传**

- 子进程内复用 SweepRunner/CaptureBuffer 完整录制 → 分析在子进程或宿主（分析器无插件依赖，**留宿主**，结果 JSON 回传）
- 理由：逐 block IPC 回传 ≈ 8MB/s 大流量；一次性回传毫秒级完成，且崩溃安全由子进程内 flush 承担（CaptureBuffer flush 已在子进程内可用）

---

## 候选架构（brainstorming 输入，v1 定案后冻结）

```
宿主 PluginLab（现有）
├── CommandParser（不动，前端协议不变；内部路由：黑名单→子进程）
├── ChildProcessCoordinator（新，source/host/）
│   ├── 子进程生命周期（start/stop/restart/心跳/崩溃检测）
│   ├── 崩溃恢复（黑名单联动 + 参数快照恢复 + 上限）
│   └── IPC 路由（stdin/stdout JSON 行）
└── 子进程 PluginHostChild（新可执行，source/child/）
    ├── 加载（/EHa 保护）+ 测量（SweepRunner/CaptureBuffer 复用或拷贝）
    ├── Generic 编辑器（SetParent 显示到宿主，v1 可选）
    ├── 结果 JSON 回传 + 心跳上报
    └── 崩溃日志上报（exitCode + 最后日志）
```

## 实施顺序（设计门已通过，拆票如下）

> 每票遵循项目铁律：TDD（RED→GREEN→SURFACE）+ 真机验收 + 双轴代码审查。新 .cpp 双登记（根 CMakeLists.txt + tests/CMakeLists.txt）。子进程目标照抄 `tools/CMakeLists.txt` 的 `juce_add_console_app(VST3Scanner)` 模板。

| 票 | 阶段 | 内容 | 依赖 | 验收 |
|----|------|------|------|------|
| **D1a** | 子进程骨架-目标 | `source/child/PluginHostChild` 可执行目标（juce_add_console_app，链接 audio_basics/processors/formats/core/events + JUCE_PLUGINHOST_VST3=1 + JUCE_MODAL_LOOPS_PERMITTED=1；加载插件 TU 开 /EHa）+ main() 启动参数解析 | 无 | CMake 构建出独立 exe |
| **D1b** | 子进程骨架-协议 | 子进程协议层：读 stdin JSON 行 → cmd 分发（start/load/measure/stop/heartbeat/result/crash）→ 写 stdout JSON 行；复用 makeResponse/makeProgress 风格（Protocol.h:79-96 为 inline 可复制）；`start` 返回 pid+version，`load {path}` 加载插件，`heartbeat` 应答 | D1a | 命令行喂 JSON 行 → 回 JSON 行（脚本可测） |
| **D1c** | 宿主-协调器 | `source/host/ChildProcessCoordinator`：CreateProcess 启动子进程 + stdin/stdout 管道读写 + 心跳看门狗（3s 超时）+ 进程退出码检测（GetExitCodeProcess）+ 回收（WAIT + 超时 TerminateProcess）；崩溃 → 回调宿主（黑名单联动：`addToBlacklistLocked(bundleKey)` + `saveCache()` + CRASH_LOG_WARN，PluginManager.h:178 唯一写入口） | D1b | 子进程可启动、崩溃可检测（退出码 + 心跳超时双路径） |
| **D1d** | 骨架测试 | ChildProcessCoordinatorTests.cpp：spawn 真子进程 → start/load → kill → 断言检测到崩溃 + 宿主存活；心跳超时路径（子进程挂起不答） | D1c | TDD：RED→GREEN；ctest 全绿 |
| **D2a** | 测量入子进程-子进程侧 | 子进程 `measure {type,...}` 命令：镜像 MeasurementSession.cpp:135-153 生成器选择（freq → SineSweep 5s 或 MLS Impulse；harmonic/compression 留扩展）；复用 SweepRunner/CaptureBuffer（prepare→generate→process→capture，SweepRunner.cpp:27-251 冻结不动）；`setFlushConfig(wav_path, interval)` WAV 崩溃镜像（ADR-D-5）；`result` 回传 samples/rate/export_path/wav_path/name/class_id/latency_samples（ADR-D-6）；新增目标源：SweepRunner.cpp/AudioBuffer.cpp/SignalGenerator.cpp/SineSweep.cpp/Impulse.cpp 编入 PluginHostChild | D1b | freq 测量子进程内完成，WAV + 结果 JSON 回传（脚本可测：喂 measure → 回 result + wav 文件生成） |
| **D2b** | 测量入子进程-宿主路由 | 宿主新增「无 plugin 分析入口」（ADR-D-6）：读子进程回传 WAV → 拆 dry/wet（[dry ch0..N-1, wet ch0..N-1] 布局）→ FreqResponse::analyze/analyzeMLS（latency 从回传取）→ Export::freqResponseToJSON（Context 从回传元数据 + measure 请求参数构造）；ChildProcessCoordinator 向子进程转发 load+measure、收集 result；`measure` 命令内部路由（黑名单 → 子进程；白名单宿主直载，D6 补全） | D2a | Pro-Q 4 经子进程测量与宿主直测一致（<0.5dB） |
| **D3a** | 恢复-参数快照 | 新增 stable-id 键控全量参数快照/恢复（复用 R2 模式，ADR-D-1）；快照随 IPC 传递 | D2b | 快照→子进程重启→恢复后参数与崩溃前一致 |
| **D3b** | 恢复-自动重启 | 崩溃 → 黑名单联动 + CrashLog → 自动重启子进程 → 恢复参数快照 → 续测当前命令（重新 load+measure）；连续崩溃 ≥3 次（kMaxScanHangs=3 语义）停重启 → `{"ok":false,"error":"child process crashed (restarting)"}` | D3a | TestPlugin 注入 ExitProcess → 宿主存活 + 自动重启 + 续测最终成功；崩溃 3 次后返回错误响应 |
| **D4** | Pianoteq 实战 | 真机验收（需用户环境有 Pianoteq） | D3b | **roadmap 验收：Pianoteq 可加载测量不杀宿主** |
| **D5** | 编辑器（c 方案） | 子进程 GenericAudioProcessorEditor 经 SetParent 显示到宿主（v1 可后置，AI 主路径不需要） | D4 | 子进程 Generic 编辑器经 SetParent 显示；无原生 UI 可接受 |
| **D6** | 双路径路由收尾 | 白名单插件宿主直载零回归测试；黑名单插件全走子进程 | D3b | 路由测试全绿，宿主直载路径无回归 |

### 子进程 IPC 协议契约（stdin/stdout JSON 行，与 Named Pipe 同风格）

**宿主 → 子进程**（`cmd` 判别）：

```
{"cmd":"start"}
{"cmd":"load","path":"C:\\VST3\\X.vst3","sample_rate":48000,"block_size":512}
{"cmd":"measure","type":"frequency_response","source":"signal","excitation":"sweep","sample_rate":48000,"block_size":512,"export_path":"C:\\tmp\\freq.json","wav_path":"C:\\tmp\\freq.wav"}   // D2：type 目前支持 frequency_response（harmonic/compression 留扩展），excitation sweep|mls 镜像宿主
{"cmd":"snapshot_params"}                                                // D3a：全量参数快照（稳定 id 键控，ADR-D-1）
{"cmd":"restore_params","params":[{"id":"crash_mode","value":0.5}]}      // D3a：按稳定 id 恢复参数（未知 id 跳过，值 0..1）
{"cmd":"stop"}
{"cmd":"heartbeat"}
```

**子进程 → 宿主**（`{"ok":...}` 响应 + 主动上报行）：

```
{"ok":true,"pid":1234,"version":1}                              // start 应答
{"ok":true,"name":"X"}                                          // load 应答
{"ok":true,"progress":0.10}                                     // 心跳/进度行（复用 makeProgress 形状）
{"ok":true,"samples":816000,"rate":48000.0,"export_path":"...","wav_path":"...","name":"X","class_id":"...","channels":2,"latency_samples":0}   // measure 完成（D2：WAV 中转见 ADR-D-5；元数据见 ADR-D-6；形状对齐 CommandParser.cpp:639-643）
{"ok":true,"params":[{"id":"crash_mode","value":0.0},...]}      // snapshot_params 应答（稳定 id + 归一化值 0..1，6 位小数；空 id 跳过）
{"ok":true}                                                     // restore_params 应答
{"ok":false,"error":"..."}                                      // 错误约定同现有词汇（含 "params required"/"no plugin loaded"/"unknown excitation"）
{"crash":true,"exit_code":-1,"last_log":"..."}                  // 崩溃上报（宿主动态），失败兜底：进程退出码非 0 + 心跳超时
```

约定：每行 JSON + `\n`；字符串经 escapeJsonString（Windows 路径反斜杠必转义）；错误词汇沿用 CommandParser.cpp 现有集合（`path required`/`unknown measure type`/`unknown excitation`/`measurement failed` 等）。`channels` = 插件输入声道数 N（`pluginInstance->getTotalNumInputChannels()`，为 0 时回退 dry buffer 声道数）——宿主 WavCaptureReader 拆 dry/wet 需要 N（ADR-D-5 布局 `[dry ch0..N-1, wet ch0..N-1]`），黑名单路由下宿主无 plugin 实例，只能从 result 取。

## 风险（roadmap R3 细化）

| # | 风险 | 等级 | 缓解 |
|---|------|------|------|
| R1 | 跨进程编辑器是公认难点，可能长期卡住 | P0 | 设计门定案 c（Generic 编辑器留子进程 + SetParent）；AI 主路径不需要原生编辑器；a（双实例）为 v2 |
| R2 | 子进程音频回传带宽/IPC 复杂度 | P1 | 子进程内录制 + 结果一次性回传（设计问题 6 定案） |
| R3 | 双实例参数同步（若编辑器走 a 方案） | P1 | v1 走 c 无双实例；a 为 v2 时参数快照同步 |
| R4 | 崩溃恢复循环 | P2 | 上限机制复用扫描挂起语义（kMaxScanHangs=3） |
| R5 | 与黑名单/死马踏板/看门狗语义纠缠 | P1 | 设计问题 3 逐条定案（宿主保持黑名单唯一写者；扫描留宿主踏板不动） |
| R6 | 子进程 Generic 编辑器 SetParent 的焦点/DPI 问题 | P2 | v1 可先无编辑器（AI 驱动不需要）；SetParent 作为 D5 独立验收项 |
