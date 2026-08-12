# Plugin Lab — 当前状态 (2026-08-11)

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
   - 长期方案：进程外托管（ChildProcessCoordinator）——**已实施（块 D，2026-08-11，见 git 历史 docs/archive/plan-block-d-out-of-process.md）**：黑名单插件经 PluginHostChild 子进程托管，D4 实战验收 Pianoteq 9 在子进程加载即杀子进程 → 宿主检测 heartbeat timeout → 自动重启 3 次上限 → 返回明确错误，**宿主永不死亡**
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

- **详细计划**：`git 历史 docs/archive/plan-phase2-5.md`（含 P0/P1/P2 问题清单 + 修正后阶段计划 + 依赖图）
- 阶段 2：输入与信号增强（FilePlayback 重采样 / 噪声固定种子 / EnvelopeSignal / source 选择）
- 阶段 3：参数连续扫描（ScanEngine + 连续性 JSON + GUI 多曲线）— 与阶段 4 可并行
- 阶段 4：动态压缩行为（TestCompressorPlugin + GR 时间线 + τ 曲线族 + GR 表头）
- 阶段 5：建模与数据整合（数据包 + 反推验证）
- 关键路径：`2 → max(3,4) → 5`
- vocal 素材：`samples/take01.wav`（48k/16bit/stereo/17.0s，阶段 2 已入库）

## 待办（下一步）—— 以 GitHub issue 为准

> 2026-08-11 更新：原路线图（docs/roadmap-next.md）五块（C/E/A/B/D）已全部完成并删除（内容在 git 历史与 PR 记录中）。**需求与待开发全部走 GitHub issue**（当前：issue #7 子进程测量扩展）；历史待办均已交付（T3 阶段 1-5 见本文件完成记录；T2 稳定加固并入块 C 于 2026-08-08 完成；块 B 记录模式于 2026-08-10 完成，见下方完成记录）。

- **块 4 D 进程外托管**（**下一步**，设计门）：`git 历史 docs/archive/plan-block-d-out-of-process.md` 6 设计问题各附推荐+理由+备选+决策标准，逐条确认即拆票（D0-D6）

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

**待改进项（4 个，未做）**：data-schema.md scan 结构描述与实现差异（**✅ 已核对，块 C 任务 6，f44e7a0**）、loadPlugin name 大小写/别名匹配（**未做，仍开放**）、Pro-Q 4 Band 1 Used 状态（**✅ 已验证，块 C 任务 7：getParams 暴露 Band 1 Used name/value/param_id，setParam 按 param_id 可切换**）、getParams 响应不带 param_id（**✅ 已修，3332e3d 前已带 param_id，测试 [getParams-param-id] + [band-used] 锁定**）。

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
├── host/child*           # 进程外托管（块 D）：ChildProcessCoordinator / ChildMeasureOrchestrator / ChildMeasureContract（黑名单插件测量）
├── ui/PluginEditorWindow # 独立插件编辑器窗口（DocumentWindow 子类）
├── signal/               # 信号生成器 (SineSweep/MultiTone/ToneBurst/Impulse/FilePlayback/NoiseGenerator/EnvelopeSignal)
├── capture/              # 采集引擎 (AudioBuffer::CaptureBuffer/SweepRunner/MeasurementSession/ParameterTimeline)
├── analysis/             # 分析引擎 (FreqResponse/Harmonic/CompressionCurve/GainReduction/TimeConstants/CompressionFamily + Export + WavExporter)
├── ipc/                  # Named Pipe 控制 (PipeServer 双轨并发/CommandParser/Protocol)
├── child/                # 子进程可执行（块 D）：PluginHostChild（VST3 加载 + 测量，stdin/stdout JSON 协议）
├── ui/PlotWidget         # 绘图组件
└── utils/                # FftHelper/MathUtils/CrashLog
tools/VST3Scanner         # 独立扫描工具（运行时死代码但仍在构建，主进程内扫描）
tools/ipc_client.ps1      # NamedPipe 手动客户端（多行读取 + -CancelAfterMs，可配超时）
tools/monitor.ps1         # 实时监控脚本（2026-08-02 b3e1813 移入 tools/）
tools/pipe_client.py      # Python NamedPipe 客户端（stdlib + ctypes，batch_collect 依赖）
tools/batch_collect.py    # 批量采集 CLI 驱动（dataset 流程编排）
tools/compare_freq.py     # 频响对比（MLS vs sweep 验收，|Δ|<0.5dB）
tools/reverse_derive.py   # 导出 JSON 反推验证（stdlib-only）
tools/verify_export.py    # 导出 JSON 峰值/Q 验证（stdlib-only）
DESIGN.md                 # 设计文档
```

> 注：`RecorderEngine`/`AnalysisStrategy` 为 §8.2 设计组件，依 plan-phase2-5 P2-13 **显式延后未实现**（实际落地见阶段 3-5 记录）；`ParameterTimeline`/`WavExporter` 同列 §8.2 组件，**已于 2026-08-10 块 B 落地**（见下方 B 块完成记录）。

## 扫描优化专项（2026-08-03/04，计划见 git 历史 docs/archive/plan-scan-optimization.md）

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

### 块 C 稳定加固进度（2026-08-08，计划见 git 历史 docs/archive/plan-block-c-stability.md）

- [x] **任务 1 测量路径异常保护**（ea1ebe2）：SweepRunner.cpp 开 /EHa + run() 全 plugin 调用 try/catch（prepare/process/teardown 三段），异常 → CRASH_LOG + 测量失败响应，宿主存活；5 个测试锁定（SweepRunner + CommandParser 级），真机 Pro-Q 4 回归通过
- [x] **任务 2 EditorCrashGuard 入测试目标**（6a77fb5）：真实 EditorCrashGuard.cpp 编入 unit_tests（/EHa），移除空桩；5 个测试含 **SEH 硬件故障保护**（析构访问违规被 catch(...) 拦截）与 C++ 异常路径
- [x] **任务 3 Generic 编辑器兜底**（1dcb013 已实现，本块验证）：Main.cpp:1617-1628 fallback（createEditorSafe null → GenericAudioProcessorEditor + try/catch → 仍失败 "Loaded (no editor)"）；真机 Pro-Q 4 "Editor ok" 原生编辑器，fallback 不误触发
- [x] **任务 4 观察者指针生命周期加固**（add971c）：PluginEditorWindow::closeButtonPressed 先 move 出回调再调用（回调内 delete-this 加固）；其余观察者（ChangeListener/Timer/CommandParser 回调）审查确认析构顺序安全。真机验证：IPC 加载 Pro-Q 4 → WM_CLOSE 关闭（editor 窗口 + 主窗口）→ 干净退出，crashlog 无 ERROR
- [x] **任务 5 CGII.vst3 预防性黑名单**（见"已知残留"）：`blacklistUnregistered` + `isBlacklistedPath` + scanDirectory 跳过接线；4 个单测锁定；真机二次热启无 CGII 重扫
- [x] **任务 6 data-schema scan 结构最终核对**（f44e7a0）：§5 scan 与 scanToJSON 逐字段核对一致（顶层 scan/context 七字段/family/精度/dataset 内嵌）；补 context 顶层 sample_rate + source 断言锁定（[export][scan-schema] 30 断言）；data-schema.md 记核对记录
- [x] **任务 7 getParams Band Used 状态**：Pro-Q 4 真机验证——getParams 暴露 "Band 1 Used"（name/param_id/value），setParam 按 param_id 切换成功（0→1 回读确认）；TestPlugin 锁定测试 [band-used]；STATUS 待改进项 3/4 标记解决
- [ ] ~~任务 6/7 收尾~~（已完成）


## 2026-08-08 文档同步记录（81a935d → 063edf3）

**commit 范围**：`81a935d` → `063edf3`（HEAD 063edf3，docs 同步 commit）。

- **`81a935d`** fix(host,ui)：startup not-responding —— 启动未响应根因链：
  1. 测试缓存污染：67d23d3 新增的超时/挂起测试未 setCacheFile，ctest 全量跑后把真实 90 插件缓存覆写成少量假条目 → 下次启动增量重扫 ~90 插件（Debug 35s）；
  2. UI 刷新风暴：changeListenerCallback 每发现一个插件同步 updateContent+repaint，Debug 下消息线程被淹没 → Windows 标记 Not Responding。
  修复：超时/挂起持久化黑名单的路径全部 setCacheFile(tmp)；changeListenerCallback 改 pending flag + triggerAsyncUpdate（AsyncUpdater ≤50ms 合并刷新）。验证全量绿（commit message 记 158/158），Debug 热扫 ~1s。
- **`063edf3`** docs：sync AGENTS.md —— 刷新 commit ref + 测试计数对齐（commit message 记 126/126，**实测为 158/158**，后续以 tests/AGENTS.md 158 为准）。

**测试计数口径**：STATUS.md 历史记录中的 155/155、116/116、113/113、107/107、158/158、186/186、199/199、208/208 均为对应时点值；当前权威计数以 tests/AGENTS.md 为准（**265 个 TEST_CASE，2026-08-11 实测**）。

## E 块任务 E3 验证记录（2026-08-08）

- 原 roadmap 任务“MeasurementSession::runMultiple 多轮参数扫描接口”已被 ScanEngine::run 完整覆盖（阶段 3 交付），本任务重新定性为验证型，无新增代码：
  - 多轮参数扫描：run(paramId, values[], type, progress) 每值一轮（ScanEngine.cpp:99-168）✅
  - 曲线族/数据面：ScanResult.family[]（每轮 freq/harmonic/compression + latency + cancelled）✅
  - 取消：cancel() 线程安全，round 边界生效 ✅
  - 参数快照/恢复：RAII ParamGuard（entry 快照全部参数，exit 恢复含取消/异常）✅
  - 进度：progress(round+1, totalRounds) 每轮后回调 ✅
  - 块 A 复用：dataset 命令基于 ScanEngine（git 历史 docs/archive/plan-batch-pipeline.md S1/S4 真机通过）✅
- 结论：E3 无缺口，标记完成。

## E 块完成记录（2026-08-08，块 1 E 测量质量改进；计划 v2 见 git 历史 docs/archive/plan-block-e-measurement-quality.md）

**commit 范围**：`7c83631`（E3 文档）→ `850ecc3`（E2）→ `4ed51e4`（E1）→ `7480612`（E 收尾审查修复，HEAD 已 push origin/main）。

**关键交付**：

- **E1 MLS 接入 EQ 频响**（4ed51e4，+ 7480612 修复）：`FreqResponse::analyzeMLS` 整段 FFT 频域除法（H=Y/X，FFT size = 2×mlsLength 补零避免循环卷积混叠 + 低能量保护）；重构抽取 `applySmoothing`/`applyPhasePost` 私有辅助（H1 扫频路径共用，既有 [freqresponse] 用例锁定行为）；`MeasurementSession::setFreqExcitation(useMLS)` + IPC measure/dataset 可选 `excitation:"sweep"|"mls"` 字段（缺省 sweep 向后兼容）。真机验收：MLS vs sweep 100Hz-10kHz 平均 |Δ| < 0.5dB PASS
- **E2 MultiTone 确定性随机初始相位**（850ecc3）：`setRandomPhaseSeed(seed)`——0（默认）= 旧全零相位波形字节级不变；非 0 = xorshift32 确定性 PRNG 每频点 [0,2π) 相位，降峰值因子（8 音 CF < 4.0）。4 个 [multitone] 用例锁定（CF 下降 / 同种子位级可复现 / 异种子不同 / seed 0 兼容）
- **E3 runMultiple 验证型**（7c83631，无新增代码）：ScanEngine::run 完整覆盖多轮扫描/曲线族/取消/快照恢复/进度，块 A 复用兑现

**测试**：199/199 全绿（基线 186 + E2 4 + E1 5 + 审查修复 4；含激励泄漏收敛 3 用例 + analyzeMLS 短录制 clamp 1 用例），`ctest --timeout 180` 双跑 + 真机验收 + 双轴审查（Standards + Spec）。

**审查修复要点**（7480612）：scan/dataset/measure 三路径激励泄漏收敛（`scan` 结束复位 freq excitation 防残留、dataset scan 块透传 excitation、非 freq 类型忽略 excitation）+ analyzeMLS 短录制（< MLS 周期）DFT clamp 防越界。

## B 块完成记录（2026-08-10，块 3 B 记录模式；计划 v2 见 git 历史 docs/archive/plan-block-b-recording.md）

**commit 范围**：`5c128da`（B1）→ `b6d90b9`（B2）→ `531c9eb`（B3）→ `50cb115`（B 收尾文档，HEAD 已 push origin/main）。

**开工决策**（brainstorming 草案逐条确认）：D1 单文件 6 声道交织 `[dryL,R wetL,R bypassL,R]`；D2 recordTimeline **v1 仅事件**非阻塞 + 独立 stopTimeline（音频由 playTimeline 采集）；D3 timeline 专用命令，不进 measure source 枚举；rate 可配置默认 1.0。

**关键交付**：

- **B1 WavExporter**（5c128da）：`WavExporter::exportTracks(dry, wet, sampleRate, wavPath)` 手写 44 字节 RIFF + 24-bit PCM（量化镜像 CaptureBuffer flush），单文件 3×声道交织，bypass = dry 副本（v1）；错误 → false + CRASH_LOG_WARN，无异常。IPC `exportWav`（四件套：Protocol.h + handleCommand + CommandParserTests + data-schema.md §9）：从最近一次测量的 session result 导出，路径复用 `wavPathFor`（.json→.wav）。**真机 Pro-Q 4：6 声道/48k/24bit/5.000s PASS**（时长与测量精确一致）。审查修复：测试补 6 声道全查（ch1/3/5）+ bitsPerSample 断言 + IPC 级内容布局验证（setGain 2.0 使 wet=2×dry 区分三轨）
- **B2 ParameterTimeline**（b6d90b9）：事件录制——AudioProcessorListener（JUCE 9 **双纯虚**：audioProcessorParameterChanged + audioProcessorChanged 均覆写）、mutex + 原子时间戳（C8 回调可触发于 IPC 线程）、param_id 稳定 id（R9 跳过空 id）、wall-clock 起点；回放——rate 预缩放（effectiveMs = timeMs/rate）、applyEventsUpTo 按光标推进、R2 参数快照恢复。MeasurementSession `setTimelinePlayback`（blockCallback seam 包装，**SweepRunner 冻结未动**；一次性标志防陈旧时间线泄漏；R2 恢复覆盖失败/取消路径）。IPC 三命令：`recordTimeline`（非阻塞 {ok,recording:true}）/`stopTimeline`（手写 JSON 导出 timeline）/`playTimeline`（阻塞镜像 measure 派发；WAV 复用 B1；play JSON 兄弟 `_play.json` 绝不覆盖输入）。**真机 Pro-Q 4：record → 3×setParam（time_ms 322/843/1363，id/值精确）→ stop（3 事件 JSON）→ play（240000 samples，WAV 6 声道）→ R2 参数恢复 PASS**。审查修复：playTimeline 拒绝录制中（防回放/R2 恢复污染录音）、stopRecording 锁外分离 removeListener（消丢事件竞态 + 规避 JUCE 通知锁死锁）、findParam 同模块去重（ParameterTimeline::findParam 静态，MeasurementSession 复用）+ 文档同步（tests/AGENTS.md 208、capture 7 文件、根 AGENTS.md 62 文件）
- **B3 GUI 面板**（531c9eb）：MainContentComponent Record/Stop TL/Play 按钮 + ProgressBar（JUCE 9 API：构造取 `double&` 引用 + 内部 50ms 定时器自刷新，值 <0 显示 spinner）；按钮为同一 handleCommand 薄包装（R4：IPC 主路径，GUI 后置）；**回放期置 measurementInProgress 共享重入守卫**（闭合 Measure/Scan/Record 重入口）；布局第 4 行（controls 124px）。审查修复：重入守卫 + 死状态 timelinePlaybackInProgress 清除 + 布局 124px + 注释准确性

**测试**：**208/208 全绿**（199 + B1 3 [wavexporter][exportwav] + B2 6 [paramtimeline][commandparser] timeline 命令），`ctest --timeout 180` 双跑 + 真机验收 + 双轴审查（Standards + Spec，每任务一轮，修复后复跑双绿）。

**已知限制**：

- **GUI 点击路径未自动化验证**（2026-08-10 真机时前台有全屏游戏遮挡窗口，置顶失败；按钮为 IPC 已验证命令的薄包装 + 构建 clean + 审查通过；产物 `cwd/pluginlab_timeline.json`）
- **UADx 系列不可测**：processBlock 抛未知异常（块 C 保护兜底，测量返回失败不崩宿主）；magic.CURVE 编辑器消息重入致静默退出——真机验收统一用 Pro-Q 4（UADx 1176/LA-2A 等加载 OK 但测量不可用）
- ~~回放进度为 spinner~~（已解决：2026-08-11 issue #2，GUI「事件 N/M」+ IPC 进度行）

## D 块完成记录（2026-08-11，块 4 D 进程外托管；计划 v2 见 git 历史 docs/archive/plan-block-d-out-of-process.md）

**commit 范围**：`d68b094`（D1+D3）→ `9771131`（D2）→ `7fbea02`（D6）→ `7370613`（ipc 客户端修复）→ `18079ef`（构建登记）→ `26fb746`（D 收尾文档，已 push origin/main）。

**设计门**（2026-08-10 D0，6 决策全部按推荐定案）：B+ 边界（load+measure 进子进程、扫描留宿主、仅黑名单插件走子进程）/ 编辑器 c 降级（Generic SetParent，v1 后置）/ 宿主唯一黑名单写者 / 崩溃恢复 3 次上限（kMaxScanHangs 语义）/ stdin-stdout JSON 行协议 / 子进程内录制 + 结果回传。ADR-D-1..7（stable-id 参数快照替代不可回灌的 captureParameterSnapshot / ExitProcess 注入必须跑子进程 / 类名 ChildProcessCoordinator 冲突改 PluginHostChildCoordinator / CreateProcess 不用 juce::ChildProcess / WAV 中转回传 / 宿主无 plugin 的 Context 构造 / 非 freq 黑名单插件安全降级）。

**关键交付**：

- **D1 子进程骨架**（d68b094）：`source/child/PluginHostChild.exe`（stdin/stdout JSON 行协议 start/load/heartbeat/stop/snapshot_params/restore_params，/EHa 加载保护，measure 期 progress 行防 3s 看门狗误杀）；`PluginHostChildCoordinator`（CreateProcess + 匿名管道，PeekNamedPipe 融合看门狗，**恰好一次**崩溃上报，restart()/crashCount()，崩溃路径句柄泄漏修复，handleLock 四方并发关闭互斥）。**真机**：子进程可启动、taskkill 强杀可检测、句柄 5 轮无泄漏。
- **D2 测量入子进程**（9771131）：子进程 measure 镜像宿主生成器选择（SineSweep 5s / MLS Impulse 16383），复用冻结 SweepRunner/CaptureBuffer（WAV 崩溃镜像 [dry,wet] 交织 24-bit，ADR-D-5）；宿主 `WavCaptureReader`（手写 24-bit 读取）+ `ChildWavAnalyzer`（无 plugin 分析入口，ADR-D-6 Context 从子进程回传元数据构造）。**真机 magic.CURVE**：sweep/MLS 双路径带内 3379 点 **max |ΔdB| = 0**（验收 <0.5dB）。
- **D3 崩溃恢复**（d68b094）：stable-id 参数快照/恢复（ADR-D-1，往返 max Δ=0）；崩溃 → 黑名单联动 + CrashLog → 自动重启 → 恢复快照 → 续测；连续崩溃 ≥3 停重启；`tests/SuicidePlugin` VST3 崩溃注入 fixture（crash_mode 参数 0 直通/1 ExitProcess/2 abort，ADR-D-2）。**真机 TestPlugin/SuicidePlugin**：崩溃→重启→续测成功，参数跨崩溃一致。
- **D6 双路径路由**（7fbea02）：`ChildMeasureContract`（冻结契约头）+ `ChildMeasureOrchestrator`（恢复序列 + crashCount-baseline 跨 run 累积 + ADR-D-7 非 freq 拒绝）+ CommandParser measure 黑名单路由（isBlacklistedPath → 子进程，无回调显式报错，白名单宿主直载零改动）+ **loadPlugin 黑名单寻址**（扫描跳过黑名单插件 → knownPlugins 无条目，按黑名单条目文件名/精确路径匹配合成 desc）+ Main.cpp 接线（onCrash 黑名单写 + setChildMeasurePath）。**真机**：loadPlugin "Pianoteq 9" → `{"ok":true,"blacklisted":true}`。
- **D4 Pianoteq 实战验收**（真机）：Pianoteq 9（`C:\Program Files\Common Files\VST3\Pianoteq 9`）注入黑名单条目 → loadPlugin 路由子进程 → measure → 子进程加载 Pianoteq 即被杀（heartbeat timeout 3000ms 检测）→ 自动重启 3 次上限 → `{"ok":false,"error":"child process crashed (restarting)"}`，**宿主存活**——roadmap 验收「Pianoteq 可加载测量不杀宿主」达成（黑名单条目经 pluginlist.xml 注入模拟其宿主杀手场景，验收后已还原）
- **预存缺陷修复**（7370613）：`tools/ipc_client.ps1` 由 .NET NamedPipeClientStream 改**原生 Win32 P/Invoke**（CreateFileA/WriteFile/ReadFile/CloseHandle）——.NET Dispose 不释放 OS 管道句柄致真机多命令序列卡死（服务器 ReadFile 永不返回，后续连接排队超时）；PipeServerTests 新增 R1 重连回归 + R2 shutdown 响应性锁定。

**测试**：**265/265 全绿**（208 + 块 D 新增 57：childcoordinator 11 + childprotocol 3 + childrestart 7 + childparity 2 + wavcapturereader 5 + childmeasure 6 + routing 5 + loadplugin-blacklist 5 + pipeserver 2 + 其余配套 11），`ctest --timeout 180` 双跑 + 真机验收 + 双轴审查（Standards + Spec）。

**已知限制**：

- **D5 编辑器 SetParent 后置**（v1 决策）：子进程 Generic 编辑器经 SetParent 显示到宿主未实施——AI 主路径不需要，黑名单插件无原生 UI 为可接受降级
- **非 freq 黑名单插件测量不可用**（ADR-D-7）：ChildWavAnalyzer 仅支持 frequency_response，harmonic/compression 黑名单插件返回 "child measurement not implemented for type X"——安全优先，绝不禁用黑名单隔离兜底宿主直载；子进程扩展 harmonic/compression 后自然解锁
- **Pianoteq 9 黑名单条目为注入**（本机装有 Pianoteq 9，但黑名单默认无其条目——验收经 pluginlist.xml 注入条目模拟宿主杀手场景，完成后已还原；子进程加载即杀子进程的宿主侧完整链路已验证）
- **hosted 子进程测量为同步阻塞**：黑名单插件 measure 期间 CommandParser 阻塞等待子进程（秒级），期间 stop 不可达——与宿主直测 measure 语义一致，协议面扩展留后续

## 开发流程标准化记录（2026-08-11，GitHub 分支/PR 工作流）

**决策**：仓库 `kurumi1ksllq/plugin-lab` 由 private 转 **public**（免费套餐下私有仓库的分支保护为 Pro 付费功能，HTTP 403；转 public 换取服务端强制分支保护）。main 分支启用完整保护，后续开发一律分支 + PR 合并。

**保护配置**（服务端强制，已生效）：

- **禁直接 push main**：`enforce_admins=true`（含 owner）；仅 PR 可合并
- **CI 门禁**：required status check `build-and-test`（`.github/workflows/build.yml`，Windows/MSVC，BUILD_TESTS=ON，ctest 连跑 2 次）；strict=true（分支需最新）
- **合并策略**：仅 squash（merge commit / rebase 已禁）+ required linear history；合并后自动删源分支；禁 force push / 禁删保护分支
- required approvals = 0（单人仓库，PR 本身即门槛，免自我审批摩擦）

**新增文件**：`.github/workflows/build.yml`（CI）、`.github/pull_request_template.md`（PR 模板：what/why/验证清单）；`.gitignore` 追加 `opencode.json`（本地 IDE MCP 配置，机器相关）。

**流程约定**：见根 AGENTS.md「WORKFLOW」节——`feat/*`/`fix/*`/`chore/*`/`docs/*` 分支 → Conventional Commits → `gh pr create` → CI 绿 → squash 合并。本地遗留分支 `feat/phase1-freq-response`、`feat/phase2-input-signal`（历史阶段已并入 main）未删除。

**CI 首跑即抓出隐藏缺陷**（2026-08-11，PR #1 验证流程时）：265 个测试中唯一带非 ASCII 字符（°）的测试名 `FreqResponse identity: dry==wet gives 0dB/0°` 在 CI 上 0.02s 挂掉——ctest 把测试名作过滤串传给测试二进制，° 的 UTF-8 字节在 runner（cp437/cp1252）往返损坏 → Catch2 `No test cases matched` → 测试未执行即判失败；本地 cp936 恰好保字节故长期 265/265 绿。修复：测试名改纯 ASCII（`0dB flat/zero phase`）+ tests/AGENTS.md 立约定。另将 JUCE 由 `GIT_TAG master` 钉到 release tag `9.0.1`（master 漂移致本地缓存与 CI 拉取不同版本，构建不可复现）。

## issue #2/#3 并发改造记录（2026-08-11，PipeServer 双轨并发模型）

**背景**：issue #2（playTimeline 回放进度）与 #3（hosted 子进程测量期间 stop 可达）共享同一个根——PipeServer 单线程同步 read→write，长命令阻塞期间读不了第二条命令。项目决策史（plan-scan-optimization.md）曾因「PipeServer 同步 read→write」否决推送式进度、改选快照式（getScanStatus），但快照式同样要求管道并发读，被同一个根卡住。Oracle 设计评审后定案。

**架构**（`source/ipc/PipeServer.*`，TDD 驱动）：

- **双轨并发**：控制命令（`stop`/`getScanStatus`，`setControlCommands` 配置）在管道读线程**内联**执行（只碰原子/快照）；**其余一切命令**（长命令 measure/playTimeline/dataset/scan + setParam 等快命令）FIFO 排队到**单个 worker 线程**串行执行——杜绝并发触碰 plugin/session 的竞态，同时让长命令期间读循环保持活跃。
- **写路径**：所有 WriteFile（内联/worker 最终/emitLine 进度行）经 `ioMutex` + 连接代数（generation）防写已关闭/复用句柄。
- **读路径关键坑**：阻塞 ReadFile 会被另一线程的并发 WriteFile **打断**（实测客户端 ERROR_PIPE_NOT_CONNECTED，误判断连）→ 读循环改 **PeekNamedPipe 轮询**（与 ChildProcessCoordinator 读者同模式）。
- **#3 子进程取消**：`PluginHostChildCoordinator::requestCancel()` 原子标志 → `ChildMeasureOrchestrator::waitForLine` 每轮检查 → 取消 = `coordinator->stop()` **主动弃子**（D3 本就每 run 全新子进程；主动 stop ≠ 崩溃，不增 crashCount、不触发黑名单/崩溃基线），返回 `{"ok":false,"error":"cancelled"}`。`CommandParser::setCancelRequestCallback`（Main.cpp 接线：session + orchestrator 双取消）。
- **#2 回放进度**：`ParameterTimeline::getPlaybackCursor/getPlaybackEventCount` + `MeasurementSession::setPlaybackProgressCallback`（block callback 中 cursor 前进即发，消息线程）→ GUI「事件 N/M」（~50ms 节流）+ IPC 推送 `{"ok":true,"progress":...,"event_index":N,"event_total":M,"time_ms":T}` 进度行（`Protocol::makeProgress` 复用，子进程先例）。
- **客户端**：`tools/ipc_client.ps1` 改 PeekNamedPipe 轮询多行读取（进度行/控制 ack 走 stderr，最终响应带 `samples`/`export_path`/`error` 走 stdout）+ `-CancelAfterMs` 连接内发 stop。

**验证**：270/270 测试双跑绿（新增 5：paramtimeline 游标 1 + orchestrator 取消 1 + pipeserver 并发 3）。新增测试曾抓出 PipeServer 读循环被并发写打断的实测 bug（err=233）。协议契约见 `source/ipc/AGENTS.md` + `SPEC.md` §10.1。
