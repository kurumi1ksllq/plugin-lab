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

| 项目       | 方案                                                       |
| ---------- | ---------------------------------------------------------- |
| 信号       | 对数正弦扫描 20Hz ~ 20kHz                                  |
| 录制       | 干路（原始） + 湿路（过插件）同时录制                      |
| 分析       | 逐频点 FFT → 幅度比 + 相位差                               |
| 绘图       | 曲线从左向右增量生长                                       |
| **平滑**   | 提供多级平滑：原始 / 1/12 octave / 1/3 octave              |
| **策略**   | **纯黑盒** — 不关心内部有几个频点/Q值/增益，一次扫完全频段 |
| **非线性** | 可选：用不同信号电平（-20dB, -10dB, 0dB）扫多组，叠加显示  |

**为什么黑盒方案足够：**

- 不需要知道插件内部有几个频点、每个频点对应哪个旋钮
- 一次全频段扫描 + 平滑处理，自然包含所有频点信息和耦合关系
- 步进式旋钮不影响测量精度（步进是参数设置层面的问题，不是信号分析层面的问题）
- 采集数据后 Sisyphus 做后处理：峰值检测 → 反推频点/频率/Q值/增益

### 3.2 谐波分析（饱和/失真类插件）

| 项目 | 方案                                             |
| ---- | ------------------------------------------------ |
| 信号 | 单音正弦（如 1kHz），多电平（-20dB, -10dB, 0dB） |
| 录制 | 湿路                                             |
| 分析 | FFT → 基波 + 各次谐波能量                        |
| 绘图 | 柱状图：基波 + H2 + H3 + H4 + H5                 |
| 输出 | THD%、各次谐波百分比                             |

### 3.3 压缩曲线（动态类插件）

| 项目 | 方案                                        |
| ---- | ------------------------------------------- |
| 信号 | ToneBurst，从 -60dB 到 0dB 步进             |
| 录制 | 干路 + 湿路                                 |
| 分析 | 输入 dB vs 输出 dB → 算出压缩比、拐点、GR   |
| 绘图 | XY 折线图，逐点生长                         |
| 输出 | `{input_dB, output_dB, gr_dB}[]` + 拟合参数 |

---

## 四、数据导出格式（JSON）

```json
{
  "session": {
    "timestamp": "2026-07-31T12:00:00Z",
    "plugin": {
      "name": "MyFavoriteComp",
      "path": "C:/VST/MyFavoriteComp.vst3",
      "uid": "vendor123_comp_v1"
    },
    "snapshot": {
      "description": "用户调到的喜欢的声音",
      "parameters": [
        { "name": "Ratio", "index": 3, "normalized": 0.5, "value": 4.0 },
        { "name": "Threshold", "index": 4, "normalized": 0.3, "value": -20.0 }
      ]
    },
    "measurements": [
      {
        "type": "freq_response",
        "config": {
          "freq_range": [20, 20000],
          "sweep_duration_s": 5,
          "input_level_dB": -12,
          "output": "results/freq_sweep.json",
          "smoothing": ["raw", "1_12_octave", "1_3_octave"]
        },
        "parameters": {
          "sample_rate": 48000,
          "fft_size": 2048
        }
      }
    ]
  }
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

| 模块            | 变更                                                                |
| --------------- | ------------------------------------------------------------------- |
| **IPC**（新增） | `ipc/PipeServer.h/cpp`, `ipc/CommandParser.h/cpp`, `ipc/Protocol.h` |
| **host/**       | 无实质变化，但控制路径从 UI 点击改为 IPC 命令驱动                   |
| **signal/**     | 无实质变化                                                          |
| **capture/**    | 新增"实时进度回调"，测量结果逐步推送到 UI                           |
| **analysis/**   | 新增**平滑处理**功能（raw / 1/12 / 1/3 octave）                     |
| **ui/**         | 重大变化：需嵌入 VST3 原生编辑器 UI；曲线改为增量绘制               |
| **控制路径**    | 从 GUI 点击 → IPC 命令驱动，GUI 作为"显示器"                        |
