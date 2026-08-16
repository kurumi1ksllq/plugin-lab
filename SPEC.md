# PluginLab — 工程文档（SPEC）

> **定位**（2026-08-11 定）：本地三文档体系之一——`SPEC.md`（工程：8 类导出 JSON schema + IPC 协议契约，原 `docs/data-schema.md`）、`DESIGN.md`（设计）、`STATUS.md`（状态）；`AGENTS.md` 为开发约束。需求与待开发走 GitHub issue。
>
> 2026-08-02（T5.2）编写。定义 `source/analysis/Export.h` 导出的全部 JSON 文档结构。
> 消费者：开发者/建模——用测量数据反推插件处理方式、开发效果一致的插件（DESIGN.md 开发目标）。
> 所有导出都必须能被
> `juce::JSON::parse` 与 python `json.load` 解析，且携带完整测量上下文以便复现。
> 实现与测试：`source/analysis/Export.cpp`、`tests/ExportTests.cpp`（测试锁定字段与精度）。
>
> 2026-08-14 变更记录：新增 §11「处理顺序反推（processing order 探测）」——黑盒判定 EQ/压缩器
> 模块顺序的设计契约（未实现，待开 issue）；产出并入 chain_doc `processing_order`，不新增导出
> schema 类型（复用 §5 scan + §6 gr_timeline + §7 compression_family 数据）。
>
> 2026-08-14 定位更新：本文件（测量导出 8 类 JSON + WAV）是"可带走开发规格"的**数据侧**；
> 反推结论侧（chain_doc 处理链路描述）契约见 `tools/describe_schema.py`（`CONTRACT_VERSION="2"`，
> 由 `describe_chain.py` 生成）——两者合起来构成交付给开发者的完整规格，见 DESIGN.md 开发目标「规格交付链路」。
>
> 2026-08-04 变更记录：新增 IPC 协议命令 `getScanStatus`（扫描状态快照，见
> `source/ipc/AGENTS.md` 协议表）——属**协议命令**而非导出文档，不入 §9 导出 schema。
>
> 2026-08-08 变更记录：新增 IPC 协议命令 `dataset`（批量采集 battery，默认跑全部 4 类
> 测量，见 `source/ipc/AGENTS.md` 协议表）——导出 §8 dataset 文档（type "dataset"）；
> §8 文档在 scan / compression_family / gr_timeline 之外新增可选 frequency_response /
> harmonic / compression 三个测量块。battery 校验：逐类型成败记录在命令响应 "types"
> 对象，不入文档（未跑/失败的块整体省略）。
>
> 2026-08-08 核对记录（块 C 任务 6）：§5 scan 结构描述与 `Export::scanToJSON` 实现
> **最终核对一致**——顶层 `scan{param_id, param_name, values, param_texts}`、
> context 七字段（plugin/class_id/latency_samples/sample_rate/measurement/
> parameter_snapshot/source）、`family[{param_value_normalized, param_value_text,
> latency_samples, result}]`、精度（values 6 位）、dataset 内嵌 family 布局全部一致；
> `tests/ExportTests.cpp [export][scan-schema]` 锁定（含 context 顶层 sample_rate /
> source 断言，30 断言）。
>
> 2026-08-08 变更记录（块 E 任务 1）：`measurement` 块新增可选 `excitation`
> 字段（`"sweep"` 缺省 / `"mls"`）——记录频响测量所用激励（MLS 频域除法解卷积 vs
> 扫频 H1 估计），仅非缺省值（mls）时输出，缺省 sweep 导出保持字节不变；
> `tests/CommandParserTests.cpp [commandparser][measure][excitation]` 锁定。
>
> 2026-08-10 变更记录（块 B 任务 1）：新增 IPC 协议命令 `exportWav`——把**最后一次
> 测量**的 dry/wet（+ bypass = dry 副本）导出为单个 24-bit PCM WAV 文件。二进制导出，
> 不入 §9 JSON schema，命令与文件布局见 §9「exportWav 命令（WAV 导出）」。
>
> 2026-08-10 变更记录（块 B 任务 2）：新增 IPC 协议命令 `recordTimeline` /
> `stopTimeline` / `playTimeline`——参数自动化（automation）的录制与回放。录制产物为
> `parameter_timeline` JSON（见 §10），回放产物为 `parameter_timeline_play` JSON +
> dry/wet WAV。属协议命令 + 新导出文档，见 §10。

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
`raw_capture` 只带元数据：plugin/class_id/latency/parameter_snapshot/source/samples/sample_rate/block_size（原始音频不随 JSON 导出）。

| 字段                 | 类型   | 说明                                                                                                                                                          |
| -------------------- | ------ | ------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `plugin`             | string | 被测插件显示名                                                                                                                                                |
| `class_id`           | string | 插件 VST3 class id（识别精确实现）                                                                                                                            |
| `latency_samples`    | int    | 测量时观测的插件延迟（采样点）                                                                                                                                |
| `sample_rate`        | number | 测量采样率（Hz）                                                                                                                                              |
| `measurement`        | object | `sample_rate` + `block_size`（处理块大小）+ `excitation`（可选，频响激励：`sweep` 缺省 / `mls`，仅非缺省时输出）                                                        |
| `parameter_snapshot` | object | 测量时全部参数归一化值快照（可复现参数状态）。键为参数显示名（display name），非 param_id（param_id 经 getParams 响应暴露）                                   |
| `source`             | object | 输入源元数据：`type`（signal/noise/file/dynamic）、`file_path`、`sample_rate`、`resample_ratio`、`duration_sec`、`noise_type`、`seed`（固定种子，噪声可复现） |

**为什么需要**：开发者开发效果一致的插件时，同一插件的同一参数状态在不同采样率/块大小/源下表现不同；
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

**单一自洽 JSON**：整合参数扫描族 + 压缩响应族 + GR 时间线 + 拟合建议，一个文件包含开发者建模所需的全部数据与元数据（DESIGN.md 开发目标核心需求）。

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
| `frequency_response` | object（可选） | 频响块：`raw`/`smoothed_1_12`/`smoothed_1_3`（与 §1 body 一致）                                   |
| `harmonic`           | object（可选） | 谐波块：`tones`（与 §2 body 一致）                                                                |
| `compression`        | object（可选） | 压缩块：`curve`/`fitted`（与 §3 body 一致）                                                       |
| `scan`               | object（可选） | 参数扫描块：`param_id`/`param_name`/`values`/`param_texts` + **`family` 内嵌**（与 §5 family 布局一致） |
| `compression_family` | object（可选） | 压缩响应族块：`family`（与 §7 条目布局一致）                                                            |
| `gr_timeline`        | object（可选） | GR 块：`gr` + `tau`（与 §6 body 一致，含 valid/details）                                                |

**可选语义**：`Dataset` 各字段均为"可选测量"——调用方只填已完成的测量，未提供项在 JSON 中**整体省略**（空 dataset 仅含 `type` + `context`）。`grTau` 与 `grTimeline` 同现。

### dataset 如何支撑开发者建模（自洽性）

1. **一个文件、零外部依赖**：`context` 内嵌插件标识、延迟、采样率/块大小、参数快照与输入源元数据——消费方无需查询任何外部状态即可精确复现测量条件。
2. **四维覆盖**：
   - **param**：`scan.values` + `family[].param_value_*`（参数 → 响应映射）
   - **level**：`compression_family` 每格的 `input_level_db` + `curve`（电平依赖静态特性）
   - **freq**：`scan.family[].result`（freq 分析随参数变化）
   - **time**：`gr_timeline.gr.timeline` + `tau`（动态包络）
3. **数据一致性保证**：dataset 的每个 body 与独立导出（scanToJSON / compressionFamilyToJSON / grTimelineToJSON）**逐数据等价**（测试 `[export][dataset-body-equiv]` 锁定）——AI 可放心把 dataset 当作独立导出的并集使用。
4. **拟合建议**：`note` 携带测量工具自动检测的线索（如峰值位置），引导建模优先级。

---

## 9. exportWav 命令（WAV 导出）

**二进制导出**（非 JSON），属于 IPC 协议命令而非导出文档——此处记录命令契约与文件布局。
实现：`source/analysis/WavExporter.*`；来源：最后一次测量留在会话结果（`MeasurementResults`
底层 `CaptureBuffer`）中的 dry/wet 录音。

### 命令契约

| 请求 | 成功响应 | 失败响应 |
| ---- | -------- | -------- |
| `{"cmd":"exportWav","path":<导出 .json 路径>}` | `{"ok":true,"wav_path":<实际 .wav 路径>}` | `{"ok":false,"error":...}` |

- `path`：导出目标路径（习惯给 `.json`，内部按 `.json→.wav` 换扩展名；无 `.json` 后缀则追加 `.wav`）——与 measure/scan 崩溃保护镜像的 `wavPathFor` 同一规则
- 失败错误：`no session`（未接线会话）/ `no measurement result`（会话结果无录音样本）/ `path required` / `wav export failed`（文件创建或写入失败）
- 不触发新测量——纯离线导出内存中的上次测量结果

### WAV 文件布局

- **格式**：单文件 24-bit PCM（44 字节 RIFF 头，byteRate/blockAlign 标准，与
  CaptureBuffer 增量镜像一致的手写 writer）
- **声道**：`3 × 插件声道`，交织布局 `[dry ch0..N-1, wet ch0..N-1, dry ch0..N-1]`
  （立体声插件 → 6 声道）；**bypass = dry 副本（v1）**
- **采样**：sample → int32 = `jlimit(-1.0, 1.0, sample) * 8388607`，小端 3 字节
  （low/mid/high）——与 CaptureBuffer 量化逐位一致
- **长度**：`dry.getNumSamples()`（= 上次测量的录音样本数）

**建模用途**：dry/wet 双路参照供反推插件处理方式（绕过 = dry 副本，v1 语义）；
与导出 JSON 的 `context` 配合即可精确复现测量条件。

---

## 10. 参数自动化时间线（parameter_timeline / parameter_timeline_play）

块 B 任务 2：`recordTimeline` / `stopTimeline` / `playTimeline` 三命令的录制/回放契约。
录制**只记参数事件，不录音频**（D2）；音频采集发生在 `playTimeline`。实现：
`source/capture/ParameterTimeline.*`（录制 + 回放）、`MeasurementSession::setTimelinePlayback`
（每 block 应用事件 + R2 参数恢复）、`CommandParser`（三命令接线）。

### 10.1 命令契约

| 命令 | 请求 | 成功响应 | 失败响应 |
| ---- | ---- | -------- | -------- |
| `recordTimeline` | `{}`（无参） | `{"ok":true,"recording":true}` | `no plugin loaded` / `already recording` |
| `stopTimeline` | `{"path":<timeline .json 路径>}` | `{"ok":true,"timeline_path":<路径>,"events":N}` | `not recording` / `path required` / `timeline export failed` |
| `playTimeline` | `{"path":<timeline .json 路径>,"rate":<倍速, 可选, 缺省 1.0>}` | 回放期推送进度行（见下），完成后 `{"ok":true,"samples":N,"rate":R,"export_path":<play json 路径>,"wav_path":<wav 路径>}` | `no session or plugin` / `path required` / `file not found` / `invalid rate` / `invalid timeline json` / `measurement failed` / `wav export failed` |

要点：

- **`recordTimeline` 非阻塞**（C4 例外）：立即返回，监听器保持挂载，期间每次
  `setValueNotifyingHost`（任何线程，C8）都被记录；`stopTimeline` 才卸载监听器并落盘。
- **事件即变更**：只在参数值实际变化时触发（`setValueNotifyingHost` 的语义）。
- **R9**：无稳定 id 的非托管参数（`param_id` 为空）不记录。
- **`playTimeline` 阻塞**（与 measure 同构：WaitableEvent + callAsync 回消息线程）：
  在测量 run 中逐 block 应用自动化（elapsed wall-clock ms 对比事件 `time_ms`，`rate`
  预缩放为 `effectiveMs = time_ms / rate`），结束后**恢复被触及参数的播放前值（R2）**。
- **回放进度行（issue #2）**：每应用一个事件，服务器在最终响应前推送一行
  `{"ok":true,"progress":<fraction>,"event_index":N,"event_total":M,"time_ms":T}`
  （`time_ms` 为 run 内 elapsed ms；进度行是中间行，最终响应带 `samples`/`export_path`/
  `wav_path`，客户端据此区分）。GUI 面板同步显示「事件 N/M」（~50 ms 节流）。
  依赖 PipeServer 并发模型（长命令在 worker 上跑，读循环继续服务控制命令——
  见 `source/ipc/AGENTS.md`）。
- **事件按墙钟 elapsed 应用**：`applyEventsUpTo` 用 run 内 wall-clock ms 对比事件
  `time_ms`。run 处理快于实时（纯计算，无音频设备时 5 s sweep 约 1 s 墙钟完成），
  **墙钟超过 run 时长的晚事件不会应用**——录制时间戳含客户端进程延迟时尤其明显；
  用 `rate` 预缩放（`effectiveMs = time_ms / rate`）把事件压进 run 墙钟窗口。
- **回放产物绝不覆盖输入 timeline 文件**：`"tl.json"` → play JSON `"tl_play.json"`
  （sibling 手工拼接，`withFileExtension` 会产出 `tl._play.json`），WAV = `wavPathFor`（
  `.json→.wav` → `"tl_play.wav"`），布局同 §9（3 × 插件声道 `[dry, wet, bypass=dry]`）。

### 10.2 parameter_timeline（`stopTimeline` 导出）

```json
{
  "type": "parameter_timeline",
  "events": [
    { "time_ms": 0,   "param_id": "drive", "value": 0.7 },
    { "time_ms": 30,  "param_id": "mix",   "value": 0.2 }
  ]
}
```

| 字段 | 类型 | 说明 |
| ---- | ---- | ---- |
| `type` | string | 固定 `"parameter_timeline"` |
| `events[]` | array | 按 `time_ms` 升序（同毫秒内保参数到达顺序） |
| `events[].time_ms` | int | 相对录制起点（`recordTimeline`）的墙钟毫秒 |
| `events[].param_id` | string | 参数稳定 id（与 `getParams` 响应的 `param_id` 一致） |
| `events[].value` | number | 归一化值 0..1 |

**建模用途**：自动化轨迹——AI 回放同一参数操作序列复现插件在动态参数下的行为。

### 10.3 parameter_timeline_play（`playTimeline` 导出）

```json
{
  "type": "parameter_timeline_play",
  "timeline_path": "C:\\tmp\\tl.json",
  "rate": 1.0,
  "samples": 240000,
  "sample_rate": 48000.0
}
```

| 字段 | 类型 | 说明 |
| ---- | ---- | ---- |
| `type` | string | 固定 `"parameter_timeline_play"` |
| `timeline_path` | string | 本次回放的输入 timeline 文件路径 |
| `rate` | number | 本次回放倍速 |
| `samples` | int | 回放 run 的录音样本数 |
| `sample_rate` | number | 录音采样率 |

**回放音频**：`wav_path` 指向的 WAV（布局同 §9，dry/wet/bypass 三路）供 AI 对比
"自动化驱动下的插件输出 vs 输入"，从而反推参数→处理的动态关系。

---

## 11. 处理顺序反推（processing order 探测）

> 2026-08-14 定稿（设计契约；**未实现**，待开 issue）。目的：黑盒判定插件处理链的
> **模块顺序**（EQ 在压缩器前还是后）——规格可开发性的关键缺口（DESIGN.md 开发目标）。
> 产出并入 chain_doc `processing_order`（从"永远 unknown + canonical 建议"升级为
> "实测 + 证据列表"）。不新增 IPC measure 类型：复用 ScanEngine（扫参数）+
> CompressionFamily/GR 时间线（观 GR）+ 既有 latency 补偿。

### 11.1 判定原理（可观测信号）

顺序的可观测信号 = **扫 EQ 增益，GR/压缩曲线动不动**：

```
EQ → Dyn（压缩器前有 EQ）：压缩器检测信号已过 EQ → 改 EQ 增益，GR 跟着变
Dyn → EQ（EQ 在压缩器后）：压缩器检测原始信号 → 改 EQ 增益，GR 不变
```

### 11.2 探测方案（三个，可组合交叉验证）

| 方案 | 做法 | 判定 |
| ---- | ---- | ---- |
| **A. gain sweep × GR（主）** | 压缩器设明显工作区（低 threshold/高 ratio，GR 显著非零）→ EQ band 提升（+6dB @ 某频点）→ ScanEngine 扫 EQ gain 0→+12dB 每轮测 GR | GR 随 gain 单调增 → EQ→Dyn；GR 不变 → Dyn→EQ |
| **B. 频率选择性** | band 内频点 vs band 外频点等幅注入，比较 GR | GR 差异曲线与 EQ 形状吻合 → EQ→Dyn；与任何 EQ band 无关 → 压缩器自身频率依赖侧链（非顺序证据） |
| **C. 压缩曲线指纹（静态）** | 扫 EQ gain，拟合 compression_curve `fitted` 参数族 | threshold 漂移 → EQ→Dyn；threshold 不动仅输出整体增益变 → Dyn→EQ |

**交叉验证要求**：三方案一致才给高置信；方案 A/B/C 冲突 → 降置信或 unknown。

### 11.3 输出契约（并入 chain_doc.processing_order）

```json
{
  "order": "eq->dyn",
  "confidence": "high",
  "basis": [
    { "probe": "gain_sweep", "param": "Band 1 Gain", "gr_delta_db": 6.2 },
    { "probe": "freq_selective", "in_band_gr_db": 8.1, "out_band_gr_db": 0.4 },
    { "probe": "curve_fit", "threshold_shift_db": 5.8 }
  ]
}
```

| 字段 | 类型 | 说明 |
| ---- | ---- | ---- |
| `order` | string | `"eq->dyn"` / `"dyn->eq"` / `"unknown"`（可扩展 `"eq->dyn->eq"` 等多段） |
| `confidence` | string | `"high"` / `"medium"` / `"low"`——多信号一致才 high；判定不了给 `"low"` + `order:"unknown"` |
| `basis[]` | array | 每项一个探测证据：`{probe, ...观测值}`——probe 取 `gain_sweep` / `freq_selective` / `curve_fit`，观测值字段见上表，供开发者复核 |

### 11.4 已知陷阱与排除

| 陷阱 | 排除方法 |
| ---- | -------- |
| 频率依赖侧链（现代压缩器常见） | 方案 B：GR 曲线形状与 EQ 曲线吻合才视为顺序证据 |
| 并行压缩 / dry-wet mix | 混合比稀释 GR——多方案交叉 + 多信号验证 |
| 多段压缩 | band 恰匹配某段 → 混淆；多 band 探测，单 band 结论仅 medium |
| lookahead / 平滑 | 稳态测量（每轮测量达稳态后取 GR）+ 既有 latency 补偿 |
| detector 类型（peak/RMS） | 单音 + ToneBurst + 宽带多信号交叉，结论取交集 |

### 11.5 验证方案（先验已知顺序）

用 `TestCompressorPlugin`（可配置 EQ→压缩器顺序）作为 ground truth 验证探测有效性：
扫描其 EQ band gain → GR 响应必须与真实顺序一致，三方案判定全对才视为探测可靠；
再以真机插件（如 FabFilter Pro-Q 4 + Pro-C 3 组合）实测校准 confidence 阈值。

---

## 回归保障

- 全部导出测试：`tests/ExportTests.cpp`（schema、精度、转义、python 可解析性、body 等价、dataset 聚合）。
- 现有独立导出的输出格式由既有 schema 测试逐字节/逐数据锁定；dataset 聚合**只复用 body 序列化函数，不触碰独立导出函数**（策略 b）。
- 验证：`ctest --test-dir build -C Release --timeout 180` 全绿（158 项，2026-08-08）。
