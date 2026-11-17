# Plugin Lab

VST3 插件黑盒测量实验室 —— Windows 桌面 GUI 工具（C++20 + JUCE 9 + CMake + Catch2）。

AI 通过 Named Pipe IPC（`\\.\pipe\PluginLab`）驱动 GUI：加载插件 → 测量（扫频 / 谐波 / 压缩 / GR 时间线）→ JSON 导出 → 反推插件参数。全程**纯黑盒**：不依赖插件内部先验知识，只靠注入已知信号 + 捕获输出。

## 开发目标

让 AI 把本工具当"逆向工程仪器"：搞清任意 VST 插件在信号层面到底做了什么处理，把**测量数据 + 反推结论**保存为可读、可带走的开发规格，供开发者据此开发声音效果几乎一致的插件（指声音处理效果，不指 UI / 操作方式；**本工具不做自动复刻插件**）。

```
注入已知信号 → 捕获输出 → 分析
  ├─ 频响曲线   → EQ 结构（频点 / Q / 增益）
  ├─ 压缩曲线   → 动态处理（threshold / ratio / knee）
  ├─ GR 时间线  → 时域行为（attack / release）
  └─ 谐波 / IMD → 非线性处理（饱和 / 失真 / 谐波激励）
  → 结论："这个插件是这么处理的"（测量数据 + 反推规格一并交付）
```

## 主要能力

- **插件扫描与加载**：VST3 目录扫描 + 缓存（`%APPDATA%/PluginLab/pluginlist.xml`）；/EHa 崩溃保护 + 黑名单；会杀宿主的插件走**子进程托管**（`PluginHostChild`），宿主永不死亡
- **测量**（4 类型 × 4 激励源）：类型 `frequency_response` / `harmonic` / `compression` / `gr_timeline`；激励源 `signal` / `file` / `noise` / `dynamic`
- **参数扫描**：`ScanEngine` 多轮扫描返回曲线族，参数快照 / 恢复 / 取消
- **导出**：8 类 JSON schema（含完整测量上下文，可复现）+ 6 声道 24-bit WAV 多轨
- **参数自动化**：录制 / 回放参数时间线
- **反推工具链**（stdlib-only Python）：`tools/aggregate_report.py` 聚合 → `tools/describe_chain.py` 生成处理链路描述（chain_doc）→ 交付开发者

## 构建（Windows / MSVC）

> 需 Windows + MSVC。编译开启 `/W4 /permissive- /WX /utf-8`（警告即错误）；`build/` 是唯一构建树。

```
cmake -S . -B build && cmake --build build --config Release
# 运行
build\PluginLab_artefacts\Release\Plugin Lab.exe
```

## 测试

```
cmake -S . -B build -DBUILD_TESTS=ON && cmake --build build --config Release
ctest --test-dir build -C Release --timeout 180
```

Catch2 单元测试 318 项（计数以 `tests/AGENTS.md` 为准，连跑 2 次验稳定）。CI：`.github/workflows/build.yml`，`build-and-test` 为 PR 必过门禁。

## AI / IPC 驱动

Named Pipe `\\.\pipe\PluginLab`，JSON 行协议。命令：`loadPlugin` / `setParam` / `getParams` / `measure` / `scan` / `stop` / `exportData` / `exportWav` / `getScanStatus` / `dataset` / `recordTimeline` / `stopTimeline` / `playTimeline`（协议契约见 `SPEC.md` 与 `source/ipc/AGENTS.md` PROTOCOL 表）。

```
→ {"cmd":"loadPlugin","path":"C:/VST/MyPlugin.vst3"}
→ {"cmd":"measure","type":"frequency_response"}
→ {"cmd":"exportData","path":"out/freq.json"}
```

手动客户端：`tools/ipc_client.ps1`。

## 文档导航

| 文档 | 内容 |
| --- | --- |
| `DESIGN.md` | 设计文档（方法论 + 开发目标） |
| `SPEC.md` | 8 类导出 JSON schema + IPC 协议契约 |
| `STATUS.md` | 决策史 + 阶段记录 + 已知限制 |
| `CONTEXT.md` | 领域词汇表 |
| `docs/adr/` | 架构决策记录（ADR） |
| `AGENTS.md` | AI 开发约束（结构地图 / 惯例 / 红线） |

## 目录结构

```
source/   生产代码（9 模块：host / child / signal / capture / scan / analysis / ipc / ui / utils）
tests/    Catch2 单元测试（含假插件 ground truth）
tools/    Python / PowerShell 工具脚本 + VST3Scanner（默认不构建）
samples/  vocal 测试素材（take01.wav）
docs/     adr / agents / 专项文档
```
