# Capture — 采集/测量管线

**位置:** `source/capture/`（6 文件）· **模块角色:** 测量编排层

## OVERVIEW

测量编排中枢：`MeasurementSession` 协调 `SweepRunner` 执行 generate→process→capture，产出 4 类测量数据（CaptureBuffer 存 dry/wet）。

## PIPELINE（SweepRunner，冻结）

generate → process → capture 三段式 I/O 管线，按 block 循环直至完成：

- generate：信号生成器产 dry block
- process：插件处理
- capture：录音 dry/wet → CaptureBuffer
- 运行于测量线程；期间 yield 消息循环（`runDispatchLoopUntil`，`JUCE_MODAL_LOOPS_PERMITTED=1`）

## MeasurementSession API

### Type × Source 矩阵

| Type \ Source     | signal       | file | noise | dynamic |
| ----------------- | ------------ | ---- | ----- | ------- |
| frequencyResponse | ✓            | -    | -     | -       |
| harmonicAnalysis  | ✓            | -    | -     | -       |
| compressionCurve  | ✓            | -    | -     | -       |
| grTimeline        | ✗（拒绝）    | ✓    | ✓     | ✓       |

- 分析（freq/谐波/压缩）仅 `Source::signal`；`grTimeline` 仅非 signal 源有效（signal 源被 CommandParser 拒绝，`"gr_timeline requires a non-signal source"`），由 dry/wet 反推 GR 时间线 + attack/release τ

### 生命周期

create → configure → run → read result → destroy。`run()` 阻塞至完成，失败/取消返回 false；`cancel()` 线程安全（委托 `runner.cancel()`）。

### 关键 setter

- `setSampleRate`（默认 48000.0）/ `setBlockSize`（默认 512）
- `setSource` / `setFilePath` / `setNoiseConfig(type, durationSec, seed)`
- `setDynamicCarrierFreq/Amplitude/Speed/ADSR/CarrierStartHz`；dynamic 默认参数精确复刻原 signal，未调用前不影响现有命令
- `captureParameterSnapshot`：记录测量时参数值供导出
- `setProgressCallback`（0.0-1.0）；`setBlockCallback`（T4.4 live GR 头）：每 block 回调总进度 + dry/wet block，透传 SweepRunner

## THREADING RULES

- `prepareToPlay`/`processBlock` 必须在测量线程（`SweepRunner::run`）执行
- 测量经 WaitableEvent + `MessageManager::callAsync` 派发到消息线程（Pro-Q 4 要求与编辑器同线程）
- CommandParser 同步路径检查 `isThisTheMessageThread()` 防测试挂死

## FROZEN BOUNDARY

**SweepRunner 不改**（依据 DESIGN.md §8.2；**唯一例外**：块 C 任务 1 授权的测量路径异常保护——`run()` 内 plugin 调用（prepare/process/teardown）加 try/catch + CRASH_LOG + 失败返回 false，TU 开 /EHa；语义不变，仅防插件异常逃逸消息循环。详见 source/AGENTS.md /EHa 约定）：

- 测量目的感知放 MeasurementSession，为什么测放上层
- prepareToPlay 统一并入 `SweepRunner::run`（Phase-1 bug：loadPlugin 线程 + sweep 线程双调用致 Pro-Q 4 崩溃）
- 动态声道分配 `getTotalNumInput/OutputChannels`（Phase-1 bug：硬编码 2ch 溢出 4-in/4-out 插件）

## KNOWN P0 ISSUES

- ~~长采集应增量 flush，当前全内存驻留（插件崩溃→全丢）~~ → ✅ **已解决**：flush 机制落地 CaptureBuffer（见下「INCREMENTAL WAV FLUSH」）并**已接入 IPC**（CommandParser measure/scan 运行前调用 `setFlushConfig`，响应带 `wav_path`）

## INCREMENTAL WAV FLUSH

CaptureBuffer 采集期间把 dry/wet 增量镜像到 24-bit PCM WAV（崩溃安全镜像；分析仍用内存 buffer）。

- **API**：`CaptureBuffer::setFlushConfig(const juce::File& wavPath, double intervalSec)`——采集前调用；空 wavPath 禁用（默认关闭）
- **格式**：单文件交织 24-bit PCM，声道 = 2 × 插件声道，布局 `[dry ch0..N-1, wet ch0..N-1]`（立体声插件 → 4 声道）；44 字节 RIFF 头，byteRate/blockAlign 标准
- **间隔**：由调用方传 `intervalSec`（采样数 = sampleRate × intervalSec）；崩溃最多丢一个间隔
- **手写 writer 理由**：`juce::WavAudioFormatWriter` 只在析构时 finalise RIFF 头，崩溃后文件不可读；本实现每 flush 边界 patch 头尺寸 + 落盘（无缓冲 FileOutputStream），任意时刻文件有效
- **生命周期**：首次 flush 惰性建流（先 deleteFile 清旧文件）→ 每间隔 flush → `trim()`/`clear()` 时 finaliseWav 收尾并回填最终尺寸
- **冻结边界**：flush 全在 CaptureBuffer 内部（append 同步路径），**SweepRunner 未动**；路径/间隔由上层传入——CommandParser 经 `wavPathFor(exportPath)`（`.json→.wav`）+ `kDefaultFlushIntervalSec`(5s) 在 measure/scan 运行前配置
