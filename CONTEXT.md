# CONTEXT — PluginLab 领域词汇表

> 项目术语词典：统一每个反复出现的词的确切含义，供所有对话、spec、tickets 使用。由 `/domain-modeling`(grill-with-docs) 维护扩充。**设计/工程/状态详见 DESIGN.md / SPEC.md / STATUS.md。**

## 测量(measurement)

| 术语 | 含义 |
|------|------|
| 测量类型 (type) | `frequency_response`(频响) / `harmonic`(谐波) / `compression`(压缩曲线) / `gr_timeline`(GR 时间线) |
| 激励源 (source) | `signal`(信号发生器) / `file`(音频文件) / `noise`(噪声) / `dynamic`(动态信号) |
| 黑盒测量 | 不依赖插件内部先验知识，仅通过干/湿信号推断处理方式 |

## 插件加载与托管

| 术语 | 含义 |
|------|------|
| 白名单插件 | 宿主进程直接加载测量(VST3 在进程内) |
| 黑名单插件 | 曾崩溃/挂起被登记，**进程外托管**测量(`source/child/` PluginHostChild) |
| 子进程托管 (hosted) | 黑名单插件由独立子进程加载测量，宿主通过 stdin/stdout JSON 协议驱动 |
| 崩溃恢复 (D3) | 子进程崩溃→自动重启→参数快照恢复→续测;连续崩溃 ≥3 次拒绝(不黑名单宿主) |
| 死马踏板 | `%APPDATA%/PluginLab/deadMansPedal`——挂起/崩溃插件下次自动黑名单 |

## 采集与分析

| 术语 | 含义 |
|------|------|
| SweepRunner | 冻结的 generate→process→capture 管线(永不修改) |
| 干/湿信号 | dry(输入)/ wet(插件输出)对比;bypass=dry 副本 |
| 参数快照 | 测量前记录参数值,测量后恢复(R2) |
| 回放进度 | playTimeline 期间推送的 event_index/event_total(issue #2) |

## IPC

| 术语 | 含义 |
|------|------|
| 命令 (cmd) | Named Pipe JSON 行协议字段;控制命令(stop/getScanStatus)内联,其余 worker 排队 |
| 最终响应 | 带 `samples`/`export_path`/`error` 的响应行(客户端区分中间行) |
| 进度行 | playTimeline 期间推送的中间行(带 `event_index`) |
