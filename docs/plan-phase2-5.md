# PluginLab 阶段 2-5 开发计划（v2，两轮审查定稿）

> 2026-08-03 制定。第一轮：Momus（Plan Critic）严格审查（基于真实代码核验）→ P0/P1/P2 问题清单 + 修正计划。
> 第二轮：Sisyphus 执行者视角审查 → 补充 4 项落地细节（A/B/C/D）+ 1 项小项（E）。
> v2 为两轮审查整合定稿。用户确认后开始执行。每阶段：TDD + 实测验证 + push GitHub main。

---

## 审查结论（Momus，基于代码核验）

已核实的 8 项关键事实（影响计划正确性）：

| 事实                                                                                                                                                                    | 位置                                                  |
| ----------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ----------------------------------------------------- |
| `SignalGenerator::getTotalLength()` 返回 -1 表示无限；`generate` 全通道写同一信号                                                                                       | `SignalGenerator.h:25-31`                             |
| `SweepRunner::run()`：`totalLength <= 0` 时**静默回退 10s**；每块 `runDispatchLoopUntil(2)`；每轮 `releaseResources→prepareToPlay`；`result.prepare(2,bs)` 硬编码双声道 | `SweepRunner.cpp:54-56,94-97,45-49,8`                 |
| `progressCallback` 只传 `float`，**不传干/湿块**                                                                                                                        | `SweepRunner.h:35`                                    |
| measure 命令经 `callAsync + WaitableEvent + done.wait()` 同步执行；PipeServer 单线程"读-处理-写回"，**执行期间管道上 stop 不可达**                                      | `CommandParser.cpp:201-216`、`PipeServer.cpp:113-135` |
| latency 在 `run()` 之后只读一次                                                                                                                                         | `CommandParser.cpp:135`                               |
| `CaptureBuffer` 预分配 30s@48k + 动态扩容 + `trim()`                                                                                                                    | `AudioBuffer.h`                                       |
| GUI 测量按钮在消息线程直接 `handleCommand` 同步路径                                                                                                                     | `Main.cpp:96-115`                                     |
| **take01.wav 实测：48kHz / 16bit / stereo / 17.0s**（RIFF 头 28 字节 JUNK chunk，fmt 在 offset 48）                                                                     | 文件头解析                                            |
| 阶段 1 全部 33 测试；TestPlugin 目前只有 gain/latency/参数能力                                                                                                          | `tests/TestPlugin.h`                                  |

---

## ① 问题清单

### P0（阻塞，执行该阶段前必须解决）

**P0-1：FilePlayback 的采样率差异无处理方案**（阶段 2.1）
`prepare(sr,bs)` 传会话采样率（固定 48000）。take01.wav 实测 48k 无问题，但用户后续可能加载 44.1k 文件。草案只写"prepare 读 header/声道数"，没定义文件 SR ≠ 会话 SR 怎么办。
**方案**：`prepare` 时读文件 SR，若 ≠ 会话 SR 用 `juce::ResamplingAudioSource` + `AudioFormatReaderSource`，`setResamplingRatio(sessionSR/fileSR)` 实时重采样；同时把 `source_sample_rate`/`resample_ratio`/`duration_sec` 写入导出元数据（否则 AI 建模会误以为输入是 48k 原始文件）。v1 可选"拒绝加载"明确报错。

**P0-2：噪声生成器的 `getTotalLength()` 语义未定义**（阶段 2.2）
`SweepRunner.cpp:55-56` 对 `totalLength <= 0` 静默回退 10s。若噪声按"无限"（-1）实现会得到隐式、不可解释的 10s，违反"测量配置可复现"原则。
**方案**：`NoiseGenerator::setDuration(seconds)`，`getTotalLength()` 返回 `sampleRate*durationSec`（有限，**不要返回 -1**）；**固定种子 RNG**（`std::mt19937`，种子写入 JSON）保证可复现（AI 拟合需要输入可复现）。粉噪声：Paul Kellet 3 阶经济滤波器或 Voss-McCartney，同样固定种子。

**P0-3：GR 时间线缺干/湿对齐 + 尾部截断处理**（阶段 4.1）
插件延迟（Pro-C/Pro-Q 的 latency 随参数可变）使 wet 相对 dry 滞后 `latencySamples`。直接比 `RMS(dryBlock)/RMS(wetBlock)` 会把延迟误判为 GR；vocal 瞬态下 RMS 窗口错位会污染攻击段。另外 `SweepRunner` 在 generator 耗尽即停，**wet 尾部（latency 长度的输出余量）被截断**——扫频靠去卷积免疫，vocal/GR 会丢尾。
**方案**：`SweepRunner::setTailPadSamples(L)` 末尾补 L 个静音 dry 让 wet 尾部完整；GR 计算前**对齐**：`GR_dB = 20·log10(RMS_wet_aligned/RMS_dry_aligned)`（dry 平移 +L，wet 平移 -L）。验收：TestPlugin `setLatencySamples(100)` + `gain=0.5` → GR 恒定 -6.02dB、无边界毛刺。

**P0-4：扫描期间 IPC `stop` 不可达，进度不可见**（阶段 3.1）
measure 用 `callAsync + done.wait()`，PipeServer 单线程循环——**50s 扫描期间管道上的 stop 命令根本读不到**；IPC 客户端无法中断；GUI 按钮同路径也无法中途取消（只能等完）。`progressCallback` 只给 GUI，IPC 客户端收不到任何进度。
**方案**：v1 最简——扫描引擎每轮之间检查取消标志 + **GUI 增加停止按钮**直接 `session->cancel()`（消息线程可中断，因每块让出 2ms）。IPC 中断列为增强项：PipeServer 拆分"控制循环"与"执行"，measure/scan 改异步启动 + 立即回 `{"ok":true,"started":true}`，客户端轮询结果文件；进度走第二条管道。文档明确默认"stop 无效"。

### P1（重要，影响正确性/质量/耗时）

**P1-5：扫描每轮必须重读 latency**（阶段 3.1）
lookahead 压缩器/oversampling EQ 的 latency 随参数变化。现在 `run()` 后只读一次。扫描引擎必须在**每轮**读 `plugin->getLatencySamples()` 存入该档条目，且该档频响分析也要用该档 latency（`fr.setLatencySamples`）。验收：TestPlugin 扩展为"latency 随参数变"，扫描结果每档 latency 记录正确。

**P1-6：`scan` 命令与现有 `measure` 的关系需定义**（阶段 3.1）
**方案**：新增 `scan` IPC 命令（`{type, param_id, values[]（归一化 0-1）, path}`），响应 `{"ok":true,"runs":N,"export_path":...}`；内部**复用 `MeasurementSession::run()`**（每轮 setParam + setType + run + 分析 + 收集）；**不改动现有 measure 响应格式**（向后兼容）。`MeasurementSession::run()` 的 switch 目前同时管"信号类型"与"分析类型"——阶段 2.4 先拆为"source 决定信号、type 决定分析"二维，扫描才能组合。

**P1-7：扫描参数快照/恢复要落实（setValue 异步、双值记录）**

- 扫描前快照**全部**参数归一化值（复用 `captureParameterSnapshot` 逻辑）
- 每档 `setParam` 后**等一个 idle 周期**（`runDispatchLoopUntil(~20ms)`）——部分 VST3 的 setValue 自动化是异步生效的
- 恢复用 RAII（含取消/异常路径）
- 每档条目同时记录 `param_value_normalized` 和 `param_value_text`（`param->getText(value)`）——DESIGN §8.4 要求"归一化+实际值"，现在 snapshot 只有归一化值

**P1-8：GR 时间线需要逐块回调（`progressCallback(float)` 不够）**（阶段 4.1）
扩展为 `blockCallback(float progress, const dryBlock&, const wetBlock&)`，每块调用；GR 累加在回调内做（每块 2 次 RMS，微秒级），UI 刷新走 `AsyncUpdater` + 50ms 节流。现有 `SweepRunnerTests` 只测 cancel，扩展签名兼容成本低。

**P1-9：attack/release 估计算法需具体化**（阶段 4.2）

- attack：burst 起始沿，10%→90% 上升时间；或拟合 `GR(t)=GR_ss·(1−e^(−t/τ))`，瞬时 `τ = −Δt/ln(1−GR(t)/GR_ss)`，稳态段取均值
- release：burst 结束沿，`GR(t)=GR_ss·e^(−t/τ)`，`τ = −GR(t)/GR'(t)`（一阶差分）或 37% 剩余点
- **opto/vari-mu 关键**：τ 随电平/增益变化 → 产物不是单 τ 而是 **`τ_attack(level)` / `τ_release(level)` 曲线族**（按动态 GR 分 bin 统计）——这正是要喂给 AI 建模的量
- **测试前置**：TestPlugin 扩展 `TestCompressorPlugin`（processBlock 实现包络跟随器 + gain computer，attack/release/threshold/ratio 可配）→ 已知 τ 误差 <10% 的确定性测试。没有它 4.2 无法 TDD。

**P1-10：vocal 输入的分析路径缺失**（阶段 2.4/4 遗漏）
**方案**：阶段 4 引入 `AnalysisStrategy`：SignalGenerator 源 → 现有分析器；FilePlayback 源 → vocal 分析器（RMS/GR 时间线 + 逐帧频谱）。**显式指定**（命令里 `source=file, analysis=gr_timeline`），不能自动猜。

### P2（改进项）

- **P2-11：扫描耗时预算**：5s sweep×10 档=50s，实际 ×1.2（2ms/块让出）；双参数 10×10=100 轮≈10 分钟。建议扫描轮默认短信号（2s sweep/2s 噪声）；GUI 显示预计时长；文档写时间预算。
- **P2-12：PlotWidget 多曲线可读性**：需 HSL 均匀 10 色色板（检查 `PlotWidget.cpp` 当前颜色分配逻辑）；谐波曲线族用线图（freq×幅值每档一线）比柱状图叠加可读；GR 表头是时间轴（线性 X）、频响是对数 X——`setXAxisLog` 每图可切换（`PlotWidget.h:50` 已支持）。
- **P2-13：与 §8 定稿方案脱节**：§8.2 定稿 RecorderEngine/ParameterTimeline/AnalysisStrategy/WavExporter + "SweepRunner 不动"边界。**明确**：AnalysisStrategy 在阶段 4 引入；RecorderEngine 作为扫描协调器在阶段 5；WavExporter（用户要听干/湿对比）与 ParameterTimeline（记录模式参数自动化）**显式延后**，在 DESIGN.md 记录该决定，避免与 §8 冲突。
- **P2-14：素材管理**：take01.wav（3.1MB/17s）入库可接受；但**单元测试不依赖外部 wav**——测试里用 `juce::WavAudioFormat` 动态生成 1s 测试 wav 写临时目录，FilePlayback 测试自包含。
- **P2-15：扫描跨轮状态漂移**：每轮 `releaseResources→prepareToPlay` 重置多数插件状态，但个别插件（自适应 DSP、模拟建模 warmup）有漂移。缓解：首末轮同参数重测对比，差异超阈值时 JSON 记 `warning`。

### 二次审查补充（Sisyphus 执行者视角，v2 新增）

| #     | 优先级 | 问题                                                                                                                                                           | 补充方案                                                                                                                               |
| ----- | ------ | -------------------------------------------------------------------------------------------------------------------------------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------- |
| **A** | 🔴 P0  | **FilePlayback 缺"音频正确性验证"**：T2.1 测试只写读样本数/重采样时长，没验证"加载的文件真的被正确播放"。重采样/声道映射 bug 会悄悄污染所有 vocal 测量         | T2.1 增加冒烟测试：TestPlugin `gain=1.0`（旁路）时 `wet ≈ 原始 wav`（逐样本误差 < 0.001）                                              |
| **B** | 🟡 P1  | **阶段 2"vocal 全流程"分析空窗**：vocal 分析器（RMS/GR/频谱）是阶段 4 的 AnalysisStrategy 才引入，阶段 2 验收却写"跑通 vocal 全流程"——输出什么不界定会范围膨胀 | **明确**：阶段 2 vocal 验收 = "文件加载→回放→插件处理→干湿 capture→导出 raw 数据 + source 元数据"；**分析（RMS/GR/频谱）显式归阶段 4** |
| **C** | 🟡 P1  | **阶段 3 缺"扫描参数配置 UI"**：T3.3 只写多曲线对比，没写用户怎么配置扫描（选参数/填档位）                                                                     | T3.3 补扫描面板：参数下拉 + 档位编辑 + 预计时长显示；多曲线对比只是结果展示的一半                                                      |
| **D** | 🟡 P1  | **scan 命令客户端超时未处理**：scan 同步 60s，ipc_client.ps1 的 ReadLine 无超时保护会挂起；期间无进度反馈                                                      | ipc_client.ps1 加响应超时；scan 期间 GUI 显示进度（round N/M）                                                                         |
| **E** | 🟢 P2  | 阶段 3 验收 `<60s` 偏临界（10 档×5s=50s+开销≈60s）；长 vocal 文件（>30s）触发 CaptureBuffer 扩容 ~92MB/2 分钟                                                  | 验收用 2s 短信号（同步 P2-11）；文档提一句长文件内存                                                                                   |

---

## ② 修正后的详细阶段计划（v2 整合）

> 执行原则不变：每阶段 TDD（先写失败测试）+ 实测验证；每阶段 push main。

### 阶段 2：输入与信号增强（前置，3/4 的基础）

**T2.1 FilePlayback**（`source/signal/FilePlayback.h/cpp`）

- 实现 `SignalGenerator`：`prepare` 用 `AudioFormatManager`（注册 wav）+ `AudioFormatReader` 读头（SR/声道/长度）；**SR ≠ 会话 SR 时 `ResamplingAudioSource` 重采样**；记录 `sourceSampleRate/resampleRatio/durationSec`
- `generate`：`AudioFormatReaderSource` + 声道映射（mono 复制全通道；stereo 保持；>2 取 L/R；目标通道不足补 0）
- `getTotalLength()`：文件样本数（重采样后）；`reset()`：`setNextReadPosition(0)`
- `setTailPadSamples(L)`：SweepRunner 支持末尾补 L 静音 dry（**为阶段 4 预留**，P0-3）
- **测试**：
  1. 动态生成 1s 48k stereo wav（`juce::WavAudioFormat`）→ 读样本数正确、reset 重放一致、mono→stereo 映射、44.1k 文件重采样时长正确
  2. **🔴 补充 A（冒烟验证）**：TestPlugin `gain=1.0`（旁路）跑 FilePlayback → `wet` 逐样本 ≈ 原始 wav（误差 < 0.001）——**验证文件真的被正确播放**
- **验收**：vocal 17s 全流程 capture+分析 < 30s；JSON 含 source 元数据 `{type:"file", path, sample_rate, resample_ratio, duration}`

**T2.2 全频段信号**（`source/signal/NoiseGenerator.h/cpp`，WhiteNoise/PinkNoise）

- `setDuration(seconds)` + `getTotalLength()` 返回有限值（**不要返回 -1**，P0-2）；`setAmplitude`；`setSeed(uint32)` 默认固定种子
- 白：`std::mt19937` 均匀 [-1,1]；粉：Paul Kellet 3 阶 IIR
- **测试**：同一种子两次生成逐样本相等（可复现）；粉噪声 PSD 斜率 ≈ −3dB/oct（FFT 校验）；幅度不越界
- **验收**：同种子两次测量 dry 缓冲逐样本一致

**T2.3 动态信号生成器**（`source/signal/EnvelopeSignal.h/cpp`）

- 包裹内部 `SignalGenerator` 指针（载波：正弦/噪声/多音）+ 包络函数（ADSR 预设、正弦调制、指数调制可选）；输出 `out = carrier·env(t)`；`setSpeed` 缩放时间轴
- **所有权明确**：EnvelopeSignal 持有内部 generator（unique_ptr 成员），构造时传入；run() 生命周期管理同现有模式
- **测试**：指定包络形状逐样本断言；速度缩放后 `getTotalLength` 一致

**T2.4 输入源选择**

- 协议：`measure` 命令 JSON 加可选 `source` 字段（`signal|file|noise|dynamic`）+ 对应参数，默认 signal
- 重构 `MeasurementSession::run()`："type 决定分析"与"source 决定信号"分离为二维（P1-6 前置）
- GUI：输入源下拉 + 文件选择器（`FileChooser`）+ 噪声/动态参数面板；切换源时清空旧测量结果
- **测试**：CommandParserTests 覆盖 source 字段分支；GUI 手测
- **验收（🟡 补充 B 明确范围）**：阶段 2 vocal 验收 = 文件加载 → 回放 → 插件处理 → 干湿 capture → 导出 raw + source 元数据；**RMS/GR/频谱分析显式归阶段 4**（AnalysisStrategy 引入后）

**阶段 2 验收**：33 测试全绿 + 新增用例（含补充 A 的 FilePlayback 冒烟）；vocal 回放全流程可用；JSON 含 source 元数据。

### 阶段 3：参数连续扫描（依赖 2，与 4 可并行）

**T3.1 ScanEngine**（`source/scan/ScanEngine.h/cpp`）

- 输入：type、paramId、values[]（归一化）、path；每轮复用 `MeasurementSession::run()`
- 流程：快照全部参数 → for each value：`setParam` → 让出 20ms（P1-7）→ `run()` → **重读 latency（P1-5）** → 收集 `{value, latency, result}` → 恢复快照（RAII，含取消/异常路径）
- 分段：每轮间检查 cancel；progress 回调 `(round, roundProgress, totalRounds)`；GUI 停止按钮直接 cancel（P0-4）
- IPC：新增 `scan` 命令（同步响应；文档写明"期间 stop 不可达，用 GUI 停止"）
- **🟡 补充 D**：`ipc_client.ps1` 加响应超时（scan 最长等待）；scan 期间 GUI 显示进度（round N/M）
- **测试**：TestPlugin 扫 Gain 5 档 → 每档频响 gain 一致、参数恢复正确、cancel 中途 → 部分结果 + 参数已恢复、latency 随参数变时每档记录正确
- **验收（🟢 补充 E）**：Pro-Q 4 单参数 10 档扫描用 **2s 短信号** → 总时长 < 30s；JSON 可被 Python 直接解析

**T3.2 连续性 JSON**（`Export` 扩展）

- 结构：`{context, scan:{param_id, param_name, values[], seed...}, family:[{param_value_normalized, param_value_text, latency_samples, result}]}`；result 复用各分析器 JSON 结构；Context 含 source 元数据
- **测试**：ExportTests 校验 schema 往返

**T3.3 GUI 扫描面板 + 多曲线对比**

- **🟡 补充 C（扫描配置 UI）**：扫描面板 = 参数下拉（从 getParams 加载）+ 档位列表编辑 + 预计时长显示（档数 × 单轮时长）+ 扫描/停止按钮
- PlotWidget 加 HSL 10 色色板；扫描结果每档一个 Series；谐波曲线族用线图
- **验收**：Pro-Q 4 单参数 10 档扫描（2s 短信号）< 30s；GUI 10 条曲线可分辨；JSON 可被 Python 直接解析

### 阶段 4：动态压缩行为测量（依赖 2，与 3 可并行）

**T4.0 测试设施先行**：`TestCompressorPlugin`（包络跟随器 + gain computer，attack/release/threshold/ratio 可配）——4.1/4.2/4.3 全部测试的地基。

**T4.1 GR 时间线**（`source/analysis/GainReduction.h/cpp`）

- 前置输入：`SweepRunner::setTailPadSamples(latency)` + 干/湿平移对齐（P0-3）
- `SweepRunner` 扩展 `blockCallback(progress, dry, wet)` 每块调用（P1-8）
- GR 分析在回调内累加每块 RMS → `GR_dB(t)`；UI 用 AsyncUpdater 50ms 节流
- **测试**：TestPlugin gain=0.5 → −6.02dB；延迟 100 无毛刺；TestCompressorPlugin 已知 τ 响应正确

**T4.2 attack/release 估计**（`source/analysis/TimeConstants.h/cpp`，依赖 GainReduction）

- 事件沿标注（ToneBurst/EnvelopeSignal 提供已知时刻）：10%→90% + 单指数拟合 + 瞬时 τ 曲线；产物 **`τ_attack(level)` / `τ_release(level)` 曲线族**（P1-9）
- **测试**：TestCompressorPlugin 已知 τ → 估计误差 <10%

**T4.3 压缩响应曲线族**：EnvelopeSignal 控制电平/动态速度 → （静态 CompressionCurve + GR 时间线）→ 复用 3.2 的 family 结构，扫描维度化为"电平/动态速度"

**T4.4 GR 表头可视化**：PlotWidget 时间轴显示 GR(t) 实时刷新

**阶段 4 验收**：TestCompressorPlugin 全链路；动态信号 GR 时间线 τ 误差 <10%；Pro-C 3 实测 GR 曲线合理（attack 在 µs~ms 级）；vocal 输入经 AnalysisStrategy 出 RMS/GR 时间线（P1-10，🟡 补充 B 的"分析归此"兑现）

### 阶段 5：建模与数据整合（依赖 3+4）

**T5.1 数据整合**：ScanResult + GR 曲线族 → 建模数据包（统一结构：param × level × freq × time 维度）
**T5.2 数据包输出**：完整 JSON（context + source + scan 元数据 + family + GR 时间线 + 拟合建议）；文档化 schema
**T5.3 反推验证**：Pro-C 3/Pro-Q 4 实测 → 反推参数 → 与 `getParams` 实际值对比，输出验证报告
**验收**：Pro-Q 4 扫频扫描反推频点/Q/增益落在设定容差内；schema 文档化

---

## ③ 依赖图

```
                     ┌──────────────────────────────┐
                     │  阶段 2：输入与信号增强        │
                     │  T2.1 FilePlayback(重采样/声道映射/tail pad/旁路冒烟) │
                     │  T2.2 噪声(白+粉,固定种子)    │
                     │  T2.3 EnvelopeSignal          │
                     │  T2.4 source 选择 + run() 二维化 │
                     └──────────────┬───────────────┘
              ┌────────────────────┼────────────────────┐
   ┌──────────▼──────────┐  ┌──────▼──────────┐  ┌──────▼──────────┐
   │ 阶段3：参数扫描      │  │ 阶段4：动态压缩   │  │ (记录模式,延后)  │
   │ T3.1 ScanEngine     │  │ T4.0 TestCompressorPlugin │  │ WavExporter     │
   │  (依赖2.4/2.1)      │  │ T4.1 GR时间线(依赖2.3/2.1 tail pad) │  │ ParameterTimeline│
   │ T3.2 连续性JSON     │  │ T4.2 τ估计(依赖4.1)  │  └─────────────────┘
   │ T3.3 扫描面板+多曲线 │  │ T4.3 压缩响应族(依赖4.1) │
   └──────────┬──────────┘  │ T4.4 GR表头(依赖4.1+3.3可选) │
              │             └──────────┬──────────┘
              └────────────┼──────────┘
                        ┌──▼──────────┐
                        │ 阶段5：建模整合 │
                        │ T5.1 数据整合(依赖3.2+4.2) │
                        │ T5.2 数据包(依赖5.1) │
                        │ T5.3 反推验证(依赖5.2+2.1) │
                        └─────────────┘
```

- **关键路径**：`2 → max(3,4) → 5`；阶段 3 与 4 相互独立可并行（3 用多轮串行测量，4 用动态信号+新分析器）
- **阶段内顺序**：4.0 先于 4.1/4.2/4.3；4.2 依赖 4.1；4.3 依赖 4.1；4.4 依赖 4.1（UI 部分可复用 3.3 色板工作）
- **阶段 3 内**：3.1 → 3.2 → 3.3 严格串行（3.3 需要 family 数据）

---

## 审查历程

1. **v1（Momus 审查）**：草案阶段划分和顺序正确，但缺 4 个 P0（采样率重采样、噪声时长语义、GR 干湿对齐、扫描中断/进度）、6 个 P1 实现细节（其中"TestCompressorPlugin 测试设施"是阶段 4 能否 TDD 的前提）。
2. **v2（Sisyphus 执行者审查）**：Momus 全部有效；补充 4 项落地细节——A. FilePlayback 音频正确性冒烟验证（P0）；B. 阶段 2 vocal 验收范围界定（分析归阶段 4）；C. 阶段 3 扫描配置 UI；D. scan 客户端超时与进度；E. 扫描验收改短信号 + 长文件内存提示。

**一句话结论**：按 v2 计划执行即可——两轮审查覆盖了方法论（P0/P1/P2）与落地细节（A-E），计划可直接作为各阶段实施依据。
