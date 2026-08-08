# PluginLab 下一步路线图（2026-08-08 定稿）

> 阶段 2-5 + 扫描优化全部完成后，剩余工作按五个方向排期。2026-08-08 用户选定：**五个方向全做**，执行顺序 `C → E → A∥B → D`，**从 A（批量采集管线）先开始**（C 顺延，不改顺序本身）。
> 每块遵循项目铁律：TDD + 真机验收 + push + 代码审查；每块开工前单独规划（brainstorming → spec/tickets）。

---

## 一、执行顺序与依赖

```
C（稳定加固+残余小项）→ E（测量质量改进）→ A（批量采集管线）‖ B（记录模式）→ D（进程外托管）
```

- **A 与 B 可并行**：A 动 IPC + tools/（Python），B 动 capture + analysis，不踩文件
- **D 殿后且设设计门**：必须 brainstorming 定架构后才允许拆票实施
- 关键路径：`C → E → A → D`（B 挂旁路）

## 二、各块内容

### 块 0：C 稳定加固 + 残余小项（小，低风险）— 当前块

任务清单（按实施顺序）：

1. **测量路径异常保护**（2026-08-08 LA-2A 崩溃调查新增，最高优先）
   - 背景：`SweepRunner::run` → `plugin->processBlock()`（及 `prepareToPlay`/`releaseResources`）在消息线程同步执行，测量路径零 try/catch——插件抛 C++ 异常时逃逸到消息循环 → `std::terminate` → abort → 进程无征兆退出（无 crashFilter/WER/minidump，仅日志停在 Sweep start）。违反项目铁律"崩溃保护用 /EHa + catch(...) + CRASH_LOG"
   - 对策：在 host/（/EHa TU）提供受保护的 plugin 调用入口（或给 SweepRunner.cpp 开 /EHa），`catch(...)` + CRASH_LOG_ERR + 测量失败响应，宿主存活；同步补 `catch (const std::exception&)` 记录 what()
   - 验收：TestPlugin 注入 throwing processBlock 的假插件，测量返回错误响应不闪退；CRASH_LOG 有异常记录
2. **EditorCrashGuard 单独 /EHa TU**：当前不编译进测试（tests/AGENTS.md 列明），补进测试目标
3. **Generic 编辑器兜底**：插件无编辑器/编辑器创建失败时回退，不再空窗
4. **观察者指针清理**：生命周期加固
5. **CGII.vst3 预防性黑名单**：0 类型插件每轮热启重扫 ~0.5s（STATUS.md 已知残留）
6. **data-schema.md scan 结构描述与实现最终核对**（待改进项 1）
7. **getParams 响应带 Band Used 状态**（Pro-Q 4 Band 1，待改进项 3）
- **验收**：158+ 测试全绿；热启无 CGII 重扫；schema 与实现一致；异常保护项有测试锁定

### 块 1：E 测量质量改进（小-中）

- Impulse/MLS 接入 EQ 线性测量（比扫频快，DESIGN.md §9.2 P2）
- MultiTone 加随机初始相位（固定种子可复现，降峰值因子，DESIGN.md §9.2 P2）
- `MeasurementSession::runMultiple` 多轮参数扫描接口（DESIGN.md §9.2 P2，块 A 可选复用）
- **验收**：MLS 频响与扫频结果一致性测试；MultiTone 峰值因子下降且同种子可复现

### 块 2：A 批量采集管线（中低，Python 为主）— 当前块

- `dataset` IPC 导出命令（`datasetToJSON` 当前**未暴露**到 IPC，已核实 source/ipc 无引用；按 source/ipc/AGENTS.md 四件套新增）
- Python 无人值守采集管线（tools/，stdlib-only）：loadPlugin → 全测量类型（freq/harmonic/compression/grTimeline）→ dataset 聚合 → reverse_derive 反推报告
- 目标：把实验室变成 AI 可驱动、可批量、可复现的采集管线（核心目标"AI 反推"的最后一公里）
- **验收**：脚本驱动真插件（Pro-Q 4/Pro-C 3）全流程出 dataset + 反推报告；无人值守跑批多插件

### 块 3：B 记录模式（中大型 C++）

- WavExporter：干/湿/基准多轨 WAV（可听对比；WAV mirror 已部分覆盖，此块补完整录制模式）
- ParameterTimeline：参数自动化录制 + 回放
- 新 capture 模式 + IPC 命令 + GUI 面板
- **验收**：录制-回放参数时间线与输入一致；干/湿 WAV 时长/对齐正确

### 块 4：D 进程外托管（大工程，设计门）

- ChildProcessCoordinator：VST3 托管进子进程，Pianoteq 类 ExitProcess 杀进程插件的唯一根治
- **必须先 brainstorming**：子进程边界（scan/load/measure 哪些进子进程）、编辑器跨进程托管（子进程窗口）、与黑名单/死马踏板/看门狗/缓存的关系、进程崩溃恢复语义
- **验收**：Pianoteq 可加载测量不杀宿主；子进程崩溃自动重启续测

## 三、风险清单

| # | 风险 | 等级 | 缓解 |
|---|------|------|------|
| R1 | A 的 dataset 命令改变导出路径语义，破坏既有 measure/scan 兼容 | P1 | 只新增不改旧；四件套 + schema 文档先行 |
| R2 | B 记录模式与既有 sweep 管线并发冲突 | P1 | 串行化（复用 scan 的占用/排队语义） |
| R3 | D 跨进程编辑器窗口是公认难点，可能长期卡住 | P0 | 设计门：先定边界再拆票；若编辑器无法跨进程，明确降级方案（编辑器留宿主、测量进子进程） |
| R4 | 全块并行导致的提交混乱 | P2 | 每块独立分支 + push main 存档，按块验收 |

## 四、决策记录

1. **2026-08-08**：阶段 2-5 + 扫描优化完成后无既定下一步；五个方向（A 批量管线 / B 记录模式 / C 稳定加固 / D 进程外托管 / E 测量质量）全部纳入路线图。
2. **2026-08-08**：执行顺序 `C → E → A∥B → D`；**先做 A**（用户选择，跳 C 不跳顺序——C 在 A 之后补做）。
3. **2026-08-08**：本仓库无 issue tracker（无 .issues/、无配置），规划走对话 + 本文档，不进 wayfinder 票据地图；若后续需要再 setup-matt-pocock-skills。

## 五、执行状态

- [ ] 块 0 C 稳定加固 + 残余小项（**进行中**，任务清单见上；含 LA-2A 崩溃调查新增的"测量路径异常保护"）
- [ ] 块 1 E 测量质量改进
- [x] 块 2 A 批量采集管线（2026-08-08 完成，见 docs/plan-batch-pipeline.md 完成记录；真机验收 S1/S4 通过）
- [ ] 块 3 B 记录模式
- [ ] 块 4 D 进程外托管（设计门）

## 六、决策记录补充（2026-08-08 晚）

1. **LA-2A 崩溃调查结论**（用户确认已解决，调查记录保留备查）：
   - 现场：加载 UADx LA-2A → 点测量 EQ → 日志停在 "Sweep start" 后静默退出
   - 证据：无 UNHANDLED CRASH 日志、无 WER 事件（1000/1001）、无新 minidump → 非 SEH 崩溃，机制为 C++ 异常逃逸 → std::terminate → abort（对照 8/3 真崩溃有 ERROR UNHANDLED CRASH）
   - 代码事实：测量路径（SweepRunner::run → JUCE VST3 processBlock）零异常捕获；SweepRunner 每 block `runDispatchLoopUntil(2)` 消息循环重入，LA-2A 编辑器消息在测量期间被处理；JUCE 9 wrapper processBlock 亦无 try/catch
   - 复现实验：IPC 加载+测量 LA-2A（无编辑器窗口）**成功**（4s/240000 samples）；GUI 点击（编辑器窗口打开）闪退 → 差异为编辑器窗口，指向重入路径
   - 处置：加固工作并入块 C 任务 1（无论本次根因细节如何，测量路径异常保护缺失是确定缺陷）
