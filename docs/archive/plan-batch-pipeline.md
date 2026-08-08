# PluginLab 块 A：批量采集管线开发计划

> 2026-08-08 制定。brainstorming 访谈 5 问定案（Q1 方案 1 C++ 一站式 / Q2 先单后多 / Q3 pywin32 / Q4 4 类型默认+可选扩展 / Q5 产物布局认可），用户确认后开工。路线图总览见 `docs/roadmap-next.md`。
> 执行原则：TDD（RED→GREEN）+ 真机验收 + 原子提交 push + 代码审查；全程无 GUI 鼠标（IPC 驱动）。

## 一、目标

把实验室变成 AI 可无人值守驱动的采集管线：一条 IPC 命令跑完全部核心测量并聚合为 dataset 包，一个 Python 脚本完成 插件枚举 → 加载 → 测量 → 反推报告 → 批量汇总 的闭环。

## 二、C++ `dataset` 命令（方案 1：一站式）

### 请求

```json
{"cmd":"dataset",
 "path":"out/fabfilter-pro-q-4/dataset.json",
 "types":["frequency_response","harmonic","compression","gr_timeline"],  // 省略=默认全部 4 类型
 "scan":{"param_id":"...","values":[...]},                                // 可选：参数扫描族
 "compression_family":{"levels_db":[-12,0],"speeds":[0.5,1,2]}}          // 可选：电平×速度网格（缺省用内部默认）
```

### 行为

- **source 映射（固定内置，v1 不支持覆盖）**：`freq/harmonic/compression → signal`；`gr_timeline → dynamic`（τ 有效）。
  - 设计偏差记录：brainstorming 中"可被请求字段覆盖"删除——因 measure 的 file 源 `path` 字段与 dataset 导出 `path` 字段语义冲突，且驱动不需要 file 源（file 源仍可单独走 measure 命令）。YAGNI。
- **执行模型**：与 measure/scan 相同——消息线程同步路径 / IPC 线程 `callAsync + WaitableEvent` 双路径；整段 battery 在消息线程顺序执行（与 scan 阻塞语义一致，文档写明"期间 stop 不可达"）。
- **逐类型失败**：单类型失败 → 跳过该块（datasetToJSON 缺失块自动省略），继续后续；响应逐类型记录成败。全失败 → `ok:false`。
- **scan 可选块**：param_id/values 校验失败 → 该块跳过 + `"scan":false`，其余照常（确定性部分失败，S3）。
- **compression_family 可选块**：`CompressionFamily::measure(plugin, session, levelsDB, speeds, progress)`。
- **导出**：`Export::Dataset` 聚合（scan / compressionFamily / grTimeline+grTau）+ `datasetToJSON` + `writeToFile`。
  - **实施修正（2026-08-08，plan agent 核实后补充）**：原 `Dataset` 结构只含 scan/cf/grTimeline——battery 的 freq/harmonic/compression 结果会被丢弃，与 Q4"S1 契约（dataset.json 含 freq/harmonic/compression 块）"冲突（EQ 插件反推报告将为空）。**扩展 `Export::Dataset` + `datasetToJSON` 增加 frequency_response / harmonic / compression 三个可选块**（复用现有 append 辅助，body-equiv 测试锁定块等价）；data-schema.md §8 变更记录。
- **WAV 镜像**：`setFlushConfig(wavPathFor(path), kDefaultFlushIntervalSec)`——与 measure/scan 一致（最后类型胜出，与 scan 多轮语义相同）。

### 响应

```json
{"ok":true,"export_path":"...",
 "types":{"frequency_response":true,"harmonic":true,"compression":true,"gr_timeline":false},
 "scan":true,"compression_family":false}
```

### 实现结构（CommandParser.cpp，无新 .cpp → CMake 零改动）

1. **重构（零行为变化，现有测试锁定）**：从 measure case 提取匿名命名空间共享 helper：
   - `static bool runAndAnalyze(MeasurementSession*, AudioPluginInstance*, Type, Source, MeasurementResults&, juce::String& error)` —— session->run() + 分析器 dispatch（signal 源按 type 分析；gr_timeline 非 signal 源 GR+τ 分析；其他非 signal 源 raw 元数据）
   - `static juce::String exportResultsToJSON(Type, const MeasurementResults&, const Export::Context&)` —— 按 type 调各 Export::xxxToJSON（measure 的 exportJson 构建 switch 移入）
   - measure case 改为调用两个 helper（行为逐字等价）
2. **新 case** `if (cmd == Protocol::Command::dataset)`：解析 types（默认 4）/scan/compression_family → 顺序执行 → 聚合 → 导出 → 响应。
3. **四件套**：`Protocol.h` 加 `constexpr auto dataset = "dataset";`；`CommandParserStubs` 无需改（无新依赖）；`CommandParserTests` 加用例；`docs/data-schema.md` + `source/ipc/AGENTS.md` 协议表补 dataset 行。

### 线程细节

- dataset case 内部对每类型设置 `session->setSource/setPluginInstance` 后调 `session->run()`——与 measure 相同；`stop` 命令在 IPC 线程阻塞期间不可达（文档化，GUI 按钮仍可同步取消，与 scan 现状一致）。

## 三、Python 驱动（tools/，pywin32 仅限一个模块）

### `tools/pipe_client.py` —— pywin32 薄客户端

- `connect(retries)`：`win32pipe.CreateFile`，`ERROR_PIPE_BUSY` 重试（`WaitNamedPipe`）；缺 pywin32 → 清晰报错 `pip install pywin32`
- `send_line(json)` / `read_line(timeout)` / `close()`
- 全脚本唯一 pywin32 依赖点，其余纯 stdlib

### `tools/batch_collect.py` —— CLI 驱动

```
--plugin NAME   单插件模式（按名 loadPlugin，大小写不敏感）
--all           批量模式（遍历 %APPDATA%/PluginLab/pluginlist.xml，跳过 <BLACKLISTED>）
--out DIR       输出根目录（默认 out/）
--types LIST    覆盖默认 4 类型（逗号分隔）
--config FILE   每插件配置 JSON（可选 scan/expected 声明）
--limit N       批量上限（试跑用）
--launch        自动启动 PluginLab.exe（默认连现有实例）
--dry-run       解析+校验配置，不连管道（自检）
--quit          结束时按 PID 找窗口发 WM_CLOSE 关闭应用
```

流程：连接（--launch 先起进程轮询管道就绪）→ 轮询 `getScanStatus` 至 done → 逐插件 `loadPlugin`（失败重试 1 次后跳过记原因）→ `dataset`（默认 4 类型 + 配置可选块）→ 收 export_path → `subprocess` 调 `tools/reverse_derive.py`（有 expected 走 `--expected-*` 容差模式）→ `report.txt` → 批量最后 `summary.json`（每插件 ok/fail、耗时、反推 exit code、跳过原因）。

### 产物布局（Q5 定案）

```
out/<slug>/dataset.json   # dataset 命令导出
out/<slug>/dataset.wav    # 干/湿镜像（最后类型）
out/<slug>/report.txt     # reverse_derive 输出
out/summary.json          # 批量汇总
```
slug = 小写、非字母数字转 `-`。

## 四、测试与验收

### C++（ctest，TDD）

| 用例 | 断言 |
|---|---|
| dataset 默认 4 类型 | 响应 ok、export_path 存在、types 四键全 true（TestPlugin） |
| body-equiv | dataset 命令产物 == 单独 measure×4 后聚合（锁定共享 helper 等价 + 新增三块等价，ExportTests） |
| scan 参数缺失 | `"scan":false` + 其余类型成功（S3） |
| unknown type | `ok:false "unknown measure type"` |
| 无插件 | `ok:false "no session or plugin"`（S2） |
| 回归 | 现有 measure/scan 测试全绿（S5） |

### Python

- `--dry-run` 自检（解析 pluginlist.xml + 配置校验，S6）；不引入 pytest。

### 真机验收（无鼠标，全 IPC）

- S1：Pro-Q 4 单插件闭环 → dataset.json 4 块可解析 + report.txt + reverse_derive exit 0
- S4：`--all --limit 3` → summary.json 3 条目
- Pro-C 3：gr_timeline 块 τ valid=true（动态源）
- 产物校验：JSON 可解析 + 块齐全 + summary 形状

## 五、风险

| # | 风险 | 等级 | 缓解 |
|---|------|------|------|
| R1 | measure 重构破坏既有行为 | P0 | 机械提取 + 现有测试锁定 + body-equiv |
| R2 | battery 分钟级阻塞 IPC 线程，stop 不可达 | P1 | 与 scan 现状一致，文档写明；GUI 仍可同步取消 |
| R3 | 真机 batch 遇挂起插件拖垮整轮 | P1 | loadPlugin 30s 超时已有；失败跳过 + summary 记原因 |
| R4 | pywin32 依赖漂移 | P2 | 仅 pipe_client.py 依赖，import 失败清晰报错 |
| R5 | --launch 后应用未就绪竞态 | P1 | 轮询管道就绪（30s）+ getScanStatus done 轮询（300s） |

## 六、执行状态

> 2026-08-08 完成：全部交付 + 真机验收通过（见下）。

- [x] 设计文档落盘（本文件）
- [x] C++ 重构 helper（measure 零行为变化）
- [x] C++ dataset case + Protocol.h 常量
- [x] tests dataset 用例 RED→GREEN
- [x] docs 协议表补 dataset
- [x] tools/pipe_client.py
- [x] tools/batch_collect.py 单插件
- [x] tools/batch_collect.py 批量
- [x] 真机验收 S1/S4
- [x] 全量回归 + push

### 完成记录（2026-08-08）

- **测试**：169/169 全绿（连跑 2 次）；/W4 /WX 零警告
- **真机验收（无鼠标，全 IPC 驱动）**：S1 Pro-Q 4 闭环（dataset.json 4 块 1MB + dataset.wav + report.txt，reverse_derive exit 0）；Pro-C 3 `gr_timeline.tau.valid=true`（动态源）；S4 `--all --limit 3`（Scepter/UADx Vibe ok，UADx 1176 宿主崩溃 → AppGoneError 中止 + summary 记录，无 traceback）
- **验收期修复的 4 个真机 bug**（非计划内容，真机暴露）：
  1. `pipe_client` 读超时用 `SetNamedPipeHandleState` 字节模式超时参数 → 消息型管道报 winerror 87 → 改 `PIPE_READMODE_MESSAGE | PIPE_NOWAIT` + Python 截止轮询
  2. `loadPlugin/setParam/getParams/measure/scan` 响应用 `String::quoted()` → 双引号非法 JSON（`"name":""Pro-Q 4""`）→ 7 处改 `escapeJsonString` + strict-JSON 回归测试
  3. `loadPlugin` 同步响应但加载异步 → dataset 竞态 "no session or plugin" → 驱动轮询 getParams 等待加载完成
  4. `close_app_by_pid` 给隐藏系统窗口（IME/JUCEWindow）发 WM_CLOSE → app 挂起 → 只对可见顶层窗口发；`quit_app` 等待退出 + 10s 兜底 taskkill
- **驱动健壮性**：`ConnectionError`（宿主被插件搞崩）→ `AppGoneError` 中止批量并写 summary；命令级失败（超时/坏响应）→ 失败条目继续
- **reverse_derive.py**：补解析 dataset 新块 `frequency_response`/`compression`（EQ 报告不再为空）
