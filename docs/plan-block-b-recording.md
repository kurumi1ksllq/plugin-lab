# PluginLab 块 3：B 记录模式工作文档

> 2026-08-08 制定。路线图总览见 `docs/roadmap-next.md` 块 3。**本块为任务分解工作文档**（开工前仍需 brainstorming 细化接口，roadmap 铁律"每块开工前单独规划"）。
> 现状核实：WAV mirror 已部分覆盖（CaptureBuffer flush 单文件交织 24-bit `[dry ch0..N-1, wet ch0..N-1]`，崩溃安全镜像）；`ParameterTimeline` / `WavExporter` 在 DESIGN.md §8.2 规划但 P2-13 显式延后未实现。

## 目标

把实验室从"测量"扩展到"记录"：可听对比的干/湿/基准多轨 WAV 导出、参数自动化录制-回放时间线，供 AI 与人工分析插件行为（不仅是频率/压缩曲线）。

## 任务分解（实施顺序）

### B1：WavExporter —— 干/湿/基准多轨 WAV（可听对比）

**现状缺口**：CaptureBuffer flush 已有单文件交织 24-bit `[dry, wet]`（measure/scan 崩溃安全镜像），但：
- 无独立基准（bypass）轨——对比"插件处理 vs 无插件"需要旁路测量
- 单文件交织声道数 = 2 × 插件声道，人耳听对比需拆轨或双文件
- flush 仅服务于崩溃安全，无"完整录制模式"语义（trim 后 finalise，但无独立导出 API）

**Files:**
- Modify: `source/capture/AudioBuffer.cpp/.h`（或新 `source/analysis/WavExporter.*`）
- Modify: `source/analysis/Export.cpp`（导出包可选带 WAV 引用）
- Modify: `source/ipc/Protocol.h` + `CommandParser.cpp`（导出/录制命令）
- Test: `tests/WavExporterTests.cpp`（新）
- Docs: `docs/data-schema.md`、`source/analysis/AGENTS.md`

**候选接口**（brainstorming 前草案）：
- `WavExporter::exportMultiTrack (const CaptureBuffer& dry, const CaptureBuffer& wet, const juce::File& dir, int pluginChannels)` → 三文件 `dry.wav / wet.wav / bypass.wav`（bypass 来自 dry 源或独立旁路测量）
- 或单文件多轨（`[dry L, dry R, wet L, wet R, bypass L, bypass R]`）——与现有 flush 布局一致

**验收**：干/湿/基准 WAV 时长一致、对齐正确（样本级比对）；用 TestPlugin（增益 2.0）录制后 wet = 2 × dry（样本级 ±1 LSB）；真机 Pro-Q 4 录制可听对比

### B2：ParameterTimeline —— 参数自动化录制 + 回放

**Files:**
- Create: `source/capture/ParameterTimeline.*`（新，深模块：录制/回放一体）
- Modify: `source/capture/MeasurementSession.cpp/.h`（录制模式接线）
- Modify: `source/ipc/Protocol.h` + `CommandParser.cpp`（`recordTimeline` / `playTimeline` 命令）
- Modify: `source/ui/`（GUI 面板）
- Test: `tests/ParameterTimelineTests.cpp`（新）
- Docs: `docs/data-schema.md`（timeline 导出结构）

**核心语义**：
- 录制：时间轴上 `{t_ms, param_id, value_normalized}` 事件序列（参数变化经 setValueNotifyingHost 时打点）+ 同步录制音频（干/湿）
- 回放：按时间线逐事件 setValueNotifyingHost，测量线程同步跑 sweep/录制
- 与扫描/测量串行化（复用 ScanEngine 的占用语义，roadmap R2）

**验收**：录制-回放参数时间线与输入一致（事件序列逐条比对）；回放期间音频录制干/湿正确；取消/恢复参数快照

### B3：新 capture 模式 + IPC 命令 + GUI 面板

- 新 MeasurementSession Type 或 Source 模式（如 `Source::timeline`）承载录制-回放
- IPC：`{"cmd":"record","type":"timeline",...}` / `{"cmd":"playTimeline",...}`（四件套）
- GUI：录制/回放按钮 + 时间线进度显示（复用 AsyncUpdater 节流铁律）
- 验收：IPC 驱动全流程（record → playTimeline → 导出 WAV/JSON），GUI 按钮等价路径

## 验收汇总（roadmap）

- 录制-回放参数时间线与输入一致
- 干/湿 WAV 时长/对齐正确

## 风险

| # | 风险 | 等级 | 缓解 |
|---|------|------|------|
| R1 | 录制模式与 sweep 管线并发冲突 | P1 | 串行化（复用 scan 的占用/排队语义） |
| R2 | ParameterTimeline 与既有 ScanEngine 快照/恢复语义纠缠 | P2 | 时间线自持 RAII 快照；brainstorming 定边界 |
| R3 | WAV 多轨布局改变破坏现有 flush 兼容 | P1 | 新导出 API 独立于 flush；flush 冻结不改 |
| R4 | GUI 面板工作量被低估 | P2 | IPC 优先交付，GUI 面板后置（不影响 AI 驱动主路径） |

## 开工前置（brainstorming 问题清单）

1. 录制模式是"新 Source"还是"新 Type"？（grTimeline 已占 dynamic 源；timeline 是时间轴参数驱动，倾向新 Source::timeline + 新 Type::timelineRecording）
2. WavExporter 布局：三文件 vs 单文件多轨？（人耳对比 vs AI 解析）
3. 回放是否实时（墙钟时间）还是尽可能快（测量语义）？
4. 录制时参数打点的分辨率（10ms？事件驱动？）
