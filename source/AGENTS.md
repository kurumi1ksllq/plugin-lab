# source/ — PluginLab 生产代码层

## OVERVIEW

8 个子模块 + Main.cpp 共 58 文件（29 .cpp + 29 .h）。模块边界由根 CMakeLists.txt 单一 target_sources 清单强制，目录不自包含。全局风格/构建命令/通用反模式见根 `AGENTS.md`，此处只写 source 层独有的结构、接线与边界。

## STRUCTURE

```
source/
├── Main.cpp          # 入口 + 装配中枢（1829 行 god file）
├── host/             # VST3 扫描/加载/崩溃保护（唯一 /EHa TU 所在）
├── signal/           # SignalGenerator 接口 + 7 生成器 —— 见 signal/AGENTS.md
├── capture/          # 测量编排（SweepRunner 冻结管线 + MeasurementSession）—— 见 capture/AGENTS.md
├── scan/             # ScanEngine 参数扫描（快照/恢复/取消 RAII）
├── analysis/         # 6 分析器 + Export（手写 JSON）—— 见 analysis/AGENTS.md
├── ipc/              # Named Pipe 服务器 + CommandParser 分发 —— 见 ipc/AGENTS.md
├── ui/               # PlotWidget + PluginEditorWindow
└── utils/            # FftHelper / MathUtils / CrashLog
```

## WIRING (Main.cpp)

`PluginLabApplication`（START_JUCE_APPLICATION）→ `PluginLabWindow` → `MainContentComponent` 构造时接线：

- PluginManager、MeasurementSession（48kHz/512 + block callback）、CommandParser（4 回调：load/status/measurementComplete/scanComplete）、PipeServer::startup、3× PlotWidget（mag/phase/GR）、扫描/加载专用一次性 `std::thread`（scanThread/loadThread，`unique_ptr` 显式放弃）、Windows minidump + CrashLog
- 构造即 auto-run `scanPlugins()`；GUI 按钮同步调 `commandParser->handleCommand(json)`（消息线程）

## MODULE BOUNDARIES

依赖单向：`Main.cpp → host / ipc / capture / scan / ui`；`capture → signal / analysis / utils`；`ipc → capture / scan / analysis / host`

测量目的分层（DESIGN.md §8.2）：

- **SweepRunner**：不感知"测量目的"，只做 generate→process→capture I/O → **冻结不改**
- **MeasurementSession**：感知"测什么"（type + sourceType）→ 扩展点
- **上层（RecorderEngine 概念）**：感知"为什么测"→ 尚未实现，未来扩展放上层，不要下探

## SOURCE-UNIQUE CONVENTIONS

（根级通用约定：双编译、include 相对路径、无异常、线程铁律——见根 AGENTS.md，此处只列 source 特有）

- `/EHa` 只用于 `host/` 两个 TU（PluginManager + EditorCrashGuard）——根 CMake 用 `set_source_files_properties` 逐文件指定，勿扩散
- 所有类 `JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR`；成员类内初始化默认值（sampleRate=48000.0, blockSize=512）
- `Main.cpp:455/470` 是全库仅有的两个无 CRASH_LOG catch（ListBox 回调，antipattern，勿复制）

## SOURCE-UNIQUE ANTI-PATTERNS

（空 catch / 递归锁 / JUCE_TRY / 每 block 刷 UI / 改 SweepRunner 已在根文档，不重复）

- **prepareToPlay 在 loadPlugin 线程调用**——必须统一在 SweepRunner::run 测量线程（Phase-1 双调用致 Pro-Q 4 崩溃）
- **`getTotalLength()` 返回 -1**——无限长触发 SweepRunner 静默 10s 兜底（在 signal 层铁律，此处重申模块边界）
