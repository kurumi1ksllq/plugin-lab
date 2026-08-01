# Plugin Lab — 当前状态 (2026-08-01)

## 已验证通过 ✅

- **全量验收（真实 GUI 模拟测试，92 插件）**：
  - 91/92 插件覆盖测试，**90 成功（98.9%）**
  - 0 崩溃、0 无编辑器
  - 唯一失败：Pianoteq 9（黑名单拦截，见下）
- **独立插件窗口**：点击列表 → 弹出独立顶层窗口承载插件编辑器
- **窗口大小 = 插件大小**：客户区尺寸 == 编辑器尺寸（4 插件验证通过）
  - Auto-Tune Pro: 编辑器 1617x965 == 客户区 1617x965 ✅
  - AutoPitchK: 500x400 ✅ / Melodyne: 812x600 ✅ / PluginDoctor: 1000x750 ✅
  - （窗口总尺寸比编辑器大一个原生标题栏 ≈16x39，属正常）
- **窗口固定不可调整**（setResizable(false)）
- **单实例切换**：点新插件 → 旧窗口关、旧实例卸载 → 新窗口开，始终 ≤1 窗口
- **关窗即卸载**：X → 编辑器销毁 + 实例释放，主窗口不受影响
- **UI 全程响应**：扫描/加载后台线程，无卡死
- 构建通过：/W4 /WX 零错误

## 关键修复（真实测试发现）

1. **递归锁 bug（T1 前）：`loadPlugin()` 持有 listLock 又调 `getPluginDescription()` 内部再锁同一 mutex → std::system_error → 列表点击必失败**。修复：去掉外层锁。
2. **Pianoteq 9 崩溃（oracle 分析确认）**：插件在 createPluginInstance 内部调用 ExitProcess/TerminateProcess，绕过所有 try/catch + SEH + minidump，宿主进程无征兆退出（无 dmp、无 crash 事件，仅 RADAR_PRE_LEAK_64 副作用）。
   - **对策**：`PluginManager::loadPlugin` 加黑名单拦截（Pianoteq 7/8/9 匹配），返回 nullptr + 警告日志，宿主不再被杀
   - 长期方案：进程外托管（ChildProcessCoordinator，成本高，未实施）
3. **use-after-free 崩溃（Debug 构建 + 点击触发 0xc0000005，minidump 定位 atomic::operator++）**：`scanPlugins()`/`loadPluginByDescription()` 用 `std::thread([this]).detach()` + `callAsync([this])`，组件析构（关主窗口）时后台线程仍访问已析构的 this → 原子引用计数自增崩溃。
   - **对策**：改为 JUCE 标准 **ThreadPool + AsyncUpdater**（`threadPool->addJob(ThreadPoolJob)` + `triggerAsyncUpdate()`/`handleAsyncUpdate()`；析构时 `threadPool = nullptr` join 所有任务 + `cancelPendingUpdate()`）。
   - 已验证：快速点击 + 加载中关主窗口 3 次试验全部干净退出（修复前必崩）；全量 77 插件 76 成功（98.7%）0 崩溃。
4. **CLion 配置**：terminal-local.xml shellPath 多余引号致 MCP 终端报错——已修复。
5. **Debug 构建点击加载崩溃（0xc0000005，atomic++ use-after-free）**：`getPluginDescription` 返回 KnownPluginList 内部指针，锁释放后指针悬垂 → `descCopy = *desc` 拷贝时引用计数自增崩溃（Debug 暴露，Release 侥幸）。
   - **对策**：改为锁内返回 `PluginDescription` 拷贝（`bool getPluginDescription(int, PluginDescription&)`）。
   - 另加 `JUCE_DISABLE_ASSERTIONS=1`（Debug）：JUCE 9 VST3 headless 扫描器对商业插件触发消息线程断言（Debug-only），关闭后 Debug 扫描正常。
   - 已验证（Debug 构建）：UIA 加载 Auto-Key 2 / Auto-Tune Pro / AutoPitchK / Avalon / Melodyne 全部 Editor ok，无崩溃；快速点击+关主窗口干净退出。
6. **CLion 使用**：CLion 默认 Debug 构建——修复后 Debug 可正常扫描+加载插件（此前点击必崩）。如需 Release，在 CLion Settings → Build → CMake 添加 Release profile。

## 构建 & 运行

```
build:  cmake --build build --config Release
exe:    build\PluginLab_artefacts\Release\Plugin Lab.exe
cmake:  D:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe
```

## 关键架构决策

1. **扫描/加载后台线程**：juce::ThreadPool（任务安全 join）+ AsyncUpdater（UI 线程回调）→ UI 永不阻塞且无 use-after-free
2. **编辑器创建必须在消息线程**（JUCE 硬断言）——createEditorSafe 移到 callAsync 块内
3. **线程安全**：std::mutex listLock 保护 KnownPluginList；先拷贝 desc、释放锁、再加载（**禁止递归锁同一 mutex**）
4. **独立窗口**：PluginEditorWindow（DocumentWindow 子类，原生标题栏 + setContentOwned(editor,true) + setResizable(false,false) → 窗口内容区 == 编辑器尺寸）
5. **生命周期**：窗口析构必须显式 clearContentComponent()（防止编辑器在插件实例之后析构的 UB）
6. **崩溃保护**：PluginManager.cpp /EHa + catch(...)；禁止 JUCE_TRY/JUCE_CATCH_EXCEPTION（会重抛）
7. **黑名单**：已知杀进程插件在进入 DLL 前拦截

## 历史崩溃根因（已修复）

- 0xe06d7363 = C++ 异常逃逸（JUCE callAsync/消息回调未捕获）
- 0xc0000409 = std::terminate（后台 std::thread 抛异常）
- 0xc0000005 = 访问越界（VST3 DLL 崩溃，/EHa 捕获）
- 递归锁 = std::system_error（点击列表必失败）
- Pianoteq = 插件自杀 ExitProcess（黑名单拦截）

## 调试模式（可用）

- **日志**：`%TEMP%\pluginlab_crashlog.txt`（Win32 写入+FlushFileBuffers，崩溃安全）
- **UDP 实时流**：127.0.0.1:43210
- **minidump**：`%TEMP%\pluginlab_crash.dmp`（崩溃时自动生成）
- **监控脚本**：`D:\Documents\PluginLab\monitor.ps1`
- **GUI 自动化验收**：`C:\Users\admin\AppData\Local\Temp\opencode\gui_test_full8.py`（UIA select + 日志解析 + 崩溃重启）

## 待办（下一步）

- **T3 数据记录系统**（2026-08-02 定稿，详见 DESIGN.md §8）三阶段：
  - 阶段 1：修复 4 个已知 bug（见下）+ 恢复 IPC → measure 三件套 + Bypass 对比 + 元数据/配置导出
  - 阶段 2：FilePlayback（vocal 回放）+ AnalysisStrategy + ParameterTimeline + WAV 双轨导出
  - 阶段 3：PlotWidget 接线 + GR 表头逐块回调
- T2 稳定加固（EditorCrashGuard /EHa TU、Generic 编辑器兜底、观察者指针清理）— 当前已足够稳定，可按需实施
- 测量引擎接入 UI（PlotWidget 已写好未接线）
- signal/capture/analysis 模块已写好但未在 UI 中使用

## Oracle 架构审查（2026-08-02）

数据记录系统方案经 Oracle 验证，核心方向正确，发现以下问题：

### 待修 bug（现有代码，阶段 1 处理）

| #   | 位置               | 问题                                                                                                       | 优先级 |
| --- | ------------------ | ---------------------------------------------------------------------------------------------------------- | ------ |
| 1   | `SweepRunner.h:61` | `bool cancelled` 非原子（UI 线程写/工作线程读）→ 改 `std::atomic<bool>`                                    | P0     |
| 2   | `CaptureBuffer`    | 长时间录音数据全在内存，插件崩溃→全丢 → 需增量 WAV 写入（每 N 秒 flush）                                   | P0     |
| 3   | `ToneBurst.cpp:31` | `setAmplitude()` 覆盖 `levels` 数组，与 `setLevels()` 语义冲突 → 拆为 `setMasterAmplitude(scale)`          | P0     |
| 4   | `Export.cpp:16`    | `pluginName.quoted()` 不转义内部引号 → 改用 `juce::JSON::toString()`；`dropLastCharacters(2)` 逗号处理脆弱 | P0     |

### 测量方法修正（已写入 DESIGN.md §3）

- EQ：改用 **Farina 去卷积法**（扫频→IR→FFT），而非直接 FFT 比值（低频/高频相位噪声更小）
- 压缩：ToneBurst 需 **80Hz/1kHz/4kHz 多频点**（现代压缩器有频率依赖侧链）
- 谐波：**单音测 THD + 多音测 IMD 分开**（MultiTone 谐波峰交叠）

### 架构决策（已写入 DESIGN.md §8）

- 模块边界：SweepRunner 不动 / MeasurementSession 扩展 / RecorderEngine 新增顶层协调，**不做过度设计**
- FilePlayback 复用 SignalGenerator 接口（需声道映射）；分析阶段由 AnalysisStrategy 按 sourceType 分流
- 数据格式必须含：**Bypass 双路对比** + 插件元数据（class_id/latency_samples）+ 测量配置
- 实时 GR 表头：progressCallback 扩展传干/湿块 → `AsyncUpdater` 每 ~50ms 刷新 UI

### 可选改进（P2）

- MultiTone 加随机初始相位（降峰值因子）
- MeasurementSession 增加 `runMultiple(configurations)` 多轮参数扫描接口
- `Impulse`（MLS）接入 EQ 线性测量（比扫频快）

## 目录结构

```
source/
├── Main.cpp              # 主窗口 + 后台线程扫描/加载 + 独立窗口管理
├── host/PluginManager    # VST3 扫描/加载（/EHa + 黑名单）
├── ui/PluginEditorWindow # 独立插件编辑器窗口（DocumentWindow 子类）
├── signal/               # 信号生成器 (SineSweep/MultiTone/ToneBurst/Impulse + 规划 FilePlayback)
├── capture/              # 采集引擎 (CaptureBuffer/SweepRunner/MeasurementSession + 规划 RecorderEngine/ParameterTimeline/AnalysisStrategy)
├── analysis/             # 分析引擎 (FreqResponse/Harmonic/CompressionCurve/Export + 规划 WavExporter)
├── ipc/                  # Named Pipe 控制 (PipeServer/CommandParser/Protocol)
├── ui/PlotWidget         # 绘图组件
└── utils/                # FftHelper/MathUtils/CrashLog
tools/VST3Scanner         # 独立扫描工具（当前未使用，主进程内扫描）
monitor.ps1               # 实时监控脚本
DESIGN.md                 # 设计文档
```
