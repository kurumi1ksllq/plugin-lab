# #102 — 多电平非线性测量：THD vs 电平曲线（nonlinear_family，T5）

grade: standard
分支：`issue-102-nonlinear-family`（worktree: `/root/plugin-lab-102`）
父 issue：#102（enhancement / ready-for-agent，块 B）；下游 #106（T8）

## 目标

新增「同一激励、-60..0 dB 逐级加大电平」的测量：每个电平点算一次 THD，导出成 `nonlinear_family`（档位 × 谐波分析的网格）+ dataset 块，用于区分**硬削波 / 软饱和 / 谐波饱和**（clip 削波器 + 饱和器类别的核心需求）。

## 背景事实（2026-11-18 核查）

- issue 里「旧分支工作保存在 `.scratch/t5-issue102-archive/`（bundle + 5 个 patch）」**已丢失**：`.scratch/` 不存在，全盘 `*.bundle` / `*nonlinear*` 零命中。**本票是重做**，不是续做。
- 镜像模板现成：`source/analysis/CompressionFamily.{h,cpp}`（levelsDB × 分析器网格 + 原子 cancel + progress）。
- 单电平谐波分析现成：`source/analysis/HarmonicAnalysis.{h,cpp}`（`ToneResult::thdPercent`）。

## 边界

- **不动** `SweepRunner`（冻结边界）、不改 `MeasurementSession::Type` 枚举（`nonlinear_family` 是 dataset 的可选块，与 `compression_family` 同构，**不进** `battery[]`、不进 `Protocol::MeasureType`）。
- **不动** 子进程路径：`ChildWavAnalyzer` / `ChildMeasureOrchestrator` 类型白名单保持 4 类（dataset 块从不下发子进程），按 ADR-D-7 在注释里显式说明「v1 不实现」。
- **不改** `scan/` 的 3 类白名单（验收明确「scan family 不动」）。
- 默认无 `levels_db` 时，单电平 harmonic 导出**字节不变**（向后兼容硬线）。

## Tickets（红-绿循环）

| id | 内容 | 验收 |
| --- | --- | --- |
| T1 | `tests/TestSaturatorPlugin.h`：确定性 waveshaper（`tanh(drive·x)` 归一），`fillInPluginDescription` 唯一 id，参数用 hosted parameter | 可 `prepareToPlay`/`processBlock`，THD 随电平单调 |
| T2 | `tests/HarmonicFamilyTests.cpp` RED：网格计数、THD 随电平单调增、取消、默认无 levels 路径兼容 | 先红后绿 |
| T3 | `source/analysis/HarmonicFamily.{h,cpp}`：`measure(plugin, session, levelsDB, progress)` + 格间 cancel + `FamilyEntry{inputLevelDB, tone}` | 双 CMake 登记后编译过 |
| T4 | `MeasurementSession::setHarmonicLevelsDB` + `run()` harmonic 分支：非空则逐格 `setAmplitude(pow(10, dB/20))`，空则保持 `0.4` | 默认路径导出字节不变 |
| T5 | 网格实现接 `HarmonicAnalysis::analyze` | T2 全绿 |
| T6 | `SPEC.md` 新增 `§13 nonlinear_family` + 导出类型表 / dataset 块表两行（先文档） | python `json.load` 可解析示例 |
| T7 | `Export::harmonicFamilyToJSON` + `Dataset::nonlinearFamily` + body-equiv 测试 | `[export][dataset-body-equiv]` 锁 nonlinear 块 = 独立导出 |
| T8 | `CommandParser` dataset `nonlinear_family{levels_db}`（默认档位表）+ 响应标志 + 两条测试 | 有效 JSON + 响应计数；`battery[]` 4 类型断言不破 |
| T9 | 全量收尾 | `SweepRunner` / `battery[]` diff 为零；ctest 双跑绿 |

## 验收（issue 原文）

- [ ] ctest 双跑绿 + /W4 /WX 零警告
- [ ] HarmonicFamilyTests：确定性 THD-vs-level；电平单调增 THD；取消中途网格；默认无 levels 路径与今日 harmonic 导出字节一致
- [ ] `[export][dataset-body-equiv]` 锁定 nonlinear_family 块 = 独立导出
- [ ] SPEC.md schema 先写；python json.load 可解析导出
- [ ] dataset 命令 nonlinear_family 有效 JSON + 响应计数；scan family 不动
- [ ] SweepRunner 零改动（冻结边界）

## 验证能力（重要）

本机是 **Linux，无 cmake/MSVC**，而本项目是 Windows/MSVC + JUCE：**C++ 无法本地编译/跑 ctest**。因此 T1–T5、T7–T9 的验证路径只能是 GitHub Actions 的 `windows-latest`（/W4 /WX 零警告 + ctest 双跑）。PR 迭代成本显著高于 #105（Python 本地毫秒级），需按此安排轮次：一次提交尽量完整，减少 CI 往返。

## 裁剪

- 不新增 CI job（复用现有 `build-and-test`）。
- 双轴 review 不裁剪。

## 变更记录

### 变更点 1（CI 实测后修订）：谐波条数是 7 不是 8

- 首次 CI（run 44133947619）编译通过（/W4 /WX 零警告），但 `tones.size() == 8` 断言失败，实测 **7**。
- 原因：激励含 8 个基频（100 Hz…12.8 kHz），12.8 kHz 的 2 次谐波 25.6 kHz 已超 Nyquist（48 kHz），该音没有带内谐波 → `HarmonicAnalysis::analyze` 不为它产出 tone。
- 改为断言「≥7 且 ≤8，且 400 Hz tone 恒在」——钉住对消费者有价值的形状，不钉 FFT 的带边行为。

### 变更点 2（CI 实测后修订）：硬削波的判别改用「膝点」，不用单点比较

- 原设计断言「同 drive 下硬削波 THD > tanh THD」，在 -6 dBFS/drive 2 时反了：此时硬削波钳位刚好触及峰值（平顶极窄），而 tanh(2x) 全程弯曲，THD 反而更高。
- 改为断言硬削波的**定义性质**：-20 dBFS（钳位完全不生效）THD < 1%，0 dBFS THD > 5% 且 > 10× 低电平值——即「膝点」；软饱和的渐进增长由 `[thd-vs-level]` 覆盖。这个断言不依赖两条曲线在某个电平上的交叉位置。
