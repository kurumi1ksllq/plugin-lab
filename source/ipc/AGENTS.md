# IPC 模块 — AI/脚本 ↔ GUI 协议契约

## OVERVIEW

`source/ipc/` 是 AI/脚本与 GUI 的唯一通信入口：Named Pipe 收 JSON 行 → `CommandParser::handleCommand` 统一路由 → 返回 JSON。协议即契约，改协议必须同步 `docs/data-schema.md`。

## PROTOCOL

- 管道：`\\.\pipe\PluginLab`（`CreateNamedPipe`，PipeServer 后台线程，JSON 行协议——一行请求对应一行响应）
- 每行独立 JSON，命令类型用字段区分；响应统一 `{"ok":true,...}` / `{"ok":false,"error":"..."}`

| 命令                | 请求 → 响应                                                                                                            |
| ------------------- | ---------------------------------------------------------------------------------------------------------------------- |
| loadPlugin          | `{path}` → `{ok, params}`                                                                                              |
| setParam            | `{name, value}` → `{ok, value}`                                                                                        |
| getParams           | `{}` → `{ok, params}`                                                                                                  |
| measure             | `{type: freq\|harmonic\|compression\|grTimeline, source: signal\|file\|noise\|dynamic}` → `{ok, progress...}` 流式推进 |
| scan                | `{paramId, values}` → `{ok, family}`（曲线族）                                                                         |
| stop                | `{}` → `{ok}`                                                                                                          |
| exportData          | `{path}` → `{ok}`                                                                                                      |
| getState / 其余状态 | → `{ok, ...}`                                                                                                          |

- 进度流式推送：measure 期间持续发 `{"ok":true,"progress":0.10}` 行，完成后发最终结果
- JSON 手写 raw string literal + `escapeJsonString`；`Protocol.h` 持消息类型常量与响应辅助函数

## RAW-CAPTURE WAV MIRROR（已接 IPC）

`CaptureBuffer` 实现 dry/wet 增量 WAV 镜像（`setFlushConfig`，24-bit PCM 交织 `[dry ch0..N-1, wet ch0..N-1]`，见 `capture/AGENTS.md`），**已接入协议**：

- measure/scan 运行前调用 `session->getResult().setFlushConfig (wavPathFor (exportPath), kDefaultFlushIntervalSec)`（`wavPathFor` = 导出 JSON 路径 `.json→.wav`；默认间隔 5s）
- 成功响应携带 `wav_path` 字段 = dry/wet WAV 镜像路径（崩溃安全镜像，分析仍用内存 buffer）
- scan 多轮复用同一会话 → 每轮首 append 覆盖 .wav（最后轮胜出）

## TWO ENTRY PATHS

两条路径汇聚于 `CommandParser::handleCommand`，协议层无区分：

1. **GUI 直调**：按钮在消息线程同步调 `handleCommand`
2. **IPC 异步**：PipeServer 后台线程收命令 → 同一 handler

- 上层（Main.cpp）注入回调：`setPluginManager` / `setSession` / `setPluginInstance` / `setLoadPluginCallback`（消息线程）/ `setStatusCallback` / `setMeasurementCompleteCallback`（消息线程，`MeasurementResults`）/ `setScanCompleteCallback`（测量线程，`ScanResult`）

## THREADING

- 命令在 IPC 线程执行；UI 相关操作经 `MessageManager::callAsync` 派发到消息线程
- measure/scan 用 `WaitableEvent` + `callAsync` 回消息线程（Pro-Q 4 要求编辑器线程跑 processBlock）
- `isThisTheMessageThread()` 同步路径（CommandParser 内）避免测试挂起
- `JUCE_MODAL_LOOPS_PERMITTED=1` 让 SweepRunner 让步循环
- `MeasurementResults`：按类型恰有一个 Result 字段填充（freq/harmonic/compression/gr/tau）；非信号源只带原始捕获元数据（source/rawSamples/rawSampleRate），grTimeline 例外（分析 dry/wet）
- 已抽辅助：`parseSource` / `configureSessionSource` / `resolveExportPath` / `buildExportContext`（measure/scan 共用，勿重复实现）

## ADDING A COMMAND

新增命令必须四件套（缺一不可）：

1. `Protocol.h` 加常量
2. `CommandParser::handleCommand` 加 case
3. `tests/CommandParserStubs.cpp` 加 stub + `tests/CommandParserTests.cpp` 加用例
4. 新导出走 `docs/data-schema.md` 补 schema
