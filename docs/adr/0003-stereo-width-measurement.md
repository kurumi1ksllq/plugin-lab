# 0003 — 声场类测量：stereo_width 测量类型决策

- **日期**: 2026-08-25
- **状态**: 已接受（T6a #103，待 T6b 实现）

## 背景

采集方案完善块 B（2026-08-25 用户确认）：声场类效果器（stereo width / MidSide 处理器 / 立体声化 / 空间感）需要可测可复刻。现有 4 类测量（frequency_response / harmonic / compression / gr_timeline）全部是**单声道视角**——FreqResponse 只分析 channel 0（FreqResponse.cpp:22-23），所有生成器把相同信号写进每个声道（SignalGenerator.h:21）。声场类插件改变 L/R 关系，单声道测量完全测不到。

## 决策

新增第 5 种测量类型 **`stereo_width`**（Protocol `MeasureType::stereoWidth` → `MeasurementSession::Type::stereoWidth`），采用：

1. **MidSide 双激励**：Run A mid-only（L=R=扫频）测 mid→side 交叉；Run B side-only（L=−R=扫频）测 side→mid 交叉。确定性黑盒，不依赖插件先验。
2. **指标**：H_M(f) / H_S(f)（MS 分离频响）、相关度 ρ(f)=|Sxy|²/(Sxx·Syy)、宽度 width_db(f)=20·log10(|M|/|S|)。
3. **信号**：SineSweep/MLS 加声道极性模式（mono/mid/side），确定性相位重启，有限 getTotalLength()。
4. **导出**：新 schema `stereo_width`（SPEC.md），含 mid_response/side_response/correlation/width_db。
5. **子进程路径 v1 显式拒绝**该类型（ChildWavAnalyzer 不实现 stereo 分析器；防静默 mono 回退污染数据）。

## 备选

- **扩展现有 frequency_response 加 MS 模式**：破坏 body-equiv 向后兼容，且单测量内隐含双激励语义复杂，导出结构混乱。
- **L/R 独立双测量**：需要用户/调用方手动拼两个测量，无相关度/宽度指标，无法表征 L/R 关系。
- **子进程 v1 支持**：需 ChildWavAnalyzer 加 stereo 分析器（WAV 多轨解析 + MS 解码），范围膨胀，v1 不做（显式拒绝，后续可加）。

## 影响

- 新增文件：StereoAnalysis.{h,cpp}、TestStereoPlugin.h；修改：MeasurementSession、MeasurementAnalysis、Export、Protocol.h、CommandParser、SPEC.md、PlotWidget、Main.cpp。
- 测量类型枚举扩展（Type 加 stereoWidth）——analyzeByType 的 switch 必须穷尽（MeasurementAnalysis 深模块，单一分发点，issue #42）。
- SweepRunner 冻结边界不动：立体声激励在 generator 配置层，分析在新分析器层。
- SPEC.md schema 契约先行；[export][dataset-body-equiv] 锁定新块。
- T6b（#104）实现；T8（#106）复刻闭环消费 stereo_width 数据。