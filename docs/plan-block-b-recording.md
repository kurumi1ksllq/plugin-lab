# PluginLab 块 3：B 记录模式实施计划

> 2026-08-08 制定（v2 细化版）。路线图总览见 `docs/roadmap-next.md` 块 3。执行顺序：E（块 1）→ B（块 3）→ D（块 4）。
> **brainstorming 草案答案已内嵌**（每任务 "DECISION" 标注，开工时用户确认即可；有异议则按 roadmap 铁律先访谈定案）。
> 现状核实：WAV mirror 已部分覆盖（CaptureBuffer flush 单文件交织 24-bit `[dry ch0..N-1, wet ch0..N-1]`，崩溃安全镜像，**冻结不改**）；`ParameterTimeline` / `WavExporter` 在 DESIGN.md §8.2 规划但 P2-13 显式延后未实现。

## 目标

把实验室从"测量"扩展到"记录"：可听对比的干/湿/基准多轨 WAV 导出（WavExporter）、参数自动化录制-回放时间线（ParameterTimeline）、新 capture 模式 + IPC 命令 + GUI 面板。

## 全局约束（每任务隐式包含）

- 构建：`cmake -S . -B build && cmake --build build --config Release`（/W4 /WX）
- 测试：ctest 全绿连跑 2 次；新增 .cpp 双编译
- 改协议：四件套（Protocol.h / CommandParser / tests / docs/data-schema.md）
- 线程铁律：processBlock 测量线程；编辑器消息线程；UI 刷新 AsyncUpdater 节流
- 串行化：录制/回放与扫描/测量互斥（复用 ScanEngine 占用语义，roadmap R2）
- 提交：每任务一 commit + push origin main

---

## 任务 B1：WavExporter —— 干/湿/基准多轨 WAV（可听对比）

**DECISION（草案，待确认）**：
- 布局：**单文件 6 声道交织** `[dryL, dryR, wetL, wetR, bypassL, bypassR]`（立体声插件），24-bit PCM——与 CaptureBuffer flush 布局同族，AI/工具解析一致；人耳对比用工具拆轨（tools/ 提供 split 脚本）
- 基准轨来源：**bypass = dry 副本**（v1 语义：基准轨即输入参考；真实插件 bypass 参数遍历为 v2）
- 与 flush 的关系：**独立新模块**，不动 CaptureBuffer flush（崩溃安全镜像语义保留）

**Files:**
- Create: `source/analysis/WavExporter.h/.cpp`（新，深模块：导出三轨一体）
- Modify: `source/capture/MeasurementSession.h/.cpp`（run() 可选旁路轨录制：plugin bypass 状态）
- Modify: `source/ipc/Protocol.h` + `CommandParser.cpp`（`exportWav` 命令）
- Create: `tests/WavExporterTests.cpp`（新）
- Docs: `docs/data-schema.md`（wav 导出字段）、`source/analysis/AGENTS.md`

**Interfaces:**
- `static bool WavExporter::exportTracks (const juce::AudioBuffer<float>& dry, const juce::AudioBuffer<float>& wet, double sampleRate, const juce::File& wavPath)`——单文件 24-bit 交织 `[dry ch0..N-1, wet ch0..N-1, dry ch0..N-1]`（bypass=输入副本），返回是否成功
- `void MeasurementSession::setRecordBypass (bool)`——录制时读插件 bypass 参数；默认 false（v1 不启用真实 bypass，恒 dry 副本）
- Protocol：`{"cmd":"exportWav","path":"<json 路径 .json→.wav>"}` → `{ok, wav_path}`——从最近一次测量的 CaptureBuffer 导出

### Step 1（RED）：WavExporterTests

```cpp
// tests/WavExporterTests.cpp
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include "../source/analysis/WavExporter.h"
#include <juce_audio_formats/juce_audio_formats.h>

// Helper: build a deterministic stereo dry buffer + wet = 2 × dry.
static juce::AudioBuffer<float> makeSignal (int numSamples, double gain)
{
    juce::AudioBuffer<float> buf (2, numSamples);
    for (int ch = 0; ch < 2; ++ch)
        for (int i = 0; i < numSamples; ++i)
            buf.setSample (ch, i, (float) (gain * std::sin (2.0 * juce::MathConstants<double>::pi * 440.0 * i / 48000.0)));
    return buf;
}

TEST_CASE ("WavExporter: exported file round-trips dry/wet/bypass exactly",
           "[wavexporter]")
{
    // Arrange — 1 s stereo, wet = 2× dry
    const auto dry = makeSignal (48000, 0.5);
    juce::AudioBuffer<float> wet (2, 48000);
    for (int ch = 0; ch < 2; ++ch)
        for (int i = 0; i < 48000; ++i)
            wet.setSample (ch, i, dry.getSample (ch, i) * 2.0f);

    const auto tmp = juce::File::getSpecialLocation (
        juce::File::SpecialLocationType::tempDirectory)
        .getNonexistentChildFile ("pluginlab_wav_export_test_", ".wav");

    // Act
    REQUIRE (WavExporter::exportTracks (dry, wet, 48000.0, tmp));

    // Assert — 6 channels, 24-bit, correct layout
    juce::AudioFormatManager mgr;
    mgr.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader (mgr.createReaderFor (tmp));
    REQUIRE (reader != nullptr);
    REQUIRE (reader->numChannels == 6);
    REQUIRE (reader->sampleRate == 48000.0);
    REQUIRE (reader->lengthInSamples == 48000);

    juce::AudioBuffer<float> readBack (6, 48000);
    reader->read (&readBack, 0, 48000, 0, true, true);

    // dry ch0 = 0, wet ch0 = 2, bypass ch0 = 4
    for (int i = 0; i < 48000; i += 1000)
    {
        REQUIRE (readBack.getSample (0, i) == Catch::Approx (dry.getSample (0, i)).margin (2e-4));
        REQUIRE (readBack.getSample (2, i) == Catch::Approx (wet.getSample (0, i)).margin (4e-4));
        REQUIRE (readBack.getSample (4, i) == Catch::Approx (dry.getSample (0, i)).margin (2e-4));
    }

    // Cleanup
    tmp.deleteFile();
}
```

### Step 2：RED → Step 3（GREEN）：实现

```cpp
// WavExporter.cpp 要点（手写 RIFF writer 参照 CaptureBuffer flush 的 24-bit
// 布局，但导出完整文件一次性 finalise——非崩溃场景，无增量 patch 需求）:
//   声道数 = 3 × numChannels（dry, wet, bypass=dry）
//   24-bit PCM，44 字节 RIFF 头，byteRate/blockAlign 标准
//   写完全部样本后回填 dataSize——与 CaptureBuffer::writeWavHeader 同风格
// 错误处理：文件创建失败 / 写入失败 → 返回 false + CRASH_LOG_WARN（无异常）
```

### Step 4：IPC 四件套 + CommandParserTests

```cpp
// Protocol.h:
    constexpr auto exportWav = "exportWav";
// CommandParser case（复用 resolveExportPath 语义，.json→.wav）:
//   wavPathFor(exportPath) 已有 —— 直接复用；从 session->getResult()
//   （dry/wet）导出；成功 → {ok, wav_path}
TEST_CASE ("CommandParser: exportWav writes a 6-channel wav from last measurement",
           "[commandparser][exportWav]") { /* 见 E 块测试模式 */ }
```

### Step 5：真机验收

IPC 全流程：measure frequency_response（Pro-Q 4）→ exportWav → tools 拆轨脚本验证 6 声道 + 时长 = 测量时长；人耳可听对比（可选）。

### Step 6：全量 ctest + 提交

```bash
git add source/analysis/WavExporter.cpp source/analysis/WavExporter.h source/capture/MeasurementSession.cpp source/capture/MeasurementSession.h source/ipc/Protocol.h source/ipc/CommandParser.cpp tests/WavExporterTests.cpp tests/CommandParserTests.cpp tests/CMakeLists.txt CMakeLists.txt docs/data-schema.md source/analysis/AGENTS.md
git commit -m "feat(analysis): WavExporter dry/wet/bypass multitrack export (IPC exportWav)"
git push origin main
```

---

## 任务 B2：ParameterTimeline —— 参数自动化录制 + 回放

**DECISION（草案，待确认）**：
- 数据结构：`{t_ms, param_id, value_normalized}` 事件序列（**毫秒时间戳 + 稳定 param_id**——与 getParams 的 param_id 一致，AI 可直接寻址）
- 录制：插件参数变化经 `AudioProcessorListener::parameterChanged` 回调打点（挂 listener 到插件）；时间基准 = 录制起始的墙钟时间
- 回放：按时间线逐事件 `setValueNotifyingHost`，测量线程同步跑 sweep/录制（回放 = 激励源的一部分）
- 串行化：回放独占测量通道（复用 ScanEngine 的占用语义）

**Files:**
- Create: `source/capture/ParameterTimeline.h/.cpp`（新）
- Modify: `source/capture/MeasurementSession.h/.cpp`（timeline 回放源接线）
- Modify: `source/ipc/Protocol.h` + `CommandParser.cpp`（`recordTimeline` / `playTimeline` 命令）
- Create: `tests/ParameterTimelineTests.cpp`（新）
- Docs: `docs/data-schema.md`（timeline 导出结构）

**Interfaces:**
```cpp
struct TimelineEvent
{
    int64_t timeMs = 0;            // ms since record start
    juce::String paramId;          // stable id (matches getParams)
    float valueNormalized = 0.0f;  // 0..1
};

class ParameterTimeline
{
public:
    // ---- recording ----
    void startRecording (juce::AudioPluginInstance* plugin);  // attach listener
    std::vector<TimelineEvent> stopRecording();               // detach + return events
    bool isRecording() const;

    // ---- playback ----
    void setPlayback (std::vector<TimelineEvent> events, double playbackRate);
    /** Apply all events with timeMs <= nowMs (relative to playback start).
     *  Returns the number applied. Call from the measurement thread between
     *  blocks. */
    int applyEventsUpTo (int64_t nowMs, juce::AudioPluginInstance* plugin);

    // ---- state ----
    void clear();
private:
    // listener 实现（AudioProcessorListener）：
    //   parameterChanged (int index, float value) → 查 param_id → 打点
};
```

### Step 1（RED）：ParameterTimelineTests

```cpp
TEST_CASE ("ParameterTimeline: recording captures param changes with ids",
           "[paramtimeline][record]")
{
    TestPlugin plugin;
    plugin.addTestParameter ("drive", "Drive", 0.0f);
    plugin.addTestParameter ("mix", "Mix", 0.5f);

    ParameterTimeline tl;
    tl.startRecording (&plugin);

    plugin.getParameters()[0]->setValueNotifyingHost (0.7f);
    juce::Thread::sleep (30);
    plugin.getParameters()[1]->setValueNotifyingHost (0.2f);

    auto events = tl.stopRecording();

    REQUIRE (events.size() == 2);
    REQUIRE (events[0].paramId == "drive");
    REQUIRE (events[0].valueNormalized == Catch::Approx (0.7f));
    REQUIRE (events[1].paramId == "mix");
    REQUIRE (events[1].valueNormalized == Catch::Approx (0.2f));
    REQUIRE (events[1].timeMs >= events[0].timeMs);   // monotonic
}

TEST_CASE ("ParameterTimeline: playback applies events up to a timestamp",
           "[paramtimeline][playback]")
{
    TestPlugin plugin;
    plugin.addTestParameter ("drive", "Drive", 0.0f);

    ParameterTimeline tl;
    tl.setPlayback ({{ 0, "drive", 0.3f }, { 500, "drive", 0.9f }}, 1.0);

    REQUIRE (tl.applyEventsUpTo (100, &plugin) == 1);
    REQUIRE (plugin.getParameters()[0]->getValue() == Catch::Approx (0.3f));
    REQUIRE (tl.applyEventsUpTo (600, &plugin) == 1);
    REQUIRE (plugin.getParameters()[0]->getValue() == Catch::Approx (0.9f));
    REQUIRE (tl.applyEventsUpTo (900, &plugin) == 0);   // nothing new
}

TEST_CASE ("ParameterTimeline: playback rate scales timestamps", "[paramtimeline][playback]")
{
    // rate 2.0 → events at t=500ms apply at wall t=250ms
}
```

### Step 2-4：RED → GREEN → 全量测试（实现要点见 Interfaces）

### Step 5：MeasurementSession 接线（timeline 回放源）

```cpp
// MeasurementSession.h:
    void setTimelinePlayback (std::vector<TimelineEvent> events, double rate);
// run() 内 Source::dynamic 或新 Source::timeline 分支：
//   激励 = 现有 sweep/dynamic 信号 + 每 block 前 applyEventsUpTo(nowMs)
//   （与 scan 串行：CommandParser 层保证 playTimeline 独占测量通道）
```

### Step 6：IPC 四件套

```cpp
// Protocol.h:
    constexpr auto recordTimeline = "recordTimeline";
    constexpr auto playTimeline   = "playTimeline";
// recordTimeline: {"cmd":"recordTimeline","path":"<json>"} → 开始录制参数事件
//   + 同步录干/湿音频；stop → 导出 timeline JSON + WAV（复用 B1）
// playTimeline: {"cmd":"playTimeline","path":"<timeline.json>","rate":2.0}
//   → 回放并录制音频
// CommandParserTests：录制打点/回放应用/参数恢复 3 用例
```

### Step 7：真机验收

IPC：loadPlugin Pro-Q 4 → recordTimeline → setParam 数次（模拟 AI 调节）→ stop → 导出 timeline JSON（事件序列正确）→ playTimeline 回放 → 录制 WAV 时长 = 回放时长、参数最终值与时间线尾一致。

### Step 8：提交

```bash
git add source/capture/ParameterTimeline.cpp source/capture/ParameterTimeline.h source/capture/MeasurementSession.cpp source/capture/MeasurementSession.h source/ipc/Protocol.h source/ipc/CommandParser.cpp tests/ParameterTimelineTests.cpp tests/CommandParserTests.cpp tests/CMakeLists.txt CMakeLists.txt docs/data-schema.md source/capture/AGENTS.md source/ipc/AGENTS.md
git commit -m "feat(capture): ParameterTimeline record/playback (IPC recordTimeline/playTimeline)"
git push origin main
```

---

## 任务 B3：新 capture 模式 + IPC 命令 + GUI 面板

**DECISION（草案，待确认）**：录制-回放作为**新 Source 轴**（`Source::timeline`），Type 复用 frequencyResponse/compressionCurve（回放时测什么由 Type 决定）；GUI 面板后置（R4——AI 驱动主路径不依赖 GUI）。

- IPC：`recordTimeline` / `playTimeline` 已在 B2 落地；B3 补 `Source::timeline` 的 measure 兼容（`"source":"timeline"` 需先 playTimeline 注入事件）
- GUI：录制/回放按钮 + 时间线进度条（AsyncUpdater ~50ms 节流，禁每 block 刷 UI）；回放进度显示当前事件序号
- 验收：IPC 驱动全流程（record → playTimeline → 导出 WAV/JSON）；GUI 按钮等价路径（人工可选验证）

---

## 验收汇总（roadmap）

- 录制-回放参数时间线与输入一致（B2 测试逐事件比对）
- 干/湿 WAV 时长/对齐正确（B1 测试样本级 + 真机时长核对）

## 风险

| # | 风险 | 等级 | 缓解 |
|---|------|------|------|
| R1 | 录制模式与 sweep 管线并发冲突 | P1 | 串行化（复用 scan 的占用/排队语义）；CommandParser 层独占测量通道 |
| R2 | ParameterTimeline 与 ScanEngine 快照/恢复语义纠缠 | P2 | 时间线自持 RAII 快照；playback 结束恢复录制前参数 |
| R3 | WAV 多轨布局改变破坏现有 flush 兼容 | P1 | WavExporter 独立新模块；flush 冻结不改 |
| R4 | GUI 面板工作量被低估 | P2 | IPC 优先交付，GUI 后置（不影响 AI 驱动主路径） |
| R5 | AudioProcessorListener 回调线程（参数变化可能非测量线程） | P1 | 打点用原子时间戳 + 互斥事件队列；stopRecording 时归并排序 |

## 开工前置确认（brainstorming 遗留，开工时逐条确认）

1. B1 布局：单文件 6 声道 vs 三文件？（草案：单文件，与 flush 同族）
2. B1 基准轨：bypass = dry 副本 vs 真实 bypass 参数遍历？（草案：v1 副本）
3. B2 回放速率：墙钟实时 vs 尽可能快？（草案：可配置 rate，默认 1.0 实时）
4. B3 Source::timeline 是否进 measure 协议（还是仅 playTimeline 专用）？（草案：专用命令，不进 measure source 枚举）
