# Plugin Lab — Handoff Context

> 会话日期: 2026-07-31
> 项目: Plugin Lab (独立 VST 插件测量工具)

---

## 用户原始需求 (AS-IS)

- "我们讨论一个事情,对于eq,压缩,谐波染色类音频插件,能不能做出一个工具,可以加载vst插件,你来一直控制这个工具,调整vst插件里的旋钮,然后采集这个插件的处理结果,这样就可以对插件的原理和处理进行记录和建模,是否就可以在此基础上进行复刻和改进了"
- "nonono,单独开发一个工具,进行这些测量工作"
- "不是为了复刻抄袭别人的插件,而是有一些插件的声音我非常喜欢,我要自己开发插件,主要是 eq,压缩,染色插件,我的插件希望可以获得我想要的效果"
- "我需要,我可以看到你每一步调试都在操作插件的哪个旋钮和功能,可以把最后得出的数据进行可视化显示,我没有相关的开发经验和知识支撑,我不一定看得懂,但是比如eq的频点,q值等,压缩插件的压缩比 拐点等这些基础参数最好是把数据可视化显示,这样方便我看懂,但是最终详细的数据,需要给到你,你获取到最详细的参数和数据后,你来按我的要求进行开发插件"

---

## 目标

定义、设计并实现 "Plugin Lab" — 一个独立的 JUCE C++ GUI 应用程序，能够：
1. 加载 VST3 插件
2. 通过系统化的参数扫描捕获插件的处理行为（频率响应、谐波失真、压缩曲线）
3. 为用户可视化结果
4. 导出结构化数据供 AI（Sisyphus）用于插件开发

---

## 已完成的讨论

- 分析了 AutoPitchK 项目结构，了解用户现有的 JUCE 环境
- 审查了现有工具（offline_render.cpp, audio_qa.cpp），其中包含可复用的信号生成和测量代码
- 讨论并完善了独立 VST 分析工具的概念
- 澄清用户意图：不是逆向工程/克隆，而是使用参考插件理解期望的声音特性，然后实现具有匹配特性的原创插件
- 建立了人机协作工作流：用户用耳朵调参 → 工具测量/可视化 → AI 消费导出数据实现匹配的 DSP
- 工具命名为 "Plugin Lab"
- 技术栈确定：纯 JUCE C++ 独立应用（无 Python/Node 依赖）
- 设计了高层模块架构：host/, signal/, capture/, analysis/, ui/
- 确定了三种核心测量模式：频率响应（EQ）、谐波分析（饱和）、压缩曲线（动态）
- 定义了测量工作流：加载插件 → 调到喜欢的声音 → 锁定参数快照 → 运行扫描 → 查看曲线 → 导出结构化 JSON 数据

---

## 当前状态

- AutoPitchK 工作区在 D:\Documents\AutoPitchK，git 仓库有一个初始提交
- DSP 源文件和工具已存在，但 Plugin Lab 尚未开始
- Plugin Lab 尚未编写任何代码；仅作为设计概念存在
- 用户的 AGENTS.md 规定：核心逻辑用 TDD，UI/UX 用原型优先，架构模块化

---

## 待完成的任务

1. 创建 Plugin Lab 项目目录，包含 CMakeLists.txt 和 JUCE 集成
2. 实现 PluginManager（VST3 扫描器/加载器）使用 JUCE AudioPluginFormatManager
3. 实现 SignalGenerator（SineSweep, MultiTone, ToneBurst, Impulse）
4. 实现 CaptureEngine（干/湿路录音，缓冲区管理）
5. 实现 AnalysisEngine（FreqResponse, HarmonicAnalysis, CompressionCurve）
6. 构建 UI：PluginPanel, MeasurementPanel, PlotWidget, 可视化组件
7. 实现数据导出为结构化 JSON 格式
8. 端到端测量工作流集成

---

## 关键参考文件

| 文件 | 说明 |
|------|------|
| `AutoPitchK/CMakeLists.txt` | JUCE FetchContent 模式，可直接复用 |
| `AutoPitchK/tools/offline_render.cpp` | 离线渲染器模式；VST 宿主 + 音频处理模型可参考 |
| `AutoPitchK/tools/audio_qa.cpp` | 质量分析器，包含信号生成（sine sweep）、FFT 分析、THD 测量；代码可直接复用 |
| `AutoPitchK/source/PluginProcessor.h` | AutoPitchK 处理器模式，展示 JUCE AudioProcessor 集成 |

---

## 架构设计

```
PluginLab/
├── CMakeLists.txt
├── source/
│   ├── Main.cpp                   // 应用入口
│   ├── AppConfig.h                // 全局配置
│   │
│   ├── host/                      // VST 宿主核心
│   │   ├── PluginManager.h/cpp    // 扫描、加载、卸载 VST3
│   │   ├── PluginParameter.h/cpp  // 参数枚举、归一化、分组
│   │   └── AudioRouter.h/cpp      // 音频 I/O 路由，干湿路录音
│   │
│   ├── signal/                    // 信号生成
│   │   ├── SignalGenerator.h/cpp
│   │   ├── SineSweep.h/cpp        // 对数正弦扫描
│   │   ├── MultiTone.h/cpp        // 多音测试
│   │   ├── ToneBurst.h/cpp        // 测量压缩器动态
│   │   └── Impulse.h/cpp          // MLS / 脉冲
│   │
│   ├── capture/                   // 数据采集
│   │   ├── MeasurementSession.h/cpp
│   │   ├── SweepRunner.h/cpp      // 参数扫描调度
│   │   └── AudioBuffer.h/cpp      // 干/湿路录制缓冲区
│   │
│   ├── analysis/                  // 数据分析
│   │   ├── FreqResponse.h/cpp     // 频率响应 (magnitude + phase)
│   │   ├── HarmonicAnalysis.h/cpp // 谐波结构 (THD, 各次谐波)
│   │   ├── CompressionCurve.h/cpp // 压缩曲线 (gain reduction)
│   │   └── Export.h/cpp           // 数据导出 → JSON
│   │
│   ├── ui/                        // 用户界面
│   │   ├── MainWindow.h/cpp
│   │   ├── PluginPanel.h/cpp      // 插件加载/参数面板
│   │   ├── MeasurementPanel.h/cpp // 测量控制面板
│   │   ├── PlotWidget.h/cpp       // 通用曲线绘图组件
│   │   ├── FreqResponsePlot.h/cpp
│   │   ├── HarmonicPlot.h/cpp
│   │   └── CompCurvePlot.h/cpp
│   │
│   └── utils/
│       ├── FftHelper.h/cpp
│       └── MathUtils.h/cpp
```

---

## 关键技术决策

| 决策 | 选择 | 理由 |
|------|------|------|
| 技术栈 | 纯 JUCE C++ | 单 .exe 分发，零运行时依赖，原生 VST3 支持 |
| VST 宿主 | AudioPluginFormatManager | JUCE 内置，支持 VST3 和其他格式 |
| 可视化 | JUCE Graphics + Path | MVP 够用，后期可扩展 websocket 接前端 |
| 数据导出 | JSON | 结构清晰，AI 和人类都可读 |
| 项目独立性 | 单独目录/仓库 | 用户明确要求与 AutoPitchK 分离 |

---

## 重要约束

- "单独开发一个工具" — Plugin Lab 必须是独立项目，不在 AutoPitchK 代码库内
- 用户缺乏 DSP/开发经验 — 可视化必须直观可读
- 不是克隆现有插件 — 而是开发具有参考信息行为的原创插件
- 开发方式：核心逻辑用 TDD，UI/UX 用原型优先，严格模块化架构

---

## 人机协作工作流

```
用户（人耳审美）     Plugin Lab（测量分析）     Sisyphus（AI 实现）
      │                      │                         │
      ├─ 加载参考插件 ──────▶│                         │
      ├─ 调旋钮找喜欢的声 ──▶│                         │
      │                      ├─ 可视化曲线 ← 用户看    │
      │                      ├─ 导出详细数据 ─────────▶│
      │                      │                         ├─ 按数据实现 DSP
      │                      │                         ├─ 生成新插件
      ├─ 试听 ←─────────────┴─────────────────────────┤
      └─ 给反馈 → 迭代修改 ────────────────────────────┘
```
