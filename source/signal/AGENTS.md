# signal — 信号生成器模块

## OVERVIEW

本模块提供 7 个确定性测量激励生成器（16 文件：接口 1 对 + 各生成器 1 对），统一实现 `SignalGenerator` 接口，被 `SweepRunner` 的 generate→process→capture 管线消费，零特判。

## GENERATORS

| 生成器           | 用途                                | 关键参数                                                             |
| ---------------- | ----------------------------------- | -------------------------------------------------------------------- |
| `SineSweep`      | 线性 EQ 频响（Farina 解卷积）       | 20Hz–20kHz 对数扫频                                                  |
| `MultiTone`      | IMD 互调测量                        | 多音叠加；`setRandomPhaseSeed(seed)`（0=旧全零相位波形，非 0=xorshift32 确定性随机初始相位降峰值因子）；谐波峰与基频峰可能重叠（勿与 THD 混用）                    |
| `ToneBurst`      | 静态压缩曲线                        | 多电平突发（-60..0 dB）；`setLevels()` / `setMasterAmplitude(scale)` |
| `Impulse`        | 快速线性 EQ 测量（可选）            | 单冲激；MLS 候选                                                     |
| `FilePlayback`   | 音频文件播放（vocal 素材）          | 通道映射；文件头在 prepare 读取                                      |
| `NoiseGenerator` | 白/粉噪（确定性）                   | 固定种子（默认 `0x2E42A5`，测试用 42）                               |
| `EnvelopeSignal` | 动态源，opto/vari-mu 压缩 GR 时间线 | ADSR（默认 0.02/0.1/0.8/0.2 s）；载波=SineSweep（频率范围起点经 session `carrierStartHz`，见下） |

## INTERFACE CONTRACT

- `prepare(sampleRate)`：音频线程外调用；FilePlayback 在此读文件头（采样率/长度/声道数），以更新 getTotalLength。
- `generate(buffer)`：音频线程回调，产出样本。
- `getTotalLength()`：返回总样本数。**禁止返回 -1**——-1 视为无限长，触发 SweepRunner 静默 10s 兜底（plan-phase2-5 P0-2）；噪声等非有限源须返回有限时长。
- `reset()`：回到起始位置（FilePlayback 回文件头）。
- SweepRunner 不做类型分派：file/noise/dynamic 与正弦源走同一 generate→process→capture 管线。

## MEASUREMENT USAGE

| 测量类型            | 生成器                          |
| ------------------- | ------------------------------- |
| freq（频响）        | SineSweep（Impulse 为快速候选） |
| harmonic（THD/谐波）| MultiTone（8 个八度基频 100–12800Hz，每基频独立测谐波+THD） |
| compression（静态） | ToneBurst（多电平）             |
| grTimeline（动态）  | EnvelopeSignal                  |

**铁律：THD 与 IMD 信号永不混用**——多音谐波峰与基频峰交叠（2kHz H2=4kHz 落上 4kHz 基频），测得失真无意义（DESIGN.md:102 勿混用）。注意：当前 `harmonicAnalysis` 实现即用 MultiTone 八度基频（100/200/.../12800），低频基频的高次谐波会落在高频基频上——这是已知实现取舍，分析器逐基频独立取峰，勿把该信号再当 IMD 用。

已知限制：`carrier_start_hz` 已暴露到 IPC（957e597，IPC 解析默认 10000 Hz 匹配 CompressionFamily；MeasurementSession 成员默认 20.0，CompressionFamily 固定 10000）——动态源 GR τ 估计在真实插件上有效（Pro-C 3 实测 attack=3.9ms/release=39.7ms, tau.valid=true）。EnvelopeSignal::getTotalLength 对无限载波返回 -1（EnvelopeSignal.cpp:86，代码路径存在但当前载波全为有限 SineSweep，不触发 10s 兜底）。

## EXTENDING

新增生成器步骤：

1. 实现 `SignalGenerator` 接口（新 .h/.cpp 对）。
2. 注册到根 `CMakeLists.txt` target_sources，**同时**加进 `tests/CMakeLists.txt`（双编译）。
3. 新增 `tests/<Name>Tests.cpp`（确定性 ground truth 断言）。
4. 按需接线：`MeasurementSession::Source` enum、`Protocol.h` source 轴（signal|file|noise|dynamic）、`CommandParser::parseSource`、`docs/data-schema.md`。
