# analysis（分析器 + JSON 导出层）

**生成:** 2026-08-03 · **规模:** 16 文件（6 分析器 × .h/.cpp + Export.h/.cpp + WavExporter.h/.cpp，Export 23 符号为最大）

## OVERVIEW

6 分析器 + Export 层：原始捕获 → 领域结果 → 8 类 schema JSON。

## ANALYZERS

| 分析器            | 输入 → 输出                                                                      | 喂给                                   |
| ----------------- | -------------------------------------------------------------------------------- | -------------------------------------- |
| FreqResponse      | Farina 反卷积（sweep→IR→FFT→幅度比+相位）→ 点列 (freq/mag/phase)；`analyzeMLS` 为 MLS 激励频域除法（周期谐波处直接 DFT，去插件预热瞬态） | freq 测量、EQ 曲线                     |
| HarmonicAnalysis  | MultiTone 8 基频（100–12800Hz）逐基频 → THD% + H2..H5 各次谐波 % | 谐波柱状图 |
| CompressionCurve  | 输入 dB vs 输出 dB → {input_dB, output_dB, gr_dB}[] + 拟合参数（压缩比/拐点/GR） | 压缩曲线                               |
| GainReduction     | 每 block 20·log10(RMS_wet/RMS_dry) → 逐 block GR                                 | 实时 GR 表头（50ms 节流）+ gr_timeline |
| TimeConstants     | τ（attack/release）估计 → τ + .valid 标志                                        | 时间常数显示                           |
| CompressionFamily | 输入电平 × 速度网格 → 每格压缩曲线 + GR 时间线                                   | compression_family 网格（阶段 4 已交付） |

### 要点

- **THD/IMD 勿混用**：THD 单音、IMD 多音，混用则谐波峰交叠（DESIGN.md:102）。注意：`harmonicAnalysis` 当前实现用 MultiTone 八度基频（100/200/.../12800 Hz，MeasurementSession.cpp:97），逐基频独立测谐波+THD——低频基频谐波会落在高频基频上（已知取舍）。
- **FreqResponse**：必须 Farina 反卷积，勿直接 FFT 比（低/高频相位噪声，Oracle 修复）。`analyzeMLS`（块 E 任务 1）例外：MLS 整段频域除法，**在 MLS 周期谐波频率 q·sr/N 处直接 DFT 求 H=Y/X**——2 的幂 FFT bin 与谐波不重合（32768 ≠ 2·16383，高频漂移 ~0.4 bin 致 0.5 dB 级偏差），故不用 FFT 采样；双周期录音分析第二个（稳态）周期，插件预热瞬态不污染；phase unwrap+latency 补偿与 octave 平滑与 H1 路径共用 `applyPhasePost`/`applySmoothing`（行为保持重构，既有 [freqresponse] 用例锁定）。
- **TimeConstants**：动态源 τ 有效（957e597 暴露 `carrier_start_hz` 默认 10000 + GainReduction 1ms RMS 窗口 + 正 dB 副本估计）；file/noise 源无边沿 → tau.valid=false 属设计。
- **GainReduction**：实时 GR 表头走 AsyncUpdater ~50ms 节流，勿每 block 刷 UI。

## EXPORT LAYER

- 手写 JSON：raw string literal + escapeJsonString；juce::JSON::toString 已弃用（引号转义 bug，pluginName.quoted() 不转内部引号，Oracle P0-4）。
- datasetToJSON 聚合 scan 族 / gr_timeline / compression_family 为单个 Dataset 包；appendDatasetScanFamily 辅助函数。
- 既有导出函数不变，body-equiv 测试锁等价性。

## WAV EXPORT（WavExporter，块 B 任务 1）

- **角色**：把**内存中的 dry/wet 录音**（来自 MeasurementResults 底层 CaptureBuffer）导出为单个 24-bit PCM WAV，供 AI 拿 dry/wet 双路参照反推插件处理方式。纯离线导出，不触发测量、无状态。
- **接口**：`WavExporter::exportTracks(dry, wet, sampleRate, wavPath)`——namespace 风格深模块，单函数入口。
- **布局**：3 × dry 声道交织 `[dry ch0..N-1, wet ch0..N-1, dry ch0..N-1]`（立体声 → 6 声道）；**bypass = dry 副本（v1）**；`wet.getNumChannels() < dry` → false。
- **格式**：44 字节手写 RIFF 头 + 24-bit PCM（`jlimit(-1,1,sample) * 8388607`，小端 3 字节）——量化与 CaptureBuffer 增量镜像逐位一致（镜像 AudioBuffer.cpp writeWavHeader/flush 风格）；先算尺寸后写真实头（全内存，无占位回填）。
- **错误**：文件创建/写入失败 → CRASH_LOG_WARN（含路径）+ return false；无 C++ 异常。
- **接线**：IPC `exportWav` 命令（CommandParser.cpp）→ 会话结果 → 本模块；路径 `.json→.wav` 复用 `wavPathFor` 规则。
- **协议契约**：docs/data-schema.md §9（二进制导出，不入 JSON schema）。
- **测试**：tests/WavExporterTests.cpp（round-trip 逐采样比对）+ tests/CommandParserTests.cpp [exportwav]（命令级）。

## SCHEMA CONTRACT

`docs/data-schema.md` 是权威导出契约，8 类：context / raw_capture / frequency_response / scan / gr_timeline / compression_family / dataset / note。导出必须含：

- 插件元数据：class_id（FUID）、manufacturer、version、latency_samples
- 测量配置：generator 参数、sample_rate、block_size、fft_size、smoothing
- 旁路双路参照：dry / wet / bypass
- 参数快照：normalized + actual 双值

## CONVENTIONS

- 改导出先改 body-equiv 测试，再动 Export.cpp。
- 分析器 Result struct 默认构造为空，MeasurementResults 每类型仅一个被填充。
- 改 schema 顺序：docs/data-schema.md → Export.cpp → 测试（契约驱动）。
