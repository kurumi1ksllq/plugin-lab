# PluginLab 导出 JSON Schema 文档

> 2026-08-02（T5.2）编写。定义 `source/analysis/Export.h` 导出的全部 JSON 文档结构。
> 消费者：AI 建模（DESIGN.md §9——用测量数据复刻插件行为）。所有导出都必须能被
> `juce::JSON::parse` 与 python `json.load` 解析，且携带完整测量上下文以便复现。
> 实现与测试：`source/analysis/Export.cpp`、`tests/ExportTests.cpp`（测试锁定字段与精度）。
>
> 2026-08-04 变更记录：新增 IPC 协议命令 `getScanStatus`（扫描状态快照，见
> `source/ipc/AGENTS.md` 协议表）——属**协议命令**而非导出文档，不入 §9 导出 schema。

## 导出类型一览

| type 值              | 导出函数                  | 覆盖建模维度                              |
| -------------------- | ------------------------- | ----------------------------------------- |
| `frequency_response` | `freqResponseToJSON`      | freq                                      |
| `harmonic_analysis`  | `harmonicAnalysisToJSON`  | freq                                      |
| `compression_curve`  | `compressionCurveToJSON`  | level                                     |
| `raw_capture`        | `rawCaptureToJSON`        | （原始录音，仅元数据）                    |
| `scan`               | `scanToJSON`              | param × (freq/level)                      |
| `gr_timeline`        | `grTimelineToJSON`        | time                                      |
| `compression_family` | `compressionFamilyToJSON` | level × time                              |
| `dataset`            | `datasetToJSON`           | param × level × freq × time（聚合，T5.1） |

---

## 通用约定

### 测量上下文（Context 块）

`frequency_response` / `harmonic_analysis` / `compression_curve` 的上下文字段平铺在顶层；
`scan` / `gr_timeline` / `compression_family` / `dataset` 的测量上下文字段收集在顶层 `"context": {...}`
对象中，数据块（`scan` / `family` / `gr` / `tau` 等）与 `context` 平级（sibling）。
`raw_capture` 只带 plugin/class_id/latency/parameter_snapshot/source。

| 字段                 | 类型   | 说明                                                                                                                                                          |
| -------------------- | ------ | ------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `plugin`             | string | 被测插件显示名                                                                                                                                                |
| `class_id`           | string | 插件 VST3 class id（识别精确实现）                                                                                                                            |
| `latency_samples`    | int    | 测量时观测的插件延迟（采样点）                                                                                                                                |
| `sample_rate`        | number | 测量采样率（Hz）                                                                                                                                              |
| `measurement`        | object | `sample_rate` + `block_size`（处理块大小）                                                                                                                    |
| `parameter_snapshot` | object | 测量时全部参数归一化值快照（可复现参数状态）。键为参数显示名（display name），非 param_id（param_id 经 getParams 响应暴露）                                   |
| `source`             | object | 输入源元数据：`type`（signal/noise/file/dynamic）、`file_path`、`sample_rate`、`resample_ratio`、`duration_sec`、`noise_type`、`seed`（固定种子，噪声可复现） |

**为什么需要**：AI 复刻插件行为时，同一插件的同一参数状态在不同采样率/块大小/源下表现不同；
上下文使数据包自洽——单文件即可精确复现测量条件。

### 数值精度

- 频率：1 位小数（`fmtDouble(v, 1)`）；幅度/相位/曲线：2 位小数。
- THD/谐波百分比：4 位小数；扫描参数值：6 位小数；τ：6 位小数；时间：4 位小数。
- 导出测试锁定"无精度损失往返"（`[export][precision]` 等）。
- 示例 JSON 中的数值为结构示意，实际输出精度以「数值精度」表为准。

---

## 1. frequency_response

```json
{
  "type": "frequency_response",
  "plugin": "MyPlugin",
  "class_id": "com.example.myplugin",
  "latency_samples": 64,
  "sample_rate": 48000.0,
  "measurement": { "sample_rate": 48000.0, "block_size": 512 },
  "parameter_snapshot": { "Gain": 0.5 },
  "source": { "type": "signal" },
  "raw": [
    { "f": 100.0, "mag": -3.0, "phase": 12.0 },
    { "f": 1000.0, "mag": 0.0, "phase": 0.0 }
  ],
  "smoothed_1_12": [{ "f": 100.0, "mag": -3.0, "phase": 12.0 }],
  "smoothed_1_3": [{ "f": 100.0, "mag": -3.0, "phase": 12.0 }]
}
```

| 字段            | 类型  | 说明                                                 |
| --------------- | ----- | ---------------------------------------------------- |
| `raw`           | array | 未平滑频响点 `{f, mag, phase}`（mag/phase 为 dB/度） |
| `smoothed_1_12` | array | 1/12 倍频程平滑点                                    |
| `smoothed_1_3`  | array | 1/3 倍频程平滑点                                     |

**建模用途**：EQ/滤波器系数拟合（频域响应 → 滤波器设计），覆盖 **freq** 维度。

---

## 2. harmonic_analysis

```json
{
  "type": "harmonic_analysis",
  "plugin": "MyPlugin",
  "class_id": "com.example.myplugin",
  "latency_samples": 64,
  "sample_rate": 48000.0,
  "measurement": { "sample_rate": 48000.0, "block_size": 512 },
  "parameter_snapshot": {},
  "source": { "type": "signal" },
  "tones": [
    {
      "fundamental_hz": 1000.0,
      "fundamental_db": -6.0,
      "thd_percent": 0.5,
      "harmonics": [
        { "order": 2, "freq": 2000.0, "mag_db": -40.0, "percent": 1.0 }
      ]
    }
  ]
}
```

| 字段                     | 类型   | 说明                                      |
| ------------------------ | ------ | ----------------------------------------- |
| `tones[].fundamental_hz` | number | 基频（Hz）                                |
| `tones[].fundamental_db` | number | 基频幅度（dB）                            |
| `tones[].thd_percent`    | number | 总谐波失真（%）                           |
| `tones[].harmonics[]`    | array  | `{order, freq, mag_db, percent}` 各次谐波 |

**建模用途**：非线性/饱和/失真特性量化，覆盖 **freq** 维度。

---

## 3. compression_curve

```json
{
  "type": "compression_curve",
  "plugin": "MyPlugin",
  "class_id": "com.example.myplugin",
  "latency_samples": 64,
  "sample_rate": 48000.0,
  "measurement": { "sample_rate": 48000.0, "block_size": 512 },
  "parameter_snapshot": {},
  "source": { "type": "signal" },
  "curve": [
    { "input_db": -60.0, "output_db": -60.0, "gr_db": 0.0 },
    { "input_db": -20.0, "output_db": -22.0, "gr_db": -2.0 }
  ],
  "fitted": { "ratio": 4.0, "threshold_db": -30.0, "knee_db": 3.0 }
}
```

| 字段      | 类型   | 说明                                              |
| --------- | ------ | ------------------------------------------------- |
| `curve[]` | array  | 静态压缩曲线点 `{input_db, output_db, gr_db}`     |
| `fitted`  | object | 最小二乘拟合参数 `{ratio, threshold_db, knee_db}` |

**建模用途**：压缩器静态特性拟合（阈值/比值/拐点），覆盖 **level** 维度。

---

## 4. raw_capture

```json
{
  "type": "raw_capture",
  "plugin": "MyPlugin",
  "class_id": "com.example.myplugin",
  "latency_samples": 0,
  "parameter_snapshot": {},
  "source": {
    "type": "file",
    "file_path": "C:\\audio\\take01.wav",
    "sample_rate": 48000.0,
    "resample_ratio": 1.0,
    "duration_sec": 17.0
  },
  "samples": 816000,
  "sample_rate": 48000.0,
  "block_size": 256
}
```

| 字段                         | 类型       | 说明          |
| ---------------------------- | ---------- | ------------- |
| `samples`                    | int        | 录音总采样数  |
| `sample_rate` / `block_size` | number/int | 录音/处理配置 |

**用途**：非信号源的原始录音元数据（原始音频不随 JSON 导出）；记录"从哪段音频、以何配置录得"。

### raw_capture 的伴随 WAV 镜像（CaptureBuffer 增量 flush）

`CaptureBuffer`（`source/capture/AudioBuffer.*`）支持把 dry/wet 录音增量镜像到 24-bit PCM WAV：采集期间每 `intervalSec` 写盘一次，插件崩溃（进程被杀）最多丢末段音频。

| 项         | 值                                                                                                                          |
| ---------- | --------------------------------------------------------------------------------------------------------------------------- |
| 格式       | 单文件交织 24-bit PCM；声道数 = 2 × 插件声道；布局 `[dry ch0..N-1, wet ch0..N-1]`（立体声插件 → 4 声道：dry L/R + wet L/R） |
| 头部       | 44 字节标准 RIFF，byteRate/blockAlign 按标准计算；每次 flush 边界回填尺寸 → 任意时刻文件有效可读                            |
| flush 间隔 | `setFlushConfig(path, intervalSec)` 由调用方给定；空 path 禁用（默认关闭）                                                  |
| 崩溃语义   | 已 flush 前缀在盘上且可读；末个间隔内的采样丢失（≤ intervalSec）；分析器仍以内存 dry/wet 为完整真值                         |
| 分析输入   | **不是**分析输入——内存 `getDryBuffer`/`getWetBuffer` 是唯一分析真值；WAV 仅作崩溃恢复 + 事后试听                            |
| 写盘实现   | 手写 RIFF 写入器（非 `juce::WavAudioFormatWriter`——其仅析构时 finalise 头部，崩溃后文件不可读）                             |

**✅ 接线状态（2026-08-03）**：flush 机制已在 CaptureBuffer 落地（`tests/CaptureBufferTests.cpp` 单测），并**已接入 IPC**——measure/scan 命令在运行前调用 `session->getResult().setFlushConfig (wavPathFor (exportPath), kDefaultFlushIntervalSec)`，成功响应携带 `wav_path` 字段（导出 JSON 路径 `.json→.wav` 派生，默认 flush 间隔 5s）。scan 多轮复用同一会话 → 每轮首 append 覆盖 .wav（最后轮胜出）。

---

## 5. scan

参数扫描（每参数值一轮测量），`family` 每项含该档分析结果。

```json
{
  "type": "scan",
  "context": {
    "plugin": "MyPlugin",
    "class_id": "com.example.myplugin",
    "latency_samples": 64,
    "sample_rate": 48000.0,
    "measurement": { "sample_rate": 48000.0, "block_size": 512 },
    "parameter_snapshot": { "Gain": 0.5 },
    "source": { "type": "signal" }
  },
  "scan": {
    "param_id": "gain",
    "param_name": "Gain",
    "values": [0.0, 0.5, 1.0],
    "param_texts": ["0.00 dB", "0.50 dB", "1.00 dB"]
  },
  "family": [
    {
      "param_value_normalized": 0.0,
      "param_value_text": "0.00 dB",
      "latency_samples": 32,
      "result": {
        "raw": [{ "f": 100.0, "mag": -3.0, "phase": 12.0 }],
        "smoothed_1_12": [],
        "smoothed_1_3": []
      }
    }
  ]
}
```

| 字段                              | 类型   | 说明                                                                                                                                                               |
| --------------------------------- | ------ | ------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| `scan.param_id` / `param_name`    | string | 扫描参数标识/显示名                                                                                                                                                |
| `scan.values`                     | array  | 归一化参数值（0..1）                                                                                                                                               |
| `scan.param_texts`                | array  | 每档参数实际值文本（与 `family[i]` 对齐）                                                                                                                          |
| `family[].param_value_normalized` | number | 该档归一化值（6 位小数）                                                                                                                                           |
| `family[].param_value_text`       | string | 该档实际值文本                                                                                                                                                     |
| `family[].latency_samples`        | int    | **该档**观测的插件延迟（随参数可变）                                                                                                                               |
| `family[].result`                 | object | 该档分析结果，按扫描类型取 body：<br>frequencyResponse → `raw`/`smoothed_1_12`/`smoothed_1_3`<br>harmonicAnalysis → `tones`<br>compressionCurve → `curve`/`fitted` |

**建模用途**：参数 → 响应的连续映射（AI 学习"参数怎么改、响应怎么变"），覆盖 **param × freq（或 level）** 维度。

---

## 6. gr_timeline

```json
{
  "type": "gr_timeline",
  "context": {
    "plugin": "MyPlugin",
    "class_id": "com.example.myplugin",
    "latency_samples": 0,
    "sample_rate": 48000.0,
    "measurement": { "sample_rate": 48000.0, "block_size": 512 },
    "parameter_snapshot": {},
    "source": { "type": "dynamic" }
  },
  "gr": {
    "sample_rate": 48000.0,
    "num_points": 3,
    "timeline": [
      { "t": 0.0, "gr_db": 0.0 },
      { "t": 0.005, "gr_db": -6.0 },
      { "t": 0.01, "gr_db": -6.0 }
    ]
  },
  "tau": {
    "attack_sec": 0.001,
    "release_sec": 0.05,
    "valid": true,
    "attack_by_level": [{ "level_db": -6.0, "tau_sec": 0.001 }],
    "release_by_level": [{ "level_db": -6.0, "tau_sec": 0.05 }]
  }
}
```

| 字段                                       | 类型       | 说明                                                              |
| ------------------------------------------ | ---------- | ----------------------------------------------------------------- |
| `gr.sample_rate` / `gr.num_points`         | number/int | 时间线采样率/点数                                                 |
| `gr.timeline[]`                            | array      | `{t, gr_db}` GR 随时间曲线；**GainReduction 约定：负值 = 压缩量** |
| `tau.attack_sec` / `release_sec`           | number     | 攻击/释放时间常数（秒，6 位小数）                                 |
| `tau.valid`                                | bool       | 是否检测到受控边沿（文件源等无边沿 → false + 全 0）               |
| `tau.attack_by_level` / `release_by_level` | array      | τ(level) 曲线族 `{level_db, tau_sec}`                             |

**建模用途**：动态处理器包络拟合（攻击/释放一阶极点 + 电平依赖），覆盖 **time** 维度。

---

## 7. compression_family

level × speed 网格（每格：静态曲线 + GR 时间线 + 时间常数）。

```json
{
  "type": "compression_family",
  "context": {
    "plugin": "MyPlugin",
    "class_id": "com.example.myplugin",
    "latency_samples": 0,
    "sample_rate": 48000.0,
    "measurement": { "sample_rate": 48000.0, "block_size": 512 },
    "parameter_snapshot": {},
    "source": { "type": "dynamic" }
  },
  "family": [
    {
      "input_level_db": -12.0,
      "speed": 1.0,
      "curve": [{ "input_db": -12.0, "output_db": -12.0, "gr_db": 0.0 }],
      "fitted": { "ratio": 2.0, "threshold_db": -12.0, "knee_db": 0.0 },
      "gr": {
        "sample_rate": 48000.0,
        "num_points": 2,
        "timeline": [
          { "t": 0.0, "gr_db": 1.0 },
          { "t": 0.005, "gr_db": 2.0 }
        ]
      },
      "tau": { "attack_sec": 0.001, "release_sec": 0.05 }
    }
  ]
}
```

| 字段                        | 类型   | 说明                                                                     |
| --------------------------- | ------ | ------------------------------------------------------------------------ |
| `family[].input_level_db`   | number | 该格输入电平（dBFS）                                                     |
| `family[].speed`            | number | 该格动态包络速度                                                         |
| `family[].curve` / `fitted` | object | 静态压缩曲线 + 拟合参数（同 §3 body）                                    |
| `family[].gr`               | object | GR 时间线（同 §6 gr body；**此处约定为正 dB = 压缩量**，与条目曲线一致） |
| `family[].tau`              | object | 该格攻击/释放 τ（attack_sec/release_sec，无 details）                    |

**建模用途**：压缩器电平依赖 + 速度依赖的完整行为网格（AI 插值任意 (level, speed) 格点），覆盖 **level × time** 维度。

---

## 8. dataset（建模数据包，T5.1）

**单一自洽 JSON**：整合参数扫描族 + 压缩响应族 + GR 时间线 + 拟合建议，一个文件包含 AI 建模所需的全部数据与元数据（DESIGN.md §9 核心需求）。

```json
{
  "type": "dataset",
  "context": {
    "plugin": "MyPlugin",
    "class_id": "com.example.myplugin",
    "latency_samples": 64,
    "sample_rate": 48000.0,
    "measurement": { "sample_rate": 48000.0, "block_size": 512 },
    "parameter_snapshot": { "Gain": 0.5 },
    "source": { "type": "signal" }
  },
  "note": "detected peak 993Hz +6.0dB -> likely bell @1k Q1",
  "scan": {
    "param_id": "gain",
    "param_name": "Gain",
    "values": [0.0, 0.5, 1.0],
    "param_texts": ["0.00 dB", "0.50 dB", "1.00 dB"],
    "family": [
      {
        "param_value_normalized": 0.0,
        "param_value_text": "0.00 dB",
        "latency_samples": 32,
        "result": {
          "raw": [{ "f": 100.0, "mag": -3.0, "phase": 12.0 }],
          "smoothed_1_12": [],
          "smoothed_1_3": []
        }
      }
    ]
  },
  "compression_family": {
    "family": [
      {
        "input_level_db": -12.0,
        "speed": 1.0,
        "curve": [{ "input_db": -12.0, "output_db": -12.0, "gr_db": 0.0 }],
        "fitted": { "ratio": 2.0, "threshold_db": -12.0, "knee_db": 0.0 },
        "gr": {
          "sample_rate": 48000.0,
          "num_points": 2,
          "timeline": [{ "t": 0.0, "gr_db": 1.0 }]
        },
        "tau": { "attack_sec": 0.001, "release_sec": 0.05 }
      }
    ]
  },
  "gr_timeline": {
    "gr": {
      "sample_rate": 48000.0,
      "num_points": 3,
      "timeline": [
        { "t": 0.0, "gr_db": 0.0 },
        { "t": 0.005, "gr_db": -6.0 }
      ]
    },
    "tau": {
      "attack_sec": 0.002,
      "release_sec": 0.04,
      "valid": true,
      "attack_by_level": [],
      "release_by_level": []
    }
  }
}
```

| 字段                 | 类型           | 说明                                                                                                    |
| -------------------- | -------------- | ------------------------------------------------------------------------------------------------------- |
| `type`               | string         | 固定 `"dataset"`                                                                                        |
| `context`            | object         | 完整测量上下文（同 §5-7 嵌套格式）                                                                      |
| `note`               | string（可选） | 拟合建议/备注（如峰值检测提示），未提供则省略                                                           |
| `scan`               | object（可选） | 参数扫描块：`param_id`/`param_name`/`values`/`param_texts` + **`family` 内嵌**（与 §5 family 布局一致） |
| `compression_family` | object（可选） | 压缩响应族块：`family`（与 §7 条目布局一致）                                                            |
| `gr_timeline`        | object（可选） | GR 块：`gr` + `tau`（与 §6 body 一致，含 valid/details）                                                |

**可选语义**：`Dataset` 各字段均为"可选测量"——调用方只填已完成的测量，未提供项在 JSON 中**整体省略**（空 dataset 仅含 `type` + `context`）。`grTau` 与 `grTimeline` 同现。

### dataset 如何支撑 AI 建模（自洽性）

1. **一个文件、零外部依赖**：`context` 内嵌插件标识、延迟、采样率/块大小、参数快照与输入源元数据——消费方无需查询任何外部状态即可精确复现测量条件。
2. **四维覆盖**：
   - **param**：`scan.values` + `family[].param_value_*`（参数 → 响应映射）
   - **level**：`compression_family` 每格的 `input_level_db` + `curve`（电平依赖静态特性）
   - **freq**：`scan.family[].result`（freq 分析随参数变化）
   - **time**：`gr_timeline.gr.timeline` + `tau`（动态包络）
3. **数据一致性保证**：dataset 的每个 body 与独立导出（scanToJSON / compressionFamilyToJSON / grTimelineToJSON）**逐数据等价**（测试 `[export][dataset-body-equiv]` 锁定）——AI 可放心把 dataset 当作独立导出的并集使用。
4. **拟合建议**：`note` 携带测量工具自动检测的线索（如峰值位置），引导建模优先级。

---

## 回归保障

- 全部导出测试：`tests/ExportTests.cpp`（schema、精度、转义、python 可解析性、body 等价、dataset 聚合）。
- 现有独立导出的输出格式由既有 schema 测试逐字节/逐数据锁定；dataset 聚合**只复用 body 序列化函数，不触碰独立导出函数**（策略 b）。
- 验证：`ctest --test-dir build -C Debug --timeout 180` 全绿（113 项）。
