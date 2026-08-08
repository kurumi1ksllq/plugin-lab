# PluginLab 块 1：E 测量质量改进实施计划

> 2026-08-08 制定（v2 细化版，可直接照做）。路线图总览见 `docs/roadmap-next.md` 块 1。块 0（C 稳定加固）已完成（docs/plan-block-c-stability.md）。
> 执行原则：TDD（RED→GREEN）+ 真机验收（全 IPC 驱动，不控制鼠标）+ 每任务原子提交 push（GitHub 存档）+ 收尾双轴代码审查。
> 现状核实（2026-08-08）：`Impulse` 生成器已实现 MLS 序列（LFSR，±1 归一化，`useMLS(true)` + `setMLSLength`）；`MultiTone` 无相位选项（同相起振，`sin(2πft)`）；`ScanEngine::run` 已实现多轮参数扫描；`FreqResponse::analyze` 用 H1 帧平均估计；tests/CMakeLists.txt 已编入 `MultiTone.cpp`/`Impulse.cpp` 但**无 MultiToneTests.cpp**（E2 需新建并注册）。

## 目标

EQ 线性测量提速：MLS 激励接入频响路径（替代扫频，快一个量级）；MultiTone 加确定性随机初始相位（降峰值因子，同种子可复现）；E3 重新定性为验证型（ScanEngine 已覆盖多轮扫描）。

## 全局约束（每任务隐式包含）

- 构建：`cmake -S . -B build && cmake --build build --config Release`（/W4 /permissive- /WX /utf-8 警告即错误）
- 测试：`cmake -S . -B build -DBUILD_TESTS=ON && cmake --build build --config Release && ctest --test-dir build -C Release --timeout 180`；全绿，连跑 2 次
- 新增 .cpp 双编译（根 + tests CMakeLists target_sources）；命名带单位；`#pragma once`；include 相对路径
- 改协议/导出：四件套（Protocol.h / CommandParser / tests / docs/data-schema.md）
- 提交：Conventional Commits（英文），每任务一 commit + push origin main
- 铁律：无失败测试不写生产代码；无新鲜验证证据不宣称完成

---

## 任务 E1：MLS 接入 EQ 线性测量（频响路径）

**背景**：`Impulse` 生成器已能产出 MLS 序列，但 `FreqResponse` 只有扫频 H1 估计路径。MLS 激励覆盖全频段，整段 FFT 频域除法即得频响，比扫频（5s 激励 + 分帧平均）快一个量级。本任务补 `FreqResponse::analyzeMLS` 分析路径 + `MeasurementSession` 激励选择 + IPC `excitation` 字段。

**Files:**
- Modify: `source/analysis/FreqResponse.h/.cpp`（新增 `analyzeMLS`；把 phase-unwrap+latency 补偿、octave smoothing 抽为私有辅助，两路径共用）
- Modify: `source/capture/MeasurementSession.h/.cpp`（`setFreqExcitation`，run() frequencyResponse 分支选择 SineSweep 或 Impulse）
- Modify: `source/ipc/Protocol.h`（`Excitation` 常量）
- Modify: `source/ipc/CommandParser.cpp`（measure 命令解析 `excitation` 字段 → session；dataset 的 freq 块透传）
- Modify: `tests/FreqResponseTests.cpp`（新增 `generateMLS` helper + 2 用例）
- Modify: `tests/CommandParserTests.cpp`（excitation 解析 3 用例）
- Docs: `docs/data-schema.md`（context.measurement.excitation）、`source/analysis/AGENTS.md`、`source/capture/AGENTS.md`、`source/ipc/AGENTS.md`（measure 协议表）

**Interfaces:**
- `FreqResponse::Result analyzeMLS (const juce::AudioBuffer<float>& dry, const juce::AudioBuffer<float>& wet, double sr, int mlsLength)`——整段 FFT 频域除法 `H = Y/X`（FFT size = 2×mlsLength 补零，避免循环卷积混叠），低能量保护，输出与 analyze 相同的 Point 结构 + 相位 unwrap + latency 补偿 + 1/12、1/3 octave 平滑
- `void MeasurementSession::setFreqExcitation (bool useMLS)`——默认 false（向后兼容 sweep）
- Protocol：measure 命令新增可选 `"excitation":"sweep"|"mls"`（缺省 sweep）

### Step 1（RED）：FreqResponse.h 声明 analyzeMLS + 抽私有辅助

```cpp
// FreqResponse.h 新增 public:
    /** Analyze frequency response from an MLS excitation recording.
     *  @param dry  MLS input (full period + tail)
     *  @param wet  MLS output (same length)
     *  @param sr   Sample rate of the recording
     *  @param mlsLength  MLS sequence length (period in samples)
     */
    Result analyzeMLS (const juce::AudioBuffer<float>& dry,
                       const juce::AudioBuffer<float>& wet,
                       double sr, int mlsLength);

// FreqResponse.h private 新增（从 analyze/processChannel 抽出，两路径共用）:
    /** Unwrap phase across frequency and apply latency compensation. */
    void applyPhasePost (std::vector<Point>& points, double sampleRate) const;

    /** Build smoothed_1_12 / smoothed_1_3 curves from raw points. */
    void applySmoothing (const std::vector<Point>& raw, double sr, int fftSize,
                         Result& result) const;
```

### Step 2：FreqResponseTests 写失败测试（复用现有 helper）

```cpp
// FreqResponseTests.cpp 顶部新增 helper（generateSweep/applyFilters 已存在）:
#include "../source/signal/Impulse.h"

//==============================================================================
// Helper: generate a full MLS excitation buffer (mono, channel 0).
static juce::AudioBuffer<float> generateMLS (double sr, int mlsLength, double amplitude)
{
    Impulse imp;
    imp.useMLS (true);
    imp.setMLSLength (mlsLength);
    imp.setAmplitude (amplitude);
    imp.prepare (sr, 512);

    juce::AudioBuffer<float> buf (1, mlsLength);
    buf.clear();
    imp.generate (buf, 0, mlsLength);
    return buf;
}

//==============================================================================
// Helper: linear interpolation of magnitude at a frequency on a curve.
static double interpMagDB (const std::vector<FreqResponse::Point>& curve, double targetFreq)
{
    REQUIRE (curve.size() >= 2);
    for (size_t i = 1; i < curve.size(); ++i)
    {
        if (curve[i].frequency >= targetFreq)
        {
            const double f0 = curve[i-1].frequency, f1 = curve[i].frequency;
            const double t = (targetFreq - f0) / (f1 - f0);
            return curve[i-1].magnitudeDB + t * (curve[i].magnitudeDB - curve[i-1].magnitudeDB);
        }
    }
    return curve.back().magnitudeDB;
}

//==============================================================================
// Test: analyzeMLS matches analyze for the same filtered system.
TEST_CASE ("FreqResponse: analyzeMLS matches analyze for a known filter", "[freqresponse][mls]")
{
    const double sr = 48000.0;

    // Same filter cascade as the sweep test: bell +6dB@1kHz Q1 + lowpass 8k
    float gainLinear = juce::Decibels::decibelsToGain (6.0f);
    std::vector<juce::dsp::IIR::Coefficients<float>::Ptr> coeffs;
    coeffs.push_back (juce::dsp::IIR::Coefficients<float>::makePeakFilter (
        sr, 1000.0, 1.0f, gainLinear));
    coeffs.push_back (juce::dsp::IIR::Coefficients<float>::makeLowPass (
        sr, 8000.0, 1.0f / std::sqrt (2.0f)));

    // Sweep path (existing analysis)
    auto drySweep = generateSweep (sr, 20.0, 20000.0, 5.0, 0.5);
    auto wetSweep = drySweep;
    applyFilters (wetSweep, coeffs);
    const auto sweepResult = FreqResponse().analyze (drySweep, wetSweep, sr);

    // MLS path (new analysis)
    constexpr int kMlsLength = 16383;
    auto dryMls = generateMLS (sr, kMlsLength, 0.5);
    auto wetMls = dryMls;
    applyFilters (wetMls, coeffs);
    const auto mlsResult = FreqResponse().analyzeMLS (dryMls, wetMls, sr, kMlsLength);

    REQUIRE (!mlsResult.raw.empty());
    REQUIRE (mlsResult.sampleRate == Catch::Approx (sr));

    // Compare magnitudes on 100 Hz – 10 kHz (both estimate the same LTI)
    for (const auto& p : mlsResult.raw)
    {
        if (p.frequency < 100.0 || p.frequency > 10000.0)
            continue;
        const double sweepMag = interpMagDB (sweepResult.raw, p.frequency);
        INFO ("f = " << p.frequency << " Hz, mls = " << p.magnitudeDB
              << " dB, sweep = " << sweepMag << " dB");
        REQUIRE (std::abs (p.magnitudeDB - sweepMag) < 0.5);
    }
}

//==============================================================================
// Test: MLS identity — dry == wet gives flat 0 dB (no filtering).
TEST_CASE ("FreqResponse: analyzeMLS identity gives 0dB flat response", "[freqresponse][mls]")
{
    const double sr = 48000.0;
    constexpr int kMlsLength = 16383;

    auto dry = generateMLS (sr, kMlsLength, 0.5);
    auto wet = dry;   // no filtering

    const auto result = FreqResponse().analyzeMLS (dry, wet, sr, kMlsLength);

    REQUIRE (!result.raw.empty());
    auto mid = pointsInRange (result.raw, 100.0, 10000.0);
    REQUIRE (mid.size() > 50);
    REQUIRE (meanAbsMagDB (mid) < 0.5);
    REQUIRE (meanAbsPhaseDeg (mid) < 3.0);
    REQUIRE (!result.smoothed_1_12.empty());
    REQUIRE (!result.smoothed_1_3.empty());
}
```

### Step 3：跑测试确认 RED

`unit_tests.exe "[freqresponse][mls]"` → 编译失败（analyzeMLS 未声明）——RED 证据。

### Step 4（GREEN）：实现 analyzeMLS + 抽取公共辅助

```cpp
// FreqResponse.cpp —— 从 analyze() 抽 applySmoothing（原 30-65 行内联代码迁移）:
void FreqResponse::applySmoothing (const std::vector<Point>& raw, double sr, int fftSize,
                                   Result& result) const
{
    if (raw.empty())
        return;
    std::vector<float> mags (raw.size());
    std::vector<float> freqs (raw.size());
    for (size_t i = 0; i < raw.size(); ++i)
    {
        mags[i] = std::pow (10.0f, static_cast<float> (raw[i].magnitudeDB / 20.0));
        freqs[i] = static_cast<float> (raw[i].frequency);
    }
    const double freqStep = sr / fftSize;
    auto mags12 = mags;
    MathUtils::smoothOctave (mags12.data(), freqStep, (int) mags12.size(), 12);
    for (size_t i = 0; i < raw.size(); ++i)
    {
        Point p;
        p.frequency = raw[i].frequency;
        p.magnitudeDB = MathUtils::amplitudeToDB (mags12[i]);
        p.phaseDeg = raw[i].phaseDeg;
        result.smoothed_1_12.push_back (p);
    }
    auto mags3 = mags;
    MathUtils::smoothOctave (mags3.data(), freqStep, (int) mags3.size(), 3);
    for (size_t i = 0; i < raw.size(); ++i)
    {
        Point p;
        p.frequency = raw[i].frequency;
        p.magnitudeDB = MathUtils::amplitudeToDB (mags3[i]);
        p.phaseDeg = raw[i].phaseDeg;
        result.smoothed_1_3.push_back (p);
    }
}

// 从 processChannel 末尾（161-193 行）抽 applyPhasePost:
void FreqResponse::applyPhasePost (std::vector<Point>& points, double sampleRate) const
{
    if (points.size() >= 2)
    {
        double cumulativeOffset = 0.0;
        double prevRaw = points[0].phaseDeg;
        for (size_t i = 1; i < points.size(); ++i)
        {
            const double raw  = points[i].phaseDeg;
            const double diff = raw - prevRaw;
            if (diff > 180.0)       cumulativeOffset -= 360.0;
            else if (diff < -180.0) cumulativeOffset += 360.0;
            points[i].phaseDeg = raw + cumulativeOffset;
            prevRaw = raw;
        }
    }
    if (latencySamples > 0)
    {
        const double coeff = 360.0 * static_cast<double> (latencySamples) / sampleRate;
        for (auto& p : points)
            p.phaseDeg += p.frequency * coeff;
    }
}

// 新增 analyzeMLS:
FreqResponse::Result FreqResponse::analyzeMLS (const juce::AudioBuffer<float>& dry,
                                               const juce::AudioBuffer<float>& wet,
                                               double sr, int mlsLength)
{
    Result result;
    result.sampleRate = sr;

    // FFT size = 2 * mlsLength (zero-padded linear convolution, avoids
    // circular aliasing); mlsLength 16383 → fftSize 32768 → order 15.
    const int numSamples = juce::jmin (dry.getNumSamples(), wet.getNumSamples(),
                                       mlsLength * 2);
    int fftOrder = 1;
    while ((1 << fftOrder) < mlsLength * 2) ++fftOrder;
    const int fftSize = 1 << fftOrder;
    const int numBins = fftSize / 2 + 1;
    const double freqStep = sr / fftSize;

    const float* dryData = dry.getReadPointer (0);
    const float* wetData = wet.getReadPointer (0);

    // Whole-segment FFT (no window: MLS is a periodic signal — the DFT of a
    // full period is the exact circular spectrum, no leakage).
    FftHelper fft (fftOrder);
    std::vector<float> dryWin (fftSize, 0.0f), wetWin (fftSize, 0.0f);
    for (int i = 0; i < numSamples; ++i)
    {
        dryWin[i] = dryData[i];
        wetWin[i] = wetData[i];
    }
    std::vector<float> dryReal (numBins), dryImag (numBins);
    std::vector<float> wetReal (numBins), wetImag (numBins);
    fft.forwardReal (dryWin.data(), dryReal.data(), dryImag.data(), false);
    fft.forwardReal (wetWin.data(), wetReal.data(), wetImag.data(), false);

    // H(f) = Y(f) / X(f), bin-wise complex division.
    const double lowEnergyThreshold = static_cast<double> (mlsLength) * 1e-8;
    std::vector<Point> points;
    for (int bin = 0; bin < numBins; ++bin)
    {
        const double freq = bin * freqStep;
        if (freq < 20.0 || freq > 20000.0)
            continue;

        const double Xr = dryReal[bin], Xi = dryImag[bin];
        const double Yr = wetReal[bin], Yi = wetImag[bin];
        const double energy = Xr * Xr + Xi * Xi;
        if (energy < lowEnergyThreshold)
            continue;

        const std::complex<double> H ((Yr * Xr + Yi * Xi) / energy,
                                      (Yi * Xr - Yr * Xi) / energy);
        Point p;
        p.frequency   = freq;
        p.magnitudeDB = 20.0 * std::log10 (std::max (std::abs (H), 1e-15));
        p.phaseDeg    = std::atan2 (H.imag(), H.real())
                        * 180.0 / juce::MathConstants<double>::pi;
        points.push_back (p);
    }

    applyPhasePost (points, sr);
    result.raw = points;
    applySmoothing (points, sr, fftSize, result);
    return result;
}

// analyze() 主体改为调用抽出的辅助（行为不变，现有 [freqresponse] 测试锁定）:
//   result.raw = points;
//   applySmoothing (points, sr, fftSize, result);   // 替换原内联 30-65 行
// processChannel 末尾 161-193 行替换为: applyPhasePost (points, sampleRate);
```

### Step 5：跑测试确认 GREEN

`unit_tests.exe "[freqresponse]"` → 全部通过（含新增 2 个 MLS 用例 + 既有回归——重构抽取不得破坏 H1 路径）。

### Step 6：MeasurementSession 接线

```cpp
// MeasurementSession.h public:
    /** Select the frequency-response excitation: true = MLS (fast, full-band),
     *  false = sine sweep (default, backward compatible). */
    void setFreqExcitation (bool useMLS) { freqExcitationMLS = useMLS; }
// private: bool freqExcitationMLS = false;   // + .cpp #include "../signal/Impulse.h"

// MeasurementSession.cpp run() 的 Type::frequencyResponse 分支改为:
                case Type::frequencyResponse:
                {
                    if (freqExcitationMLS)
                    {
                        auto mls = std::make_unique<Impulse>();
                        mls->useMLS (true);
                        mls->setMLSLength (16383);   // 0.34 s @48k vs 5 s sweep
                        mls->setAmplitude (0.5);
                        gen = std::move (mls);
                    }
                    else
                    {
                        auto sweep = std::make_unique<SineSweep>();
                        sweep->setFrequencyRange (20.0, 20000.0);
                        sweep->setDuration (5.0);
                        sweep->setAmplitude (0.5);
                        gen = std::move (sweep);
                    }
                    break;
                }
```

### Step 7：Protocol + CommandParser 四件套

```cpp
// Protocol.h 新增:
    /** Frequency-response excitation. */
    namespace Excitation
    {
        constexpr auto sweep = "sweep";   // default (backward compatible)
        constexpr auto mls   = "mls";
    }

// CommandParser.cpp —— measure case（参考现有 parseSource 模式，约 534 行附近）:
    // ...现有 source 解析之后:
    const juce::String excitationStr =
        obj->hasProperty ("excitation") ? obj->getProperty ("excitation").toString()
                                        : juce::String (Protocol::Excitation::sweep);
    if (excitationStr == Protocol::Excitation::mls)
        session->setFreqExcitation (true);
    else if (excitationStr == Protocol::Excitation::sweep)
        session->setFreqExcitation (false);
    else
    {
        error = "unknown excitation '" + excitationStr + "' (expected sweep|mls)";
        return Protocol::makeResponse (false, R"("error":")" + escapeJsonString (error) + R"(")");
    }

// dataset 的 freq 块（run 结构约 865 行处）同样解析透传（保持 battery 语义，
// 缺省 sweep；同一 dataset 全用同一 excitation）。
```

### Step 8：CommandParserTests 解析用例

```cpp
TEST_CASE ("CommandParser: measure accepts excitation mls and routes to session",
           "[commandparser][measure][excitation]")
{
    ensureMessageManager();
    auto plugin = std::make_unique<TestPlugin>();
    plugin->setGain (1.0);
    MeasurementSession session;
    session.setPluginInstance (plugin.get());
    session.setSampleRate (44100.0);
    session.setBlockSize (256);
    session.setMeasurementType (MeasurementSession::Type::frequencyResponse);
    CommandParser parser;
    parser.setPluginInstance (plugin.get());
    parser.setSession (&session);

    // MLS 激励：录制时长 = MLS 16383 样本 ≈ 0.37s，远短于 sweep 5s
    auto t0 = juce::Time::getMillisecondCounter();
    auto response = parser.handleCommand (
        R"({"cmd":"measure","type":"frequency_response","excitation":"mls"})");
    const auto elapsedMs = juce::Time::getMillisecondCounter() - t0;
    REQUIRE (response.contains ("\"ok\":true"));
    REQUIRE (elapsedMs < 4000);   // MLS 明显快于 5s sweep + 分析
}

TEST_CASE ("CommandParser: measure rejects unknown excitation",
           "[commandparser][measure][excitation]")
{
    // ...同 Arrange...
    auto response = parser.handleCommand (
        R"({"cmd":"measure","type":"frequency_response","excitation":"fancy"})");
    REQUIRE (response.contains ("\"ok\":false"));
    REQUIRE (response.contains ("excitation"));
}

TEST_CASE ("CommandParser: measure defaults to sweep excitation",
           "[commandparser][measure][excitation]")
{
    // ...同 Arrange（不传 excitation）...
    auto response = parser.handleCommand (R"({"cmd":"measure","type":"frequency_response"})");
    REQUIRE (response.contains ("\"ok\":true"));
}
```

### Step 9：全量 ctest 连跑 2 次

### Step 10：真机验收（IPC，无鼠标）

```powershell
# 1. MLS 测量
# {"cmd":"loadPlugin","path":"C:\Program Files\Common Files\VST3\FabFilter\FabFilter Pro-Q 4.vst3"}
# {"cmd":"measure","type":"frequency_response","excitation":"mls","path":"D:\...\out\e1-mls.json"}
# 2. 同参数 sweep 测量 → out\e1-sweep.json
# 3. tools/compare_freq.py（新写，或扩展 verify_export.py）比对两文件 magnitude，
#    100Hz-10kHz 平均 |Δ| < 0.5dB
# 4. 日志无 ERROR
```

### Step 11：文档同步 + 提交

```bash
git add source/analysis/FreqResponse.cpp source/analysis/FreqResponse.h \
        source/capture/MeasurementSession.cpp source/capture/MeasurementSession.h \
        source/ipc/Protocol.h source/ipc/CommandParser.cpp \
        tests/FreqResponseTests.cpp tests/CommandParserTests.cpp \
        docs/data-schema.md source/analysis/AGENTS.md source/capture/AGENTS.md source/ipc/AGENTS.md
git commit -m "feat(analysis): MLS excitation for frequency response (deconvolution path)"
git push origin main
```

---

## 任务 E2：MultiTone 随机初始相位（固定种子可复现）

**背景**：`MultiTone::generate` 各频点同相起振（`sin(2πft)`），多音叠加峰值因子（crest factor）高——数模级联易削波。加确定性随机初始相位可显著降 CF；固定种子保证同参数可复现（测量可比性）。**seed 0 必须字节级保持旧波形**（默认，向后兼容）。

**Files:**
- Modify: `source/signal/MultiTone.h/.cpp`
- Create: `tests/MultiToneTests.cpp`（**新建**，tests/CMakeLists.txt target_sources 加 `MultiToneTests.cpp`；根 CMakeLists 已含 MultiTone.cpp 无需动）
- Docs: `source/signal/AGENTS.md`（关键参数表补相位行）

**Interfaces:**
- `void MultiTone::setRandomPhaseSeed (uint32_t seed)`——0（默认）= 全零相位（旧波形不变）；非 0 = xorshift32 确定性 PRNG 生成每频点 `[0, 2π)` 初始相位，`prepare()` 时填充 `phases[]`

### Step 1（RED）：新建 MultiToneTests.cpp + 注册 CMakeLists

```cpp
// tests/MultiToneTests.cpp
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include "../source/signal/MultiTone.h"
#include <cmath>

//==============================================================================
// Helper: generate the full multitone buffer into a mono buffer.
static juce::AudioBuffer<float> generateTone (MultiTone& mt, double sr)
{
    const int totalSamples = static_cast<int> (mt.getTotalLength());
    juce::AudioBuffer<float> buf (1, totalSamples);
    buf.clear();
    mt.generate (buf, 0, totalSamples);
    return buf;
}

//==============================================================================
// Helper: crest factor = peak / RMS (linear).
static double crestFactor (const juce::AudioBuffer<float>& buf)
{
    const float* d = buf.getReadPointer (0);
    const int n = buf.getNumSamples();
    double peak = 0.0, sumSq = 0.0;
    for (int i = 0; i < n; ++i)
    {
        peak = std::max (peak, std::abs ((double) d[i]));
        sumSq += (double) d[i] * d[i];
    }
    const double rms = std::sqrt (sumSq / n);
    return peak / std::max (rms, 1e-12);
}

//==============================================================================
TEST_CASE ("MultiTone: random phase seed lowers crest factor vs zero phase",
           "[multitone][phase]")
{
    MultiTone mt;
    mt.setFrequencies ({ 100.0, 200.0, 400.0, 800.0, 1600.0, 3200.0, 6400.0, 12800.0 });
    mt.setDuration (1.0);
    mt.setAmplitude (0.3);

    mt.setRandomPhaseSeed (0);          // legacy all-zero phase
    mt.prepare (48000.0, 512);
    const auto zeroBuf = generateTone (mt, 48000.0);
    const double zeroCF = crestFactor (zeroBuf);

    mt.setRandomPhaseSeed (42);
    mt.prepare (48000.0, 512);
    const auto randBuf = generateTone (mt, 48000.0);
    const double randCF = crestFactor (randBuf);

    INFO ("zero-phase CF = " << zeroCF << ", random-phase CF = " << randCF);
    REQUIRE (randCF < zeroCF);      // crest factor dropped
    REQUIRE (randCF < 4.0);         // 8-tone random-phase CF ≈ 3.2-3.6
}

TEST_CASE ("MultiTone: same seed reproduces identical waveform", "[multitone][phase]")
{
    MultiTone mt;
    mt.setFrequencies ({ 100.0, 1000.0, 5000.0 });
    mt.setDuration (0.1);
    mt.setAmplitude (0.3);

    juce::AudioBuffer<float> a, b;
    mt.setRandomPhaseSeed (7); mt.prepare (48000.0, 512); a = generateTone (mt, 48000.0);
    mt.setRandomPhaseSeed (7); mt.prepare (48000.0, 512); b = generateTone (mt, 48000.0);

    REQUIRE (a.getNumSamples() == b.getNumSamples());
    for (int i = 0; i < a.getNumSamples(); ++i)
        REQUIRE (a.getSample (0, i) == b.getSample (0, i));   // bit-identical
}

TEST_CASE ("MultiTone: different seeds produce different waveforms", "[multitone][phase]")
{
    MultiTone mt;
    mt.setFrequencies ({ 100.0, 1000.0, 5000.0 });
    mt.setDuration (0.1);
    mt.setAmplitude (0.3);

    juce::AudioBuffer<float> a, b;
    mt.setRandomPhaseSeed (7);  mt.prepare (48000.0, 512); a = generateTone (mt, 48000.0);
    mt.setRandomPhaseSeed (99); mt.prepare (48000.0, 512); b = generateTone (mt, 48000.0);

    bool differs = false;
    for (int i = 0; i < a.getNumSamples(); ++i)
        if (a.getSample (0, i) != b.getSample (0, i)) { differs = true; break; }
    REQUIRE (differs);
}

TEST_CASE ("MultiTone: seed 0 keeps the legacy zero-phase waveform", "[multitone][phase]")
{
    // Without setRandomPhaseSeed (default = 0), sample values equal sin-sum.
    MultiTone mt;
    mt.setFrequencies ({ 100.0, 300.0, 500.0 });
    mt.setDuration (0.1);
    mt.setAmplitude (0.3);
    mt.prepare (48000.0, 512);
    const auto buf = generateTone (mt, 48000.0);

    // Spot-check against the closed form sin(2πft) with no phase offset:
    const double sr = 48000.0;
    for (int i = 0; i < 2000; i += 100)
    {
        double expected = 0.0;
        for (double f : { 100.0, 300.0, 500.0 })
            expected += std::sin (2.0 * juce::MathConstants<double>::pi * f * i / sr);
        expected *= 0.3 / 3.0;
        REQUIRE (buf.getSample (0, i) == Catch::Approx (expected).margin (1e-6));
    }
}
```

### Step 2：跑测试确认 RED

编译失败（setRandomPhaseSeed 未声明）→ RED 证据。

### Step 3（GREEN）：实现

```cpp
// MultiTone.h public:
    /** Deterministic random initial phases (one per frequency) to lower the
     *  crest factor of the summed multi-tone. Seed 0 (default) keeps the
     *  legacy all-zero-phase waveform; any non-zero seed reproduces the same
     *  waveform on every run (xorshift32, deterministic). */
    void setRandomPhaseSeed (uint32_t seed) { phaseSeed = seed; }
// private: uint32_t phaseSeed = 0; std::vector<double> phases;

// MultiTone.cpp prepare():
    SignalGenerator::prepare (sr, bs);
    phases.resize (frequencies.size());
    if (phaseSeed != 0)
    {
        uint32_t state = phaseSeed;
        const auto nextRand = [&state]()
        {
            state ^= state << 13;
            state ^= state >> 17;
            state ^= state << 5;
            return state;
        };
        for (auto& ph : phases)
            ph = 2.0 * juce::MathConstants<double>::pi
                 * (static_cast<double> (nextRand()) / 4294967296.0);
    }
    else
    {
        std::fill (phases.begin(), phases.end(), 0.0);
    }

// generate() 内循环改一行:
            double phase = 2.0 * juce::MathConstants<double>::pi * freq * t + phases[i];
```

### Step 4：跑测试确认 GREEN

`unit_tests.exe "[multitone]"` → 4 用例全过 + 既有 MultiTone 依赖回归（harmonic 相关测试，seed 0 路径不变）。

### Step 5：全量 ctest + 提交

```bash
git add source/signal/MultiTone.cpp source/signal/MultiTone.h tests/MultiToneTests.cpp tests/CMakeLists.txt source/signal/AGENTS.md
git commit -m "feat(signal): deterministic random initial phases for MultiTone (lower crest factor)"
git push origin main
```

---

## 任务 E3：runMultiple 多轮参数扫描接口（重新定性为验证型）

**现状核实（2026-08-08）**：roadmap 原任务"`MeasurementSession::runMultiple` 多轮参数扫描接口"——**已被 `ScanEngine::run` 完整覆盖**（阶段 3 交付）：参数+档位数组 → 每轮复用同一 MeasurementSession → 曲线族 `ScanResult.family[]` + progress 回调 + 取消 + 参数快照/恢复 RAII。块 A（批量采集）基于 ScanEngine 完成，roadmap 原文"块 A 可选复用"已兑现。

**本任务无新增代码**，只有验证 + 文档：

### Step 1：验证清单（对照 roadmap 意图逐项核对）

| roadmap 意图 | ScanEngine 现状 | 结论 |
|---|---|---|
| 多轮参数扫描（参数+档位→多轮测量） | `run(paramId, values[], type, progress)` 每值一轮 | ✅ 覆盖 |
| 曲线族/数据面 | `ScanResult.family[]`（每轮 freq/harmonic/compression + latency + cancelled） | ✅ 覆盖 |
| 取消 | `cancel()` 线程安全，round 边界生效 | ✅ 覆盖 |
| 参数快照/恢复 | RAII guard（entry 快照全部参数，exit 恢复含取消） | ✅ 覆盖 |
| 进度 | `progress(round, totalRounds)` 每轮后回调 | ✅ 覆盖 |
| 块 A 复用 | dataset 命令内部用 ScanEngine（docs/plan-batch-pipeline.md S1/S4 真机通过） | ✅ 兑现 |

### Step 2：确认无缺口 → 文档标记完成

```bash
git add STATUS.md docs/roadmap-next.md
git commit -m "docs: mark runMultiple covered by ScanEngine (verification-type task E3)"
git push origin main
```

> 若审查发现"同参数多次测量取平均/统计"确为原意（roadmap 未明示），则另开 brainstorming 定案后补 `ScanEngine::runRepeated`——本任务不猜。

---

## 收尾：全量回归 + 审查 + push

- [ ] **Step 1：全量 ctest 连跑 2 次**（记录输出）
- [ ] **Step 2：双轴代码审查（requesting-code-review）**——Standards + Spec 并行；固定点 = E 块首个 commit 的 parent
- [ ] **Step 3：真机冒烟**——MLS 频响 + 扫频频响 IPC 各 1 次对比（平均 |Δ| < 0.5dB）；harmonic 测量（MultiTone 默认 seed 0 回归）1 次
- [ ] **Step 4：文档同步**（AGENTS.md 计数、roadmap 执行状态、STATUS 进度）
- [ ] **Step 5：最终 push origin main**

## 风险

| # | 风险 | 等级 | 缓解 |
|---|------|------|------|
| R1 | MLS 频域除法低频 bin 信噪比差（dry 能量低处除以近零） | P1 | 低能量保护阈值（mlsLength×1e-8）跳过近零 bin；测试限 100Hz 以上 |
| R2 | MLS 循环卷积混叠（周期不足） | P1 | FFT size = 2×mlsLength 补零线性卷积；一致性测试锁定 |
| R3 | MultiTone 随机相位影响 harmonicAnalysis 基频取峰（相位改变峰形） | P2 | 分析器逐基频独立取峰（幅度域）；默认 seed 0 路径不变，现有测试零影响 |
| R4 | 重构抽取（applySmoothing/applyPhasePost）破坏 H1 路径 | P1 | 既有 [freqresponse] 用例回归锁定；先 GREEN 后重构（重构时测试已绿） |
| R5 | E3 范围蔓延 | P2 | 验证型先行；原意歧义不猜，另开 brainstorming |
