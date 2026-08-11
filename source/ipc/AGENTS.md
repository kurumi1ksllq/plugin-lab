# IPC 模块 — AI/脚本 ↔ GUI 协议契约

## OVERVIEW

`source/ipc/` 是 AI/脚本与 GUI 的唯一通信入口：Named Pipe 收 JSON 行 → `CommandParser::handleCommand` 统一路由 → 返回 JSON。协议即契约，改协议必须同步 `docs/data-schema.md`。

## PROTOCOL

- 管道：`\\.\pipe\PluginLab`（`CreateNamedPipe`，PipeServer 后台线程，JSON 行协议——一行请求对应一行响应）
- 每行独立 JSON，命令类型用字段区分；响应统一 `{"ok":true,...}` / `{"ok":false,"error":"..."}`

| 命令                | 请求 → 响应                                                                                                                                                                                       |
| ------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| loadPlugin          | `{path}` → `{ok, params}`                                                                                                                                                                         |
| setParam            | `{name, value}` → `{ok, value}`                                                                                                                                                                   |
| getParams           | `{}` → `{ok, params}`                                                                                                                                                                             |
| measure             | `{type: frequency_response\|harmonic\|compression\|gr_timeline, source: signal\|file\|noise\|dynamic, excitation?: sweep\|mls}` → 单个响应 `{ok, export_path, wav_path}`（无进度流；长命令期间控制命令可达，见下方并发模型）                                                                                       |
| scan                | `{paramId, values}` → `{ok, runs, export_path, wav_path}`（曲线族经 export_path 导出 JSON）                                                                                                       |
| dataset             | `{path, types?[], scan?{param_id, values, type?}, compression_family?{levels_db, speeds}}` → `{ok, export_path, types{...}, scan, compression_family}`                                    |
| stop                | `{}` → `{ok}` ——取消当前任务：进程内测量/回放（`session->cancel()`）+ **子进程托管测量**（`ChildMeasureOrchestrator::cancel` → 子进程主动终止，返回 `{"ok":false,"error":"cancelled"}`，不触发崩溃计数/黑名单，issue #3）；**长命令期间可达**（控制命令内联服务）                                                                                                                                                                                     |
| exportData          | `{path}` → `{ok}`                                                                                                                                                                                 |
| exportWav           | `{path}` → `{ok, wav_path}` ——从最近一次测量的 CaptureBuffer 导出 3×声道 24-bit 多轨 WAV（dry/wet/bypass=dry 副本；路径 .json→.wav 复用 wavPathFor；B1） |
| recordTimeline      | `{}` → `{ok, recording:true}` ——参数自动化录制开始（**非阻塞**，C4 例外：立即返回，监听器保持挂载；D2 不录音频）；失败：`no plugin loaded` / `already recording`（B2） |
| stopTimeline        | `{path}` → `{ok, timeline_path, events:N}` ——停止录制并导出 `parameter_timeline` JSON（`{"type":"parameter_timeline","events":[{time_ms,param_id,value}]}`，§10）；失败：`not recording` / `path required` / `timeline export failed`（B2） |
| playTimeline        | `{path, rate?}` → 回放期推送进度行 `{"ok":true,"progress":<fraction>,"event_index":N,"event_total":M,"time_ms":T}`，完成后 `{ok, samples, rate, export_path, wav_path}` ——读取 timeline JSON 并在测量 run 中逐 block 播放自动化（阻塞，dispatch 同 measure）；导出 `parameter_timeline_play` JSON（`tl_play.json`，**绝不覆盖输入文件**）+ dry/wet WAV（§10）；R2：播放后恢复被触及参数的播放前值；失败：`no session or plugin` / `path required` / `file not found` / `invalid rate` / `invalid timeline json` / `measurement failed` / `wav export failed`（B2 + issue #2 进度行） |
| getScanStatus       | `{}` → `{ok, running, done, progress, count, blacklisted, hangCount, currentFile}`——插件扫描状态快照（计划步骤 5；快照+推送双轨的快照侧，中途连接者以此拿当前状态；命名避开参数扫描 `scan` 语义） |

- **进度行**：`playTimeline` 每应用一个事件推送一行（`Protocol::makeProgress` + `event_index`/`event_total`/`time_ms`）；客户端区分中间行与最终响应（最终响应带 `samples`/`export_path`/`error`）。GUI 面板同步显示「事件 N/M」（~50ms 节流）。`tools/ipc_client.ps1` 已支持多行读取 + `-CancelAfterMs` 连接内发 stop。
- **并发模型（issue #2/#3 改造）**：PipeServer 拆两轨——**控制命令**（`stop`/`getScanStatus`，`setControlCommands` 配置）在管道读线程**内联**执行（只碰原子/快照，线程安全）；**其余一切命令**（含 measure/playTimeline/dataset/scan 长命令与 setParam 等快命令）FIFO 排队到**单个 worker 线程**串行执行（杜绝并发触碰 plugin/session）。因此长命令期间 `stop` 可达（#3）、回放进度行可推（#2）。所有写（内联响应/worker 最终响应/进度行）经 `ioMutex` + 连接代数（generation）防写已关闭/复用句柄；读循环用 **PeekNamedPipe 轮询**（阻塞 ReadFile 会被并发 WriteFile 打断——实测 ERROR_PIPE_NOT_CONNECTED，见 PipeServer.cpp 注释）。长命令排队时若 worker 忙，新命令排队等待而非拒绝。
- dataset battery 语义：`types` 省略 → 默认全部 4 类型（frequency_response/harmonic/compression/gr_timeline）；source 固定映射——freq/harmonic/compression → signal，gr_timeline → dynamic（保证 τ 有效），v1 不支持覆盖；逐类型失败 → 跳过该块 + 响应 `types[type]=false`；响应单行（无进度流式），IPC 线程整段阻塞——期间 `stop` 不可达（与 scan 一致）；全部失败 → `ok:false` "all measurements failed"
- 频响激励（块 E 任务 1）：measure 命令可选 `excitation:"sweep"|"mls"`（缺省 sweep，向后兼容）；未知值 → `{"ok":false,"error":"unknown excitation ..."}`。dataset 命令同字段：同一 dataset 内全部 frequency_response 测量（battery freq 块 + scan 块）用同一激励；未知值 → 跳过 freq 块（确定性部分失败，与 scan/compression_family 块校验语义一致）。导出 `context.measurement.excitation` 仅非缺省值（mls）时输出
- dataset 可选块：`scan`（`param_id`+`values` 必填；`type` 省略默认 frequency_response，拒绝 gr_timeline）与 `compression_family`（`levels_db`/`speeds` 可省略，内部默认 `[-12,0]` × `[0.5,1,2]`）；校验失败仅跳过该块，其余照常执行
- JSON 手写 raw string literal + `escapeJsonString`；`Protocol.h` 持消息类型常量与响应辅助函数
- exportWav（块 B 任务 1）：离线全量导出上次测量 dry/wet——**3×插件声道**布局 `[dry, wet, bypass=dry 副本]`（立体声 → 6 声道 24-bit），与下节崩溃镜像（**2×声道**增量）是两种不同产物；实现 `WavExporter`，契约见 `docs/data-schema.md` §9
- 参数时间线（块 B 任务 2）：`CommandParser` 持 `ParameterTimeline timeline;` 做录制（recordTimeline/stopTimeline，非阻塞事件录制）；`playTimeline` 走 `session->setTimelinePlayback`（会话持回放时间线，逐 block 应用 + R2 恢复）；播放结果 JSON 路径 = 输入 timeline 的 sibling `*_play.json`（手工拼接，`withFileExtension("_play.json")` 会产出 `tl._play.json` 故不可用），WAV 复用 `wavPathFor`；契约见 `docs/data-schema.md` §10

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
- 已抽辅助：`parseSource` / `configureSessionSource` / `resolveExportPath` / `buildExportContext`（measure/scan 共用）；`runAndAnalyze` / `exportResultsToJSON`（自 measure case 提取，measure/dataset 共用）——勿重复实现

## ADDING A COMMAND

新增命令必须四件套（缺一不可）：

1. `Protocol.h` 加常量
2. `CommandParser::handleCommand` 加 case
3. `tests/CommandParserStubs.cpp` 加 stub + `tests/CommandParserTests.cpp` 加用例
4. 新导出走 `docs/data-schema.md` 补 schema
