# Plugin Lab — 设计文档

> 基于 2026-07-31 讨论结果整理
> 补充/替代 handoff 中的过时决策

---

## 一、核心设计原则

1. **GUI 主模式** — 工具必须带完整的图形界面，不是后台脚本
2. **实时交互** — Sisyphus 发指令 → 工具立即响应 → 窗口同步更新（参数变化动画、曲线增量绘制）
3. **黑盒测量** — 不依赖对插件内部的先验知识，只管输入输出
4. **数据留存** — 原始数据 + 平滑处理数据都保留，不同消费场景用不同精度

---

## 二、控制方式：Windows Named Pipe IPC

### 为什么不是 HTTP

- JUCE C++ 应用内嵌 HTTP 服务器过于笨重
- 命名管道是 Windows 原生 IPC，零额外依赖，双向通信

### 通信模型

```
你(聊天) → Sisyphus → PowerShell → \\.\pipe\PluginLab → Plugin Lab GUI
                                                           ↓
                                                    实时响应 + 窗口更新
```

### 管道协议（JSON 行协议）

每个请求/响应各占一行 JSON：

```
→ {"cmd":"loadPlugin","path":"C:/VST/MyComp.vst3"}
← {"ok":true,"params":[...]}                          // 返回参数列表

→ {"cmd":"setParam","name":"Ratio","value":4.0}
← {"ok":true,"value":4.0}                             // 旋钮动画同步

→ {"cmd":"getParams"}
← {"ok":true,"params":[{"index":0,"name":"...","value":0.5,"min":0,"max":1},...]}

→ {"cmd":"measure","type":"freq|harmonic|compression"}
← {"ok":true,"progress":0.10}                         // 持续推送进度
← {"ok":true,"progress":0.50}
← {"ok":true,"progress":1.0,"data":{...}}             // 完成

→ {"cmd":"export","path":"results.json"}
← {"ok":true,"path":"results.json"}

→ {"cmd":"stop"}
← {"ok":true}
```

### 新增 IPC 模块

```
source/ipc/
├── PipeServer.h/cpp      // 命名管道服务器（CreateNamedPipe + 消息循环）
├── CommandParser.h/cpp   // JSON → 内部命令路由
└── Protocol.h            // 消息类型定义
```

---

## 三、测量策略

### 3.1 频率响应（EQ 类插件）—— 核心变化

| 项目       | 方案                                                                                                                     |
| ---------- | ------------------------------------------------------------------------------------------------------------------------ |
| 信号       | 对数正弦扫描 20Hz ~ 20kHz                                                                                                |
| 录制       | 干路（原始） + 湿路（过插件）同时录制                                                                                    |
| 分析       | **Farina 去卷积法**：扫频数据 → 脉冲响应 → FFT → 幅度比 + 相位差（比直接 FFT 比值在低频/高频相位噪声更小，行业标准做法） |
| 绘图       | 曲线从左向右增量生长                                                                                                     |
| **平滑**   | 提供多级平滑：原始 / 1/12 octave / 1/3 octave                                                                            |
| **策略**   | **纯黑盒** — 不关心内部有几个频点/Q值/增益，一次扫完全频段                                                               |
| **非线性** | 可选：用不同信号电平（-20dB, -10dB, 0dB）扫多组，叠加显示                                                                |
| **备选**   | `Impulse`（MLS 序列）可直接测脉冲响应 → FFT，EQ 线性测量可更快完成                                                       |

**为什么黑盒方案足够：**

- 不需要知道插件内部有几个频点、每个频点对应哪个旋钮
- 一次全频段扫描 + 平滑处理，自然包含所有频点信息和耦合关系
- 步进式旋钮不影响测量精度（步进是参数设置层面的问题，不是信号分析层面的问题）
- 采集数据后 Sisyphus 做后处理：峰值检测 → 反推频点/频率/Q值/增益

### 3.2 谐波分析（饱和/失真类插件）

| 项目     | 方案                                                                                                                     |
| -------- | ------------------------------------------------------------------------------------------------------------------------ |
| 信号     | **单音正弦（如 1kHz），多电平（-20dB, -10dB, 0dB）测 THD**；**多音测 IMD（互调失真）**，两者分开                         |
| 录制     | 湿路                                                                                                                     |
| 分析     | FFT → 基波 + 各次谐波能量                                                                                                |
| 绘图     | 柱状图：基波 + H2 + H3 + H4 + H5                                                                                         |
| 输出     | THD%、各次谐波百分比                                                                                                     |
| **注意** | MultiTone 各频点同时发声时谐波峰会互相交叠（如 2kHz 的 H2=4kHz 恰好落在 4kHz 基频上），故 THD 用单音、IMD 用多音，勿混用 |

### 3.3 压缩曲线（动态类插件）

| 项目     | 方案                                                                                                   |
| -------- | ------------------------------------------------------------------------------------------------------ |
| 信号     | ToneBurst，从 -60dB 到 0dB 步进                                                                        |
| **频率** | **至少 3 个频点：80Hz / 1kHz / 4kHz 各跑一轮**（现代人声压缩器多有频率依赖侧链，单测 1kHz 对低频无效） |
| 录制     | 干路 + 湿路                                                                                            |
| 分析     | 输入 dB vs 输出 dB → 算出压缩比、拐点、GR                                                              |
| 绘图     | XY 折线图，逐点生长                                                                                    |
| 输出     | `{input_dB, output_dB, gr_dB}[]` + 拟合参数                                                            |

---

## 四、数据导出格式（JSON）

> 2026-08-02 Oracle 审查补充：必须包含 **插件元数据 / 测量配置 / Bypass 双路对比**，否则 AI 无法可靠复刻。

```json
{
  "session": {
    "timestamp": "2026-07-31T12:00:00Z",
    "plugin": {
      "name": "MyFavoriteComp",
      "path": "C:/VST/MyFavoriteComp.vst3",
      "uid": "vendor123_comp_v1",
      "class_id": "FUID...",
      "manufacturer": "Vendor",
      "version": "1.0.0",
      "latency_samples": 256
    },
    "measurement_config": {
      "source_type": "test_signal | file_playback",
      "generator": {
        "type": "sine_sweep",
        "freq_range": [20, 20000],
        "duration_s": 5,
        "level_dB": -12
      },
      "sample_rate": 48000,
      "block_size": 512,
      "analysis": {
        "fft_size": 16384,
        "smoothing": ["raw", "1_12_octave", "1_3_octave"]
      }
    },
    "snapshot": {
      "description": "用户调到的喜欢的声音",
      "parameters": [
        { "name": "Ratio", "index": 3, "normalized": 0.5, "value": 4.0 }
      ]
    },
    "bypass_reference": {
      "dry": "bypass_dry.wav",
      "wet": "bypass_wet.wav",
      "note": "插件 bypass 状态跑一遍作为基准，隔离插件实际改动 vs 延迟/精度损耗"
    },
    "measurements": [
      {
        "type": "freq_response",
        "config": {
          "freq_range": [20, 20000],
          "sweep_duration_s": 5,
          "input_level_dB": -12
        },
        "parameters": { "sample_rate": 48000, "fft_size": 2048 }
      }
    ]
  }
}
```

### 记录模式（vocal 回放）导出

```json
{
  "session": {
    "timestamp": "...",
    "plugin": {
      "name": "...",
      "path": "...",
      "class_id": "...",
      "latency_samples": 256
    },
    "input_source": {
      "type": "file",
      "path": "vocal_dry.wav",
      "duration_s": 12.4,
      "channels": 2
    },
    "measurement_config": {
      "source_type": "file_playback",
      "sample_rate": 48000,
      "block_size": 512
    }
  },
  "audio": {
    "dry": "session_dry.wav",
    "wet": "session_wet.wav",
    "bypass_dry": "bypass_dry.wav",
    "bypass_wet": "bypass_wet.wav",
    "sample_rate": 48000
  },
  "parameter_timeline": [
    {
      "t_ms": 1200,
      "name": "Threshold",
      "index": 4,
      "old_normalized": 0.3,
      "new_normalized": 0.2,
      "old_value": -10.0,
      "new_value": -18.5
    }
  ],
  "block_statistics": [
    {
      "t_ms": 0,
      "rms_in_db": -14.2,
      "rms_out_db": -11.8,
      "gr_db": 2.4,
      "peak_in_db": -8.1,
      "peak_out_db": -6.3
    }
  ],
  "analysis_snapshots": [{ "t_ms": 1000, "type": "spectrum", "data": "..." }]
}
```

---

## 五、窗口布局

```
┌──────────────────────────────────────────────────────────┐
│  Plugin Lab                                              │
├─────────────────────────────┬────────────────────────────┤
│                             │  ■ 测量控制                 │
│  插件 UI 区域               │  [频率响应] [谐波] [压缩]   │
│  (VST3 原生编辑器嵌入)       │                             │
│                             │  ■ 参数日志                  │
│  旋钮实时反应参数变化         │  Ratio → 4.0  ✓            │
│                             │  Threshold → -20dB  ✓       │
│                             ├────────────────────────────┤
│                             │  ■ 实时曲线                  │
│                             │  ┌──────────────────────┐   │
│                             │  │  📈 边扫边画           │   │
│                             │  │  曲线从左向右生长       │   │
│                             │  └──────────────────────┘   │
├─────────────────────────────┴────────────────────────────┤
│  IPC 连接: 已连接 | 状态: 测量中 | 当前操作: 扫频 1.2kHz  │
└──────────────────────────────────────────────────────────┘
```

---

## 六、完整操作链路

```
1. 你: "加载这个压缩器看看参数"
   → 我发 loadPlugin → 插件 UI 出现在窗口中
   → 我发 getParams → 看到所有参数，告诉你

2. 你: "ratio 拧到 4，threshold -20"
   → 我发 setParam → 窗口里旋钮转动到 4
   → 我发 setParam → threshold 变化

3. 你: "跑个压缩曲线"
   → 我发 measure {type:"compression"}
   → 图上一个点一个点地画出来，你看着曲线成形

4. 你: "再试试 ratio 8"
   → 我发 setParam → 旋钮动
   → 我发 measure → 新曲线覆盖，实时对比

5. 你: "数据导出"
   → 我发 export → JSON 到手
```

---

## 七、架构变更总结（相对 handoff）

| 模块            | 变更                                                                                                                                                    |
| --------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------- |
| **IPC**（新增） | `ipc/PipeServer.h/cpp`, `ipc/CommandParser.h/cpp`, `ipc/Protocol.h`                                                                                     |
| **host/**       | 无实质变化，但控制路径从 UI 点击改为 IPC 命令驱动                                                                                                       |
| **signal/**     | 无实质变化                                                                                                                                              |
| **capture/**    | 新增"实时进度回调"，测量结果逐步推送到 UI                                                                                                               |
| **analysis/**   | 新增**平滑处理**功能（raw / 1/12 / 1/3 octave）                                                                                                         |
| **ui/**         | 重大变化：需嵌入 VST3 原生编辑器 UI；曲线改为增量绘制                                                                                                   |
| **capture/**    | 新增 `RecorderEngine`（顶层协调）、`ParameterTimeline`（参数时间线）、`AnalysisStrategy`（分析分流）；`MeasurementSession` 扩展 sourceType/filePlayback |
| **signal/**     | 新增 `FilePlayback`（实现 `SignalGenerator` 接口，vocal 音频回放 + 声道映射）                                                                           |
| **analysis/**   | 新增 `WavExporter`（干/湿/基准多轨 WAV）；`Export` 扩展 `recordingToJSON`；JSON 改用 `juce::JSON::toString()` 转义                                      |
| **控制路径**    | 从 GUI 点击 → IPC 命令驱动，GUI 作为"显示器"                                                                                                            |

---

## 八、数据记录系统（2026-08-02 定稿）

> 目标：记录"插件处理声音时的数据"，支持两类输入（内部测试信号 / 真实 vocal 音频文件回放），
> 详细数据给 AI（DSP 复刻/改进），直观图表给人看。
> 本方案经 Oracle 架构审查验证后定稿（审查结论见 STATUS.md）。

### 8.1 测量方法论（人声处理插件）

| 插件类型                           | 测什么                                                        | 输入                                        | 分析                              |
| ---------------------------------- | ------------------------------------------------------------- | ------------------------------------------- | --------------------------------- |
| EQ（线性）                         | 频响曲线（幅度+相位）                                         | 扫频测试信号                                | Farina 去卷积 → FFT               |
| 压缩（动态）                       | 静态曲线（比/拐点/GR）+ 动态行为（attack/release、GR 随时间） | ToneBurst 多电平（80/1k/4k Hz）+ 真实 vocal | CompressionCurve + 逐块 GR 时间线 |
| 谐波染色（非线性）                 | 静态 THD/谐波结构 + 实际染色                                  | 单音多电平（THD）+ 多音（IMD）+ 真实 vocal  | HarmonicAnalysis + 频谱对比       |
| 人声特有（去齿音/音高修正/共振峰） | 行为观察                                                      | **只能真实 vocal 触发**                     | 波形/RMS/频谱时间线对比           |

**核心原则**：静态参数只能靠受控测试信号精确量化；动态行为必须有真实信号触发——两者互补，缺一不可。

### 8.2 架构（模块边界）

```
RecorderEngine (新增, source/capture/RecorderEngine.h/cpp)   ← 顶层协调: 批量扫描/多轮对比/生命周期
  ├─ MeasurementSession (扩展: + sourceType + setFilePlayback + AnalysisStrategy)
  │    └─ SweepRunner (不动: 纯 I/O 管线 generate→process→capture)
  ├─ ParameterTimeline (新增)     ← AudioProcessorParameter::addListener 记录参数变更
  ├─ AnalysisStrategy (新增)      ← capture 之后按 sourceType 分流分析器
  ├─ FilePlayback (新增, source/signal/FilePlayback.h/cpp)  ← 实现 SignalGenerator 接口, 含声道映射
  ├─ WavExporter (新增, source/analysis/WavExporter.h/cpp)   ← JUCE WavAudioFormat 写干/湿/基准 多轨 WAV
  └─ Export (扩展: recordingToJSON)
```

**边界原则**（Oracle 确认）：

- `SweepRunner` 不感知"测量目的"，只负责 I/O 管线 → **不改**
- `MeasurementSession` 感知"测什么"（type + sourceType）→ 扩展
- `RecorderEngine` 感知"为什么测"（批量扫描/多轮对比），管理 session 生命周期 → 新增
- 新组件就这三个（RecorderEngine / ParameterTimeline / AnalysisStrategy）+ 两个输入/导出扩展，**不做过度设计**

### 8.3 FilePlayback 复用 SignalGenerator 接口

```cpp
class FilePlayback : public SignalGenerator {
    // prepare: 打开文件读 header（采样率/长度/声道数）
    // getTotalLength: 返回文件样本数
    // generate: 读一块 + 声道映射（mono→双声道复制；立体声保持；多声道取 L/R）
    // reset: seek 回文件头
};
```

真实 vocal 与测试信号走同一条 SweepRunner 管线（generate→process→capture），零特判；
分析阶段由 `AnalysisStrategy` 按 sourceType 分流。

### 8.4 数据格式

见 §四 两种 JSON 结构（测量模式 / 记录模式）。关键要求（Oracle）：

- **Bypass 双路对比**：插件 bypass 状态跑一遍作为基准 → 隔离"插件实际改动"与"延迟/浮点精度损耗"
- **插件元数据**：class_id (FUID)、厂商、版本、`latency_samples`（插件延迟必须记录才能对齐干湿）
- **测量配置**：generator 类型+参数、采样率、block size、分析参数（保证可复现）
- **参数自动化事件**：同时记录归一化值(0-1)与实际值

### 8.5 可视化（离线处理下的"实时"）

- 现有 PlotWidget 三种图（EQ 曲线 / 压缩曲线 / 谐波柱状图）+ **新增实时 GR 表头**
- 实现：`progressCallback` 从 `void(float)` 扩展为传最新干/湿块 → 算 `GR = 20·log10(RMS_wet/RMS_dry)`
- 通过 `AsyncUpdater` post 到 UI 线程，**每 ~50ms 刷新一次**（勿每块刷，避免堵塞消息线程）
- 性能：48kHz/512 块 → ~94 次回调/秒，每次算 2 个 RMS 微秒级，不影响离线处理

### 8.6 实施路线（三阶段）

| 阶段                   | 内容                                                                                                                                                       | 前置   |
| ---------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------- | ------ |
| **1. 加固 + 接通测量** | 修 4 个已知 bug（原子变量/中间落盘/ToneBurst/JSON 转义）；恢复 IPC → measure 三件套（扫频去卷积 + 单音 THD + 多频压缩）；Bypass 对比 + 元数据/配置纳入导出 | —      |
| **2. vocal 回放记录**  | FilePlayback（声道映射）+ AnalysisStrategy 分流 + ParameterTimeline + WAV 双轨导出；vocal 专用分析器（RMS/GR 时间线、频谱对比）                            | 阶段 1 |
| **3. UI 可视化**       | PlotWidget 接线 + GR 表头逐块回调                                                                                                                          | 阶段 2 |

---

## 九、用户需求补充与阶段规划（2026-08-02，用户就寝前确认）

> 用户明确的新需求（影响测量方法论与工具能力），必须在后续阶段落实。vocal 测试素材：`D:\Documents\PluginLab\take01.wav`（3.1MB）。

### 9.1 测量方法论深化（用户要求）

1. **信号多样性——不只扫频**：除对数扫频外，需支持**全频段信号**（白噪声/粉噪声/多频段叠加）作为输入，用于测插件的全频段响应（扫频与噪声各有适用场景，互为补充）。
2. **压缩测量的动态信号**：压缩器（尤其 **opto / vari-mu 类**，非数字压缩）具有：
   - attack/release 时间（启动/释放随输入变化）
   - 压缩量随**输入信号大小**变化
   - 输入**动态信号的幅度、变化速度**都会影响插件处理响应
     → 静态 ToneBurst 不足以表征 → 需要**动态信号**（包络调制/模拟音乐动态）输入 + 测量 GR 随时间的变化、attack/release 时间常数。
3. **参数连续扫描（非线性）**：插件参数改变后处理效果**非线性** → 固定参数的静态结果难以拟合 → 需要**参数空间连续扫描**（如 ratio/threshold/attack 各取多档组合，自动多轮测量），得到连续曲线族/数据面，供 AI 拟合模型。
4. **音频文件加载功能**：工具必须支持**加载音频文件**作为输入（用户后续使用工具也需要此功能；vocal 素材 take01.wav 即测试输入）。

### 9.2 阶段规划（重排）

| 阶段                    | 内容                                                                                                                                          | 存档         |
| ----------------------- | --------------------------------------------------------------------------------------------------------------------------------------------- | ------------ |
| **1（已完成，已存档）** | 测试设施 + 测量三件套（扫频 EQ / 谐波 / 静态压缩）+ 全部实测修复                                                                              | ✅ push main |
| **2. 输入与信号增强**   | FilePlayback（音频文件加载，vocal 素材入库）；全频段信号（白/粉噪声）；动态信号生成器（包络调制，用于 opto/vari-mu 测量）；IPC/GUI 输入源选择 | push         |
| **3. 参数连续扫描**     | 参数扫描引擎（参数+档位→自动多轮测量→数据族）；连续性 JSON 输出；GUI 多曲线叠加对比                                                           | push         |
| **4. 动态压缩行为测量** | 动态信号 + 逐块 GR 时间线；attack/release 时间常数估计；随输入电平的压缩响应；GR 表头可视化                                                   | push         |
| **5. 建模与数据整合**   | 多参数多条件数据整合为连续模型；输出 JSON 升级（曲线族+参数空间+拟合建议）；用测得数据反推插件参数验证                                        | push         |

**执行原则（用户要求）**：每阶段以最严谨模式工作（TDD + 实测验证）；**每阶段完成后 push GitHub 存档**（已配置 origin: kurumi1ksllq/plugin-lab），便于回滚。

### 9.3 待办清单（按阶段）

- [ ] 阶段 2：FilePlayback（含 take01.wav 素材管理）+ 全频段信号 + 动态信号 + 输入源选择
- [ ] 阶段 3：参数扫描引擎 + 连续性数据输出 + GUI 多曲线对比
- [ ] 阶段 4：动态压缩测量（GR 时间线 / attack-release / opto-vari-mu）
- [ ] 阶段 5：数据整合建模 + 反推验证
- [ ] 各阶段 GitHub push 存档
