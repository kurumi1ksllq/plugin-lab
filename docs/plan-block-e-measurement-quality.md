# PluginLab 块 1：E 测量质量改进实施计划

> 2026-08-08 制定。路线图总览见 `docs/roadmap-next.md` 块 1。块 0（C 稳定加固）已完成（docs/plan-block-c-stability.md）。
> 执行原则：TDD（RED→GREEN）+ 真机验收（全 IPC 驱动，不控制鼠标）+ 每任务原子提交 push（GitHub 存档）+ 收尾双轴代码审查。
> 现状核实（2026-08-08）：`Impulse` 生成器已实现 MLS 序列生成（LFSR，±1 归一化）；`MultiTone` 无相位选项（同相起振）；`ScanEngine::run` 已实现多轮参数扫描（round 循环 + progress + 取消 + RAII 快照恢复）。

## 目标

EQ 线性测量提速：MLS 激励接入频响路径（替代扫频，快一个量级）；MultiTone 加确定性随机初始相位（降峰值因子，同种子可复现）；重新定性"多轮参数扫描接口"（ScanEngine 已覆盖，核实即完成）。

## 全局约束（每任务隐式包含）

- 构建：`cmake -S . -B build && cmake --build build --config Release`（/W4 /permissive- /WX /utf-8 警告即错误）
- 测试：`cmake -S . -B build -DBUILD_TESTS=ON && cmake --build build --config Release && ctest --test-dir build -C Release --timeout 180`；全绿，连跑 2 次
- 新增 .cpp 双编译（根 + tests CMakeLists target_sources）；命名带单位；`#pragma once`；include 相对路径
- 改协议/导出：四件套（Protocol.h / CommandParser / tests / docs/data-schema.md）
- 提交：Conventional Commits（英文），每任务一 commit + push origin main
- 铁律：无失败测试不写生产代码；无新鲜验证证据不宣称完成

---

## 任务 E1：MLS 接入 EQ 线性测量（频响路径）

**背景**：`Impulse` 生成器已能产出 MLS 序列（`useMLS(true)` + `setMLSLength`，LFSR 确定性），但频率响应测量路径固定用 SineSweep + Farina 反卷积（`FreqResponse::analyze` 只认 sweep）。MLS 的优点是单次激励覆盖全频段，解卷积得 IR 后 FFT 即得频响——比扫频快一个量级。本任务补上"MLS 激励 → 解卷积 → 频响"的完整链路。

**Files:**
- Modify: `source/analysis/FreqResponse.h/.cpp`（新增 MLS 解卷积路径：`analyzeMLS`）
- Modify: `source/capture/MeasurementSession.h/.cpp`（frequencyResponse 可选用 MLS 激励）
- Modify: `source/ipc/Protocol.h`（measure 命令可选 `"excitation":"mls"`，默认缺省 sweep）
- Modify: `source/ipc/CommandParser.cpp`（解析 excitation 字段 → session）
- Test: `tests/FreqResponseTests.cpp`（MLS 一致性）、`tests/CommandParserTests.cpp`（excitation 解析）、`tests/MeasurementSessionTests.cpp`（如存在，MLS 激励跑通）
- Docs: `docs/data-schema.md`（context.measurement.excitation 字段）、`source/analysis/AGENTS.md`、`source/capture/AGENTS.md`

**Interfaces:**
- `FreqResponse::Result FreqResponse::analyzeMLS (const juce::AudioBuffer<float>& dry, const juce::AudioBuffer<float>& wet, double sr, int mlsLength)`——MLS 循环互相关（频域除法：FFT(wet)/FFT(dry) 逐 bin 复数除法，等价于解卷积）→ 逆 FFT 得 IR → 与 analyze 相同的幅度/相位提取。`setLatencySamples` 相位补偿语义不变
- `MeasurementSession::setFreqExcitation (bool useMLS)`——frequencyResponse 时选择激励；false（默认）走现有 SineSweep
- Protocol：measure 命令新增可选 `"excitation":"sweep"|"mls"`（缺省 sweep，向后兼容）

- [ ] **Step 1（RED）：FreqResponseTests 一致性测试**

```cpp
TEST_CASE ("FreqResponse: MLS excitation matches sine-sweep magnitude (TestPlugin gain)",
           "[freqresponse][mls]")
{
    // Arrange — same TestPlugin (gain 2.0), two recordings:
    // sweep: SweepRunner + SineSweep (100..20000 Hz, 0.5s)
    // mls:   SweepRunner + Impulse (useMLS(true), length 16383)
    // Both through the SAME plugin instance; capture dry/wet.
    TestPlugin plugin;
    plugin.setGain (2.0);

    // ... run sweep recording (existing helper pattern from sweep tests)
    // ... run mls recording (Impulse, mlsLength 16383)

    // Act
    const auto sweepResult = FreqResponse().analyze (drySweep, wetSweep, 48000.0);
    const auto mlsResult = FreqResponse().analyzeMLS (dryMls, wetMls, 48000.0, 16383);

    // Assert — magnitudes agree at shared frequencies within 0.2 dB
    // (MLS +20 dB SNR vs impulse; sweep vs MLS both measure the same LTI)
    for (const auto& p : mlsResult.raw)
    {
        if (p.frequency < 100.0 || p.frequency > 20000.0) continue;
        // interpolate sweepResult.raw at p.frequency (helper)
        const auto sweepMag = interpolateMag (sweepResult.raw, p.frequency);
        INFO ("f = " << p.frequency);
        REQUIRE (std::abs (p.magnitudeDB - sweepMag) < 0.2);
    }
}
```

- [ ] **Step 2：跑测试确认 RED**（analyzeMLS 不存在 → 编译失败）
- [ ] **Step 3（GREEN）：实现 analyzeMLS**

```cpp
// FreqResponse.cpp
FreqResponse::Result FreqResponse::analyzeMLS (const juce::AudioBuffer<float>& dry,
                                               const juce::AudioBuffer<float>& wet,
                                               double sr, int mlsLength)
{
    // MLS deconvolution via frequency-domain division:
    //   H(f) = FFT(wet) / FFT(dry)  (bin-wise complex division)
    // then IFFT → impulse response → same magnitude/phase extraction as analyze.
    // mlsLength determines the FFT size (next power of two ≥ 2 * mlsLength
    // to avoid circular aliasing of the linear convolution tail).
    ...
    // Extract points on the same frequency grid as analyze (log-spaced or
    // per-bin with magnitude/phase), reusing processChannel-style output.
}
```

- [ ] **Step 4：MeasurementSession 接线**——`setFreqExcitation(bool)`：run() 内 frequencyResponse 分支选择 SineSweep 或 Impulse(useMLS)
- [ ] **Step 5：Protocol/CommandParser 四件套**——measure 命令 `"excitation"` 字段；CommandParserTests 补解析用例（缺省 sweep、显式 mls、非法值报错）
- [ ] **Step 6：全量 ctest 连跑 2 次**
- [ ] **Step 7：真机验收**——IPC `{"cmd":"measure","type":"frequency_response","excitation":"mls"}` 对 Pro-Q 4 跑通；对比同参数 sweep 结果（out/ 下两文件）幅度差 < 0.5 dB；日志无 ERROR
- [ ] **Step 8：文档同步**——data-schema.md context.measurement.excitation；signal/analysis/capture AGENTS.md 更新测量用法表
- [ ] **Step 9：提交**

```bash
git add source/analysis/FreqResponse.cpp source/analysis/FreqResponse.h source/capture/MeasurementSession.cpp source/capture/MeasurementSession.h source/ipc/Protocol.h source/ipc/CommandParser.cpp tests/FreqResponseTests.cpp tests/CommandParserTests.cpp docs/data-schema.md source/analysis/AGENTS.md source/capture/AGENTS.md
git commit -m "feat(analysis): MLS excitation for frequency response (deconvolution path)"
git push origin main
```

---

## 任务 E2：MultiTone 随机初始相位（固定种子可复现）

**背景**：`MultiTone::generate` 各频点同相起振（`sin(2πft)`），多音叠加峰值因子（crest factor）高——数模级联易削波。加确定性随机初始相位可显著降 CF；固定种子保证同参数可复现（测量可比性）。

**Files:**
- Modify: `source/signal/MultiTone.h/.cpp`
- Test: `tests/MultiToneTests.cpp`（现有文件追加；若不存在则新建）
- Docs: `source/signal/AGENTS.md`（关键参数表补相位行）

**Interfaces:**
- `void MultiTone::setRandomPhaseSeed (uint32_t seed)`——0（默认）= 保持现有全零相位（向后兼容，现有测试不受影响）；非 0 = 用确定性 PRNG（xorshift32，种子=seed）为每个频率生成 `[0, 2π)` 初始相位，`prepare()` 时计算
- 内部：`std::vector<double> phases;`（prepare 填充）；generate 里 `sin(2πft + phases[i])`

- [ ] **Step 1（RED）：MultiToneTests**

```cpp
TEST_CASE ("MultiTone: random phase seed lowers crest factor vs zero phase",
           "[multitone][phase]")
{
    MultiTone mt;
    mt.setFrequencies ({ 100.0, 200.0, 400.0, 800.0, 1600.0, 3200.0, 6400.0, 12800.0 });
    mt.setDuration (1.0);
    mt.setAmplitude (0.3);
    mt.prepare (48000.0, 512);

    // zero-phase reference
    juce::AudioBuffer<float> zeroBuf (1, 48000);
    mt.reset();
    mt.setRandomPhaseSeed (0);
    mt.prepare (48000.0, 512);
    mt.generate (zeroBuf, 0, 48000);
    const double zeroCF = crestFactor (zeroBuf);

    // random-phase (seed 42)
    juce::AudioBuffer<float> randBuf (1, 48000);
    mt.setRandomPhaseSeed (42);
    mt.prepare (48000.0, 512);
    mt.generate (randBuf, 0, 48000);
    const double randCF = crestFactor (randBuf);

    REQUIRE (randCF < zeroCF);          // crest factor dropped
    REQUIRE (randCF < 4.0);             // 8-tone random-phase CF ≈ 3.3
}

TEST_CASE ("MultiTone: same seed reproduces identical waveform", "[multitone][phase]")
{
    MultiTone mt;
    mt.setFrequencies ({ 100.0, 1000.0, 5000.0 });
    mt.setDuration (0.1);
    mt.setAmplitude (0.3);

    juce::AudioBuffer<float> a (1, 4800), b (1, 4800);
    mt.setRandomPhaseSeed (7); mt.prepare (48000.0, 512);
    mt.generate (a, 0, 4800);
    mt.setRandomPhaseSeed (7); mt.prepare (48000.0, 512);
    mt.generate (b, 0, 4800);

    for (int i = 0; i < 4800; ++i)
        REQUIRE (a.getSample (0, i) == b.getSample (0, i));   // bit-identical
}

TEST_CASE ("MultiTone: seed 0 keeps legacy zero-phase waveform", "[multitone][phase]")
{
    // generate with seed 0 and compare against the pre-phase waveform
    // (sin-only): sample 0 must equal the legacy value, proving default
    // path is byte-identical to the old implementation.
}
```

- [ ] **Step 2：跑测试确认 RED**（setRandomPhaseSeed 不存在）
- [ ] **Step 3（GREEN）：实现**

```cpp
// MultiTone.h
/** Deterministic random initial phases (per frequency) to lower the crest
 *  factor. Seed 0 (default) keeps the legacy all-zero-phase waveform;
 *  any non-zero seed reproduces the same waveform every run. */
void setRandomPhaseSeed (uint32_t seed) { phaseSeed = seed; }
// private: uint32_t phaseSeed = 0; std::vector<double> phases;

// MultiTone.cpp prepare():
//   phases.resize (frequencies.size());
//   if (phaseSeed != 0)
//   {
//       uint32_t state = phaseSeed;
//       const auto rnd = [&state] { state ^= state << 13; state ^= state >> 17;
//                                   state ^= state << 5; return state; };
//       for (auto& ph : phases)
//           ph = 2.0 * juce::MathConstants<double>::pi
//                * (static_cast<double> (rnd()) / 4294967296.0);
//   }
//   else
//   {
//       for (auto& ph : phases) ph = 0.0;
//   }

// generate(): double phase = 2.0 * pi * freq * t + phases[i];
```

- [ ] **Step 4：跑测试确认 GREEN**（含既有 MultiToneTests 回归——seed 0 路径字节不变）
- [ ] **Step 5：全量 ctest + 提交**

```bash
git add source/signal/MultiTone.cpp source/signal/MultiTone.h tests/MultiToneTests.cpp source/signal/AGENTS.md
git commit -m "feat(signal): deterministic random initial phases for MultiTone (lower crest factor)"
git push origin main
```

---

## 任务 E3：runMultiple 多轮参数扫描接口（重新定性）

**现状核实（2026-08-08）**：roadmap 块 1 原任务"`MeasurementSession::runMultiple` 多轮参数扫描接口"——**已被 `ScanEngine::run` 完整覆盖**（阶段 3 交付）：参数+档位数组 → 每轮复用同一 MeasurementSession → 曲线族 `ScanResult.family[]` + progress 回调 + 取消 + 参数快照/恢复 RAII。块 A（批量采集）也基于 ScanEngine 完成。

**重新定性**：本任务改为**验证型**——确认 ScanEngine 满足 roadmap 意图（多轮参数扫描、块 A 复用），无新增代码；若审查发现缺口（如 runMultiple 原意含"同参数多次测量取平均/统计"语义），则补 ScanEngine 平均增强。

- [ ] **Step 1：审查确认**——读 `ScanEngine::run` + `ScanEngineTests`，对照 roadmap 意图（多轮扫描 + 曲线族 + 取消 + 快照恢复）逐项核对，记录结论
- [ ] **Step 2：若缺口（同参数多轮平均）被确认**——brainstorming 定案后补 `ScanResultEntry.averageOf` 或 `ScanEngine::runRepeated`；否则无代码
- [ ] **Step 3：roadmap/STATUS 标记完成**（验证型结论）

```bash
git add STATUS.md docs/roadmap-next.md
git commit -m "docs: mark runMultiple covered by ScanEngine (verification-type task E3)"
git push origin main
```

---

## 收尾：全量回归 + 审查 + push

- [ ] **Step 1：全量 ctest 连跑 2 次**（记录输出）
- [ ] **Step 2：双轴代码审查（requesting-code-review）**——Standards + Spec 并行
- [ ] **Step 3：真机冒烟**——MLS 频响 + 扫频频响各 1 次 IPC 对比；harmonic 测量（MultiTone 随机相位启用）1 次
- [ ] **Step 4：文档同步**（AGENTS.md 计数、roadmap 执行状态、STATUS）
- [ ] **Step 5：最终 push origin main**

## 风险

| # | 风险 | 等级 | 缓解 |
|---|------|------|------|
| R1 | MLS 频域除法在低频 bin 信噪比差（dry 能量低处除以近零） | P1 | 频响点提取用与 analyze 相同的平滑/截断；测试用增益插件（LTI 精确），真机限 100Hz 以上 |
| R2 | MLS 线性卷积尾部混叠（mlsLength 周期不足） | P1 | FFT size ≥ 2×mlsLength；测试断言与 sweep 一致即证明无混叠 |
| R3 | MultiTone 随机相位影响 harmonicAnalysis 基频取峰（相位改变峰形） | P2 | 分析器逐基频独立取峰（幅度域），相位不改变基频幅度；回归测试锁定 |
| R4 | E3 重新定性范围蔓延（用户原意可能真是"同参数多轮平均"） | P2 | 验证型先行，缺口确认后才扩展；roadmap 原文"块 A 可选复用"已由块 A 用 ScanEngine 兑现 |
