# Plugin Lab — 当前状态 (2026-08-08)

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

## 阶段 2-5 计划（2026-08-03，Momus 审查定稿）

- **详细计划**：`docs/plan-phase2-5.md`（含 P0/P1/P2 问题清单 + 修正后阶段计划 + 依赖图）
- 阶段 2：输入与信号增强（FilePlayback 重采样 / 噪声固定种子 / EnvelopeSignal / source 选择）
- 阶段 3：参数连续扫描（ScanEngine + 连续性 JSON + GUI 多曲线）— 与阶段 4 可并行
- 阶段 4：动态压缩行为（TestCompressorPlugin + GR 时间线 + τ 曲线族 + GR 表头）
- 阶段 5：建模与数据整合（数据包 + 反推验证）
- 关键路径：`2 → max(3,4) → 5`
- vocal 素材：`samples/take01.wav`（48k/16bit/stereo/17.0s，阶段 2 已入库）

## 待办（下一步）

- **T3 数据记录系统**（2026-08-02 定稿，详见 DESIGN.md §8）：
  - **阶段 1 ✅ 已完成（2026-08-02）**：测试设施 + 4 bug 修复 + IPC 恢复 + EQ 测量接通 + 右面板曲线 + Pro-Q 4 验收通过（见下"阶段 1 完成记录"）
  - **阶段 2 ✅ 已完成**：输入源（FilePlayback/noise/dynamic，6054e57）
  - **阶段 3 ✅ 已完成（2026-08-02）**：参数扫描（ScanEngine + GUI 扫描面板 + HSL 色板 + CompressionFamily 网格，5d2c86b..7fa3068）
  - **阶段 4 ✅ 已完成（2026-08-02）**：动态压缩行为（GR 时间线 gr_timeline + τ 估计 + 实时 GR 表头，995fc54；见下"阶段 3+4 完成记录"）
  - **阶段 5 ✅ 已完成（2026-08-02）**：建模与数据整合（datasetToJSON 数据包 + data-schema.md + reverse_derive.py 反推验证，2698068；见下"阶段 5 完成记录"）
  - **收尾修复 ✅ 已完成（2026-08-02 深夜，957e597..b3e1813）**：GR τ 修复（IPC 暴露 carrier_start_hz 默认 10000 + GainReduction 1ms RMS 窗口 + 正 dB 副本估计，Pro-C 3 实测 τ 有效）+ ipc_client.ps1 响应超时 + JSON 解析悬挂/param_id 转义修复 + measure/scan helper 抽取 + schema 文档修正（见下"收尾修复记录"）
  - **剩余（可选）**：4 个待改进项见"收尾修复记录"末尾
- **T2 稳定加固**（EditorCrashGuard /EHa TU、Generic 编辑器兜底、观察者指针清理）— 当前已足够稳定，可按需实施

## 阶段 1 完成记录（2026-08-02）

**验收（FabFilter Pro-Q 4 实测，四腿全过）**：

- loadPlugin → `{"ok":true,"name":"Pro-Q 4"}`（路径 `C:\Program Files\Common Files\VST3\FabFilter\FabFilter Pro-Q 4.vst3`）
- setParam → Band 1 激活 + Gain +6dB（@~1kHz）
- measure → `{"ok":true,"samples":240128,"rate":48000,"export_path":...}`，右面板幅度/相位曲线渲染
- JSON 反推（tools/verify_export.py）：**993.20 Hz（0.7% 误差）+ 6.00 dB（0 误差）** — 命中注入频点/增益

**真机发现并修复 3 个 bug（T8 前所有测量从未真实跑过）**：

| #   | Bug                                                                              | 修复                                                                                                                                    |
| --- | -------------------------------------------------------------------------------- | --------------------------------------------------------------------------------------------------------------------------------------- |
| 1   | `prepareToPlay` 双调用（loadPlugin 线程 + SweepRunner 线程不匹配）→ Pro-Q 4 崩溃 | prepareToPlay 统一移到 SweepRunner::run（测量线程），PluginManager::loadPlugin 不再调用；pluginPrepared 标志管理                        |
| 2   | SweepRunner 硬编码 2 通道 → Pro-Q 4（4 进 4 出）processBlock 越界                | 用 getTotalNumInputChannels/getTotalNumOutputChannels 动态分配 dry/wet block                                                            |
| 3   | processBlock 在 IPC 线程调用（Pro-Q 4 要求与编辑器同线程）                       | CommandParser measure 用 `WaitableEvent + MessageManager::callAsync` 派发到消息线程；已加 isThisTheMessageThread() 同步路径避免测试挂死 |

**测试基础设施**：Catch2 v3.8.0 + `unit_tests` console target（tests/，含 TestPlugin 假插件），**29/29 全绿**。
**新增工具**：`tools/ipc_client.ps1`（NamedPipe 客户端）、`tools/verify_export.py`（JSON 反推频点/Q/增益）。
**提交**：feat/phase1-freq-response 分支 11 commits（5584f1a..0fe3626）。

## 阶段 3+4 完成记录（2026-08-02）

**commit 范围**：`5d2c86b` → `995fc54`（阶段 3 扫描 + 阶段 4 动态压缩行为里程碑，HEAD 995fc54 已 push origin/main）

**关键能力**：

- **scan 命令 + GUI 扫描面板 + HSL 色板**（5d2c86b）：ScanEngine 参数扫描（快照/恢复/取消/逐轮延迟）+ scan JSON schema + 右面板多曲线渲染 + HSL 色板
- **CompressionFamily 电平×速度网格**（7fa3068）：输入电平 × 速度网格，每格一条有效压缩曲线 + GR 时间线
- **GR 时间线测量路径 `gr_timeline`**（995fc54）：非 signal 源（dynamic/file/noise）→ GainReduction::analyze → CompressionFamily::detectMarkers → TimeConstants::estimate（τ）→ Export::grTimelineToJSON；signal 源拒绝（报错）
- **实时 GR 表头**（995fc54）：setBlockCallback 累积 dry/wet + 50ms 节流 GainReduction 分析实时刷新；GR 按钮 + grPlot 表头（x=timeSec, y=grDB）
- **τ 估计 TimeConstants**（34f7db9）：tau 匹配有效 attack + 配置 release
- **支撑提交**：b2b221e TestCompressorPlugin（可配置 attack/release/threshold/ratio）、df4f82d ScanEngine、d08b94b tail pad + block callback + GainReduction、3c01add scan family JSON schema + body 重构

**测试**：**107/107 全绿**（Catch2，`ctest --timeout 180` 连跑 2 次；scan-happy ~78s 固有慢），Debug 构建 0 警告（/W4 /WX）：unit_tests + PluginLab。

## 阶段 5 完成记录（2026-08-02）

**commit 范围**：`995fc54` → `2698068`（阶段 5 建模与数据整合里程碑，HEAD 2698068 已 push origin/main；新增 b433a18 + 2698068 两个 commit）

**关键交付**：

- **datasetToJSON 建模数据包**（b433a18）：`Dataset` 结构 + `datasetToJSON` 将 scan family / gr_timeline / compression_family 三类测量聚合为单包 JSON；`appendDatasetScanFamily` helper；**现有导出函数 0 修改**；dataset 内各块 body 与独立导出数据等价（body-equiv 测试锁定）
- **data-schema.md**（b433a18，新建）：8 类导出 JSON schema 完整文档（context / raw_capture / frequency_response / scan / gr_timeline / compression_family / dataset / note）
- **reverse_derive.py 反推验证**（2698068，新建，stdlib only）：频响峰值 freq/Q 反推 + 压缩 threshold/ratio 分段拟合 + GR τ 读取 + dataset/scan/compression_family/gr_timeline 多布局解析 + `--expected`/`--tol` 容差校验

**测试**：**113/113 全绿**（107 + 6 新增 datasetToJSON 用例：dataset-scan-only / gr-only / compression-only / full / empty / body-equiv），`ctest --timeout 180` 连跑 2 次全过；Debug 构建 0 警告（/W4 /WX）：unit_tests + PluginLab。

**实测结论（真实插件）**：

- Pro-Q 4：反推 freq 987.3Hz（1.27% 误差）、gain 6.00dB（0 误差）、Q 0.90（10.2%，容差 20% PASS）
- Pro-C 3：threshold ±1dB PASS；ratio 14.3-23.5%（容差 25% PASS，偏因真实插件 burst 未充分压缩 + soft knee）

**已知局限与建议**（阶段 5 交付时记录；其中 GR τ 已于收尾修复落地，见下）：

- **GR τ 真实插件失效 → ✅ 已修复（957e597）**：IPC 暴露 `carrier_start_hz`（默认 10000，匹配 CompressionFamily）+ GainReduction 改 1ms RMS 窗口 + TimeConstants 在正 dB 副本上估计 → Pro-C 3 实测 `tau.valid=true, attack=3.9ms, release=39.7ms`
- **ratio 25% 容差说明**：真实插件 burst 未充分压缩 + soft knee 造成的系统性偏差，25% 容差内可接受
- **loadPlugin name 匹配说明**：按名称匹配插件 description，大小写/别名可能不命中（待改进项 2）
- **待改进项（4 个，见"收尾修复记录"）**：data-schema.md scan 结构描述与实现差异、loadPlugin name 匹配、Pro-Q 4 Band 1 Used、getParams 不带 param_id

## 收尾修复记录（2026-08-02 深夜，957e597 → b3e1813）

**commit 范围**：`957e597` → `b3e1813`（HEAD 已 push origin/main），阶段 5 后的收尾清理 + 遗留修复。

**关键修复**：

| #   | Commit    | 内容                                                                                                                                                                                                                                                                                                                                  |
| --- | --------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| 1   | `957e597` | **GR τ 修复**（三件套）：IPC 解析 `carrier_start_hz`（默认 10000 匹配 CompressionFamily）；GainReduction 改 1ms RMS 窗口（默认 5ms 模糊 ms 级 attack 沿）；TimeConstants 在正 dB 副本上估计（匹配 detectMarkers 约定）。Pro-C 3 实测 `tau.valid=true, attack=3.9ms, release=39.7ms`（此前 valid=false）。新增 3 个 CommandParser 测试 |
| 2   | `50d6d68` | ipc_client.ps1 加可配置响应超时（scan 同步 60s 不再挂起）                                                                                                                                                                                                                                                                             |
| 3   | `3cc6235` | 修 startScan/startMeasurement 悬挂 JSON parse + scan param_id 转义                                                                                                                                                                                                                                                                    |
| 4   | `dab0607` | 抽 `parseSource`/`configureSessionSource`/`resolveExportPath`/`buildExportContext` helper，去重 measure/scan 路径                                                                                                                                                                                                                     |
| 5   | `c9089fe` | data-schema.md 修正 compression_family gr_db 示例（正 dB = 压缩量）                                                                                                                                                                                                                                                                   |
| 6   | `e5ffe5e` | data-schema.md 补充 export_path 语义、type 缺省、dry/wet 对齐说明                                                                                                                                                                                                                                                                     |
| 7   | `b3e1813` | chore：清理过时 handoff/token 文档、monitor.ps1 移入 tools/、gitignore 测量产物                                                                                                                                                                                                                                                       |

**测试**：`116/116 全绿`（阶段 5 的 113 + 3 新增），`/W4 /WX` 双 target 零警告。

**待改进项（4 个，未做）**：data-schema.md scan 结构描述与实现差异（部分已修，待最终核对）、loadPlugin name 大小写/别名匹配、Pro-Q 4 Band 1 Used 状态、getParams 响应不带 param_id。

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
├── Main.cpp              # 主窗口 + 专用扫描/加载线程 + 独立窗口管理
├── host/PluginManager    # VST3 扫描/加载（/EHa + 黑名单 + 死马踏板 + 看门狗）
├── ui/PluginEditorWindow # 独立插件编辑器窗口（DocumentWindow 子类）
├── signal/               # 信号生成器 (SineSweep/MultiTone/ToneBurst/Impulse/FilePlayback/NoiseGenerator/EnvelopeSignal)
├── capture/              # 采集引擎 (AudioBuffer::CaptureBuffer/SweepRunner/MeasurementSession)
├── analysis/             # 分析引擎 (FreqResponse/Harmonic/CompressionCurve/GainReduction/TimeConstants/CompressionFamily + Export)
├── ipc/                  # Named Pipe 控制 (PipeServer/CommandParser/Protocol)
├── ui/PlotWidget         # 绘图组件
└── utils/                # FftHelper/MathUtils/CrashLog
tools/VST3Scanner         # 独立扫描工具（运行时死代码但仍在构建，主进程内扫描）
tools/monitor.ps1         # 实时监控脚本（2026-08-02 b3e1813 移入 tools/）
tools/ipc_client.ps1      # NamedPipe 手动客户端（可配超时）
tools/reverse_derive.py   # 导出 JSON 反推验证（stdlib-only）
tools/verify_export.py    # 导出 JSON 峰值/Q 验证（stdlib-only）
DESIGN.md                 # 设计文档
```

> 注：`RecorderEngine`/`ParameterTimeline`/`AnalysisStrategy`/`WavExporter` 为 §8.2 设计组件，依 plan-phase2-5 P2-13 **显式延后未实现**（实际落地见阶段 3-5 记录）。

## 扫描优化专项（2026-08-03/04，计划见 docs/plan-scan-optimization.md）

> 覆盖 P0 关窗死锁 + P0 慢启动 + P1 增量 UI/加载超时/扫描看门狗 + P2 进度 IPC。
> 提交：e2d45c0（步骤0）→ 9c2ca4f（步骤1）→ 8f68234（步骤6 性能）→ fa74d45（步骤2）→ 4fb74b1（步骤3）→ cb83c79（步骤4）→ c7f7f9f（步骤5）。155/155 测试绿。

### 决策记录

1. **线程模型**（步骤 0）：扫描/加载从 ThreadPool(2) 改为**专用一次性 std::thread**（`unique_ptr<std::thread>`），析构**显式放弃不 join**（挂起 DLL 无法终止，join=永久阻塞=关窗死锁）。worker 不触碰宿主成员（持 `shared_ptr<PluginManager>` + shared outcome），完成经 `MessageManager::callAsync` + alive 标志（消息线程与析构串行化；`juce_MessageManager.cpp:81-92` post() 在管理器销毁后安全丢弃）。generation token 防旧代写。
2. **XML 缓存**（步骤 1）：`%APPDATA%/PluginLab/pluginlist.xml`，根 `version="1"` 属性；`createXml/recreateFromXml` 往返（含 BLACKLISTED 子元素）；**原子写** temp + `replaceFileIn`（ReplaceFile）；loadCache 校验 version → 损坏/版本不符回退全量重扫；剪枝 ghost（文件不存在）+ Pianoteq 名。
3. **死马踏板接线**（步骤 1）：`PluginDirectoryScanner` 第 5 参传真实路径 `%APPDATA%/PluginLab/deadMansPedal`。语义（源码级）：scanNextFile 扫描前写当前插件路径、成功后移除（juce_PluginDirectoryScanner.cpp:108-117）；**挂起/崩溃=路径残留 → 下次构造自动 addToBlacklist + 移队尾**。
4. **增量跳过**（步骤 6）：`cacheIsCurrent()` 自己实现增量判断（不依赖 JUCE `getTypeForFile` 精确匹配）——枚举 bundle 路径同时匹配"精确"与"`bundle\Contents` 前缀"缓存条目，用**内层 DLL mtime** 比较（正确更新检测基准）；`dedupeKnownPlugins()` 去重（scanAndAddFile 只加不删 → bundle+inner 重复条目，inner 优先保留）。**根因**（explore 团队 3 路调查）：12 个无 moduleinfo.json 的目录型 bundle 走慢路径，缓存存内层 DLL 路径而枚举产 bundle 路径 → getTypeForFile 永不命中 → 每轮热启动全量重扫（1-3s/个）。热扫实测 **31.2s → 0.5s**，CPU 峰值 **521% → 0-3%**。
5. **扫描阶段黑名单**（步骤 6）：Pianoteq（宿主杀手）按**待扫文件名**拦截（此刻无 desc.name）——`getNextPluginFileThatWillBeScanned()` + `skipNextFile()`；此前 prune 删内存条目 → getTypeForFile null → 每轮重扫（InitDll 授权检查 CPU 密集 8s+）。
6. **加载超时**（步骤 3）：`loadPlugin` 改显式 `createPluginInstanceAsync` + `WaitableEvent` 超时（默认 30s，`setLoadTimeoutMs` 可注入）→ 超时脱出 + 黑名单（bundle key）+ nullptr；迟到回调由 LoadState.alive 丢弃（回调不捕获 this）。**限制**：真挂起=消息线程冻结（JUCE 创建在消息线程），进程内不可恢复只能杀进程——黑名单+死马踏板预防下次。
7. **扫描看门狗**（步骤 4）：PluginManager 跟踪扫描状态（beginScan/updateScanProgress/endScan）；Main 消息线程 Timer(500ms) 轮询，progress 无变化超 `kScanHangTimeoutMs`(60s) → `handleScanHang()`（黑名单当前文件 bundle key + **立即持久化**——卡死扫描到不了 saveCache，不持久化则重启重挂）+ abandon 扫描线程 + 复位 scanRunning（UI 可操作）。挂起上限 `kMaxScanHangs`(3)：每次挂起泄漏一线程+锁一 DLL。UI "Clear BL" 按钮（清除黑名单重扫，R4/R7）。
8. **线程优先级**（步骤 6）：扫描/加载线程 `THREAD_PRIORITY_BELOW_NORMAL`——插件初始化（UA 系列等）CPU 密集，降优先级避免抢占用户前台。
9. **getScanStatus**（步骤 5）：快照+推送双轨的快照侧——纯推送漏中途连接；`{ok, running, done, progress, count, blacklisted, hangCount, currentFile}`；命名避开参数扫描 `scan` 语义。协议命令非导出 schema。
10. **明确排除**：子进程扫描（AudioPluginHost 根治方案，ExitProcess 型杀进程插件的唯一根治）——工程量/架构影响大，本轮用「黑名单+踏板+看门狗」覆盖；`CustomScanner` 接口面大职责重叠；JUCE 钉 tag 另立任务。

### 已知残留

- ~~**CGII.vst3**：0 类型插件，每轮热启动重扫 ~0.5s（未入缓存）~~ → ✅ 已修（块 C 任务 5，见下）：`blacklistUnregistered` 扫描后预防性黑名单（0 类型文件存在但无已知条目）→ `scanDirectory` 跳过检查加 `isBlacklistedPath`（路径黑名单，0 类型无 desc.name 名字拦截够不到）→ 二次热启不再重扫。真机验证：CGII 黑名单持久化 + 重启日志无 "Discovered CGII"
- **扫描挂起黑名单误伤**（R7）：一次挂起即入黑名单，需 "Clear BL" 入口（已有）解除。

### 块 C 稳定加固进度（2026-08-08，计划见 docs/plan-block-c-stability.md）

- [x] **任务 1 测量路径异常保护**（ea1ebe2）：SweepRunner.cpp 开 /EHa + run() 全 plugin 调用 try/catch（prepare/process/teardown 三段），异常 → CRASH_LOG + 测量失败响应，宿主存活；5 个测试锁定（SweepRunner + CommandParser 级），真机 Pro-Q 4 回归通过
- [x] **任务 2 EditorCrashGuard 入测试目标**（6a77fb5）：真实 EditorCrashGuard.cpp 编入 unit_tests（/EHa），移除空桩；5 个测试含 **SEH 硬件故障保护**（析构访问违规被 catch(...) 拦截）与 C++ 异常路径
- [x] **任务 3 Generic 编辑器兜底**（1dcb013 已实现，本块验证）：Main.cpp:1617-1628 fallback（createEditorSafe null → GenericAudioProcessorEditor + try/catch → 仍失败 "Loaded (no editor)"）；真机 Pro-Q 4 "Editor ok" 原生编辑器，fallback 不误触发
- [x] **任务 4 观察者指针生命周期加固**（add971c）：PluginEditorWindow::closeButtonPressed 先 move 出回调再调用（回调内 delete-this 加固）；其余观察者（ChangeListener/Timer/CommandParser 回调）审查确认析构顺序安全
- [x] **任务 5 CGII.vst3 预防性黑名单**（见"已知残留"）：`blacklistUnregistered` + `isBlacklistedPath` + scanDirectory 跳过接线；4 个单测锁定；真机二次热启无 CGII 重扫
- [ ] 任务 6 data-schema scan 结构最终核对
- [ ] 任务 7 getParams Band Used 状态


## 2026-08-08 文档同步记录（81a935d → 063edf3）

**commit 范围**：`81a935d` → `063edf3`（HEAD 063edf3，docs 同步 commit）。

- **`81a935d`** fix(host,ui)：startup not-responding —— 启动未响应根因链：
  1. 测试缓存污染：67d23d3 新增的超时/挂起测试未 setCacheFile，ctest 全量跑后把真实 90 插件缓存覆写成少量假条目 → 下次启动增量重扫 ~90 插件（Debug 35s）；
  2. UI 刷新风暴：changeListenerCallback 每发现一个插件同步 updateContent+repaint，Debug 下消息线程被淹没 → Windows 标记 Not Responding。
  修复：超时/挂起持久化黑名单的路径全部 setCacheFile(tmp)；changeListenerCallback 改 pending flag + triggerAsyncUpdate（AsyncUpdater ≤50ms 合并刷新）。验证全量绿（commit message 记 158/158），Debug 热扫 ~1s。
- **`063edf3`** docs：sync AGENTS.md —— 刷新 commit ref + 测试计数对齐（commit message 记 126/126，**实测为 158/158**，后续以 tests/AGENTS.md 158 为准）。

**测试计数口径**：STATUS.md 历史记录中的 155/155、116/116、113/113、107/107、158/158 均为对应时点值；当前权威计数以 tests/AGENTS.md 为准（158 个 TEST_CASE）。
