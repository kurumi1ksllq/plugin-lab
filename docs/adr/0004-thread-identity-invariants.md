# 0004 — 跨模块线程身份不变量显性化

- **日期**: 2026-11-27
- **状态**: 已接受（issue #117）
- **范围**: 文档化——零运行时行为变更、零新依赖、零新增 .cpp

## 背景

跨模块的线程身份约定（prepareToPlay/processBlock 在哪个线程跑、编辑器在哪个线程建、
长命令体在哪个线程执行）目前只靠散落在各文件里的注释维持（SweepRunner.cpp:40-44、
CommandParser.cpp:1968-1973、CommandParser.h:174-179、PluginManager.cpp:523-525）。
注释各自只描述局部，没有一处回答"完整契约是什么、谁保证、在哪失效"。本 ADR 把
不变量逐条收拢成单一契约文档，每条给出证据 file:line、保证机制与失效后果。

## 决策：契约文档化（方案 a），不做运行时断言（方案 b）

- **文档化**：本 ADR 即契约；AGENTS.md「线程铁律」节加一行指向本文。
- **不做 ThreadIdentityCheck.h 类调试断言**：运行时零风险优先——新增 header 会进
  双编译清单（根 + tests），/W4 /WX 下需本地无法验证的编译面；且断言只是把注释
  翻译成代码，契约本身已经能审。现有守卫点（见下表"守卫"列）已覆盖最危险的两条
  （编辑器窗口构造/析构、测量线程让步循环），无需重复加断言。若未来新增跨模块
  线程入口，再评估最小断言。

## 不变量清单

> 线程命名：**消息线程** = juce MessageManager 派发线程；**测量线程** = 同步执行
> SweepRunner::run 的调用线程（见 I-1，实为消息线程）；**扫描/加载线程** =
> PluginManager 的一次性专用 std::thread。

### I-1 prepareToPlay / processBlock / releaseResources 必须与 SweepRunner::run 同线程

- **入口**: SweepRunner::run()（source/capture/SweepRunner.cpp:53-72 prepare 段、
  :90-96 processBlock 段、:228-235 teardown 段）
- **要求**: prepareToPlay（SweepRunner.cpp:58）与其后所有 processBlock
  （SweepRunner.cpp:96）在同一调用线程上执行——部分 VST3 插件跟踪线程亲和性
  （FabFilter Pro-Q 4），换线程会静默产错音/挂起。
- **谁保证**: ①loadPlugin 故意**不**调 prepareToPlay（source/host/PluginManager.cpp:523-525
  注释明示 defer 到测量线程；它在加载线程执行，违者即换线程）；②命令体被
  dispatchToMessageThread 统一送到消息线程（见 I-2），IPC 路径下 run() 的调用线程
  就是消息线程；③单元测试/消息回调内直接同步执行（同一线程，天然满足）。
- **在哪失效**: 若有人绕过 dispatchToMessageThread 在 PipeServer 线程直接调
  MeasurementSession::run()（source/capture/MeasurementSession.h:103），
  prepareToPlay/processBlock 便落在管道线程——**当前无运行时守卫，靠本条契约约束**。
- **违反后果**: 部分插件（Pro-Q 4 类）静默输出错误音频或挂起；无崩溃日志（非异常路径）。

### I-2 长命令体（measure/scan/dataset/playTimeline）必须在消息线程执行

- **入口**: CommandParser::dispatchToMessageThread
  （source/ipc/CommandParser.cpp:1966-1982，声明 source/ipc/CommandParser.h:179）
- **要求**: 四类长命令的 body 里所有插件调用走消息线程。
- **谁保证**: dispatchToMessageThread 是唯一分发点（#42 重构把四处文本相同的
  WaitableEvent 块收拢）：已在消息线程 → 同步执行（CommandParser.cpp:1974-1975）；
  否则 callAsync + WaitableEvent 把 body 抬到消息线程（:1977-1980）。
  四个调用点：measure CommandParser.cpp:1016、scan :1175、dataset :1643、
  playTimeline :1953。ScanEngine 亦感知此契约：仅在消息线程上让步消息循环
  （source/scan/ScanEngine.cpp:113-114）。
- **在哪失效**: 新增长命令若手写 WaitableEvent + callAsync（绕过 helper），
  守卫失效——helper 注释（CommandParser.cpp:1968-1973）+ 本条约束新代码必须复用
  dispatchToMessageThread。同步快路径依赖调用方真实处于消息线程，无断言兜底。
- **违反后果**: 与 I-1 叠加——processBlock 落在管道线程；或若在消息线程直接
  done.wait() 则死锁（消息线程等自己）。

### I-3 编辑器创建/销毁必须在消息线程

- **入口**: PluginEditorWindow 构造与析构（source/ui/PluginEditorWindow.cpp:16、:69，
  已有 jassert(juce::MessageManager::existsAndIsCurrentThread()) 守卫）
- **要求**: VST3 编辑器（createEditor / editorBeingDeleted / 析构）只在消息线程触碰。
- **谁保证**: ①PluginEditorWindow 构造/析构内 jassert 守卫（仅调试期生效）；
  ②EditorCrashGuard（source/host/EditorCrashGuard.*）为 /EHa TU，崩溃保护但不负责
  线程检查；③上层入口（GUI 按钮、IPC loadPlugin 回调经 callAsync，见
  source/ipc/CommandParser.cpp:503、:594）均在消息线程触发。
- **在哪失效**: jassert 在 release 构建被剥除（PluginLab target 设
  JUCE_DISABLE_ASSERTIONS=1，AGENTS.md NOTES），且仅 PluginEditorWindow 一处——
  绕过该窗口直接 createEditorSafe（source/host/PluginManager.cpp:530-533）无守卫。
- **违反后果**: VST3 编辑器跨线程触碰 UI/COM → 间歇崩溃或 UI 死锁（插件相关，难复现）。

### I-4 扫描/加载用专用一次性 std::thread，析构不 join，禁 detach

- **入口**: PluginManager 扫描线程与 loadPlugin 线程（source/host/PluginManager.*，
  汇编层见 source/Main.cpp:541-557 的 release() 放弃逻辑）
- **要求**: worker 挂死在插件 DLL 内时绝不 join（进程永不退出）；也不允许
  std::thread::detach（放弃句柄、无界泄漏更难审计）。
- **谁保证**: Main.cpp 析构序列（source/Main.cpp:549-557）：先置 alive 标志 false，
  再仅对已 signal completion 的线程 join，否则 release() 放弃；worker 不触碰宿主
  成员（shared_ptr 状态 + callAsync alive-guard，Main.cpp:455-461 是 child 侧同款）。
- **在哪失效**: 新增后台 worker 若照搬 join-无条件模式（不看 completion 标志），
  插件挂死时进程退出被永久阻塞。**契约靠 AGENTS.md 铁律 + 本条，无编译期守卫**。
- **违反后果**: 进程退出挂死（join 卡在 DllMain 死锁）或无界线程泄漏（detach）。

### I-5 子进程编排：run() 必须在消息线程，restart() 不得在 onCrash 回调（reader 线程）执行

- **入口**: ChildMeasureOrchestrator::run（source/host/ChildMeasureOrchestrator.h:47-51
  THREADING 节）、PluginHostChildCoordinator::restart
- **要求**: run() 同步阻塞在消息线程；onCrash 回调在 coordinator 的 reader 线程触发，
  回调内禁止 restart()/join reader 线程。
- **谁保证**: ①orchestrator 头注释明示契约（ChildMeasureOrchestrator.h:47-51）；
  ②onCrash 回调体只做黑名单写（mutex 保护）+ callAsync 状态更新
  （source/Main.cpp:432-462，alive-guard :455-461），restart 由 run() 从消息线程发起；
  ③coordinator 设计上拒绝从 onCrash 内 restart（Main.cpp:429-431 注释）。
- **在哪失效**: onCrash 回调内若直接 join reader 线程或 restart → 自我等待死锁。
  守卫是 coordinator 的运行时拒绝 + 代码审读，无编译期保证。
- **违反后果**: reader 线程 join 自己 → 死锁或 std::system_error；子进程编排失效。

## 守卫点汇总

| 不变量 | 运行时守卫 | 静态/文档守卫 |
|--------|------------|---------------|
| I-1 | 无 | 本 ADR + SweepRunner.cpp:40-44 注释 + PluginManager.cpp:523-525 |
| I-2 | isThisTheMessageThread 分支（CommandParser.cpp:1974） | helper 收拢（#42）+ 本 ADR |
| I-3 | jassert（PluginEditorWindow.cpp:16,69；release 被剥除） | 本 ADR + AGENTS.md 铁律 |
| I-4 | 无（析构序列人工保证） | AGENTS.md 铁律 + 本 ADR |
| I-5 | coordinator 拒绝 onCrash 内 restart | ChildMeasureOrchestrator.h:47-51 + 本 ADR |

## 为什么不动运行时行为

- 本地无编译器（无 cmake/MSVC），新增 header/断言的 /W4 /WX 编译面无法本地验证；
  文档化零编译风险。
- 最危险的两条（I-3 编辑器、I-2 同步快路径）已有运行时守卫或结构性收拢；
  其余三条靠"单一契约文档 + AGENTS.md 指针"集中可见，收益等同而成本为零。
- 符合项目明示原则「不做过度设计」。

## 关联

- AGENTS.md「CONVENTIONS → 线程铁律」节（一行指针指向本文）
- #42（dispatchToMessageThread 收拢）、#33（D1 in-flight 守卫）、块 D（子进程编排）
