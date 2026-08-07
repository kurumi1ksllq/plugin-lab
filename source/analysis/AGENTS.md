# analysis（分析器 + JSON 导出层）

**生成:** 2026-08-03 · **规模:** 14 文件（6 分析器 × .h/.cpp + Export.h/.cpp，Export 23 符号为最大）

## OVERVIEW

6 分析器 + Export 层：原始捕获 → 领域结果 → 8 类 schema JSON。

## ANALYZERS

| 分析器            | 输入 → 输出                                                                      | 喂给                                   |
| ----------------- | -------------------------------------------------------------------------------- | -------------------------------------- |
| FreqResponse      | Farina 反卷积（sweep→IR→FFT→幅度比+相位）→ 点列 (freq/mag/phase)                 | freq 测量、EQ 曲线                     |
| HarmonicAnalysis  | MultiTone 8 基频（100–12800Hz）逐基频 → THD% + H2..H5 各次谐波 % | 谐波柱状图 |
| CompressionCurve  | 输入 dB vs 输出 dB → {input_dB, output_dB, gr_dB}[] + 拟合参数（压缩比/拐点/GR） | 压缩曲线                               |
| GainReduction     | 每 block 20·log10(RMS_wet/RMS_dry) → 逐 block GR                                 | 实时 GR 表头（50ms 节流）+ gr_timeline |
| TimeConstants     | τ（attack/release）估计 → τ + .valid 标志                                        | 时间常数显示                           |
| CompressionFamily | 输入电平 × 速度网格 → 每格压缩曲线 + GR 时间线                                   | compression_family 网格（阶段 4 已交付） |

### 要点

- **THD/IMD 勿混用**：THD 单音、IMD 多音，混用则谐波峰交叠（DESIGN.md:102）。注意：`harmonicAnalysis` 当前实现用 MultiTone 八度基频（100/200/.../12800 Hz，MeasurementSession.cpp:97），逐基频独立测谐波+THD——低频基频谐波会落在高频基频上（已知取舍）。
- **FreqResponse**：必须 Farina 反卷积，勿直接 FFT 比（低/高频相位噪声，Oracle 修复）。
- **TimeConstants**：动态源 τ 有效（957e597 暴露 `carrier_start_hz` 默认 10000 + GainReduction 1ms RMS 窗口 + 正 dB 副本估计）；file/noise 源无边沿 → tau.valid=false 属设计。
- **GainReduction**：实时 GR 表头走 AsyncUpdater ~50ms 节流，勿每 block 刷 UI。

## EXPORT LAYER

- 手写 JSON：raw string literal + escapeJsonString；juce::JSON::toString 已弃用（引号转义 bug，pluginName.quoted() 不转内部引号，Oracle P0-4）。
- datasetToJSON 聚合 scan 族 / gr_timeline / compression_family 为单个 Dataset 包；appendDatasetScanFamily 辅助函数。
- 既有导出函数不变，body-equiv 测试锁等价性。

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
