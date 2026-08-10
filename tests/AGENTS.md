# tests — 测试套件知识库

**leaf 文档** · 根级构建/命令见根 `AGENTS.md`，此处只写测试侧约定。

## OVERVIEW

Catch2 v3.8.0 单元测试（FetchContent）；`unit_tests` console target（`juce_add_console_app`）；208 个 TEST_CASE（2026-08-10 实测 208/208 绿：基线 186 + 块 E2 4 个 [multitone] + 块 E1 5 个（2 [freqresponse][mls] + 3 [commandparser][measure][excitation]）+ 块 E 审查修复 4 个（[freqresponse][mls] 1 + [commandparser][measure][excitation] 3）+ 块 B1 3 个（[wavexporter] 1 + [commandparser][exportwav] 2）+ 块 B2 6 个（[paramtimeline] 3 + [commandparser] timeline 命令 3））。`Catch2::Catch2WithMain` 提供 main()，测试源里没有 main()。

## RUN

- 构建/运行命令见根 `AGENTS.md` COMMANDS（`BUILD_TESTS=ON` → build → `ctest --timeout 180`）
- `catch_discover_tests` 注册进 CTest；连跑 2 次验稳定性
- scan-happy 测试天然慢（~78s），属预期

## LAYOUT

- 命名 1:1 镜像生产模块：`tests/<Module>Tests.cpp` ↔ `source/<module>/X.cpp`（如 `CommandParser.cpp` → `CommandParserTests.cpp`）；无同目录测试、无 `__tests__`
- 20 个源文件：17 个 test .cpp + `CommandParserStubs.cpp` + `TestPlugin.h` + `TestCompressorPlugin.h`
- 覆盖映射：除 `ui/`、`utils/`（FftHelper/MathUtils/CrashLog 无直接测试文件）和 `Main.cpp` 外每个生产模块都有测试文件
  - signal → ToneBurst/NoiseGenerator/EnvelopeSignal/FilePlayback
  - capture → SweepRunner/CaptureBuffer（2026-08-03 新增，WAV flush）
  - analysis → FreqResponse/GainReduction/TimeConstants/CompressionFamily/Export
  - scan → ScanEngine；ipc → CommandParser/PipeServer；host → 经 stubs
  - host → EditorCrashGuard（2026-08-08 块 C 任务 2：真实 EditorCrashGuard.cpp 编入测试目标，含 SEH 崩溃保护用例）
- 不编译进测试：`Main.cpp`、`CrashLog.cpp`、`ui/*`

## TEST DOUBLES

测试设施模式（test facility first 流程，出处 docs/archive/plan-phase2-5.md）：

1. **先建测试设施，再写被测代码**：任何依赖假插件/假数据的测试，先把 double（TestPlugin 扩展 / 新 double / fixture 生成）写出来并跑绿，再开发生产代码
2. **确定性优先**：噪声固定种子（42）、WAV fixture 运行时生成、无外部素材——任何测试两次运行结果必须一致
3. TDD 命令：写失败测试（RED）→ 最小实现（GREEN）→ 全量回归；设施本身也按此流程

- **TestPlugin.h** — 可配置假 `AudioPluginInstance`：任意增益、真实 N 采样延迟、busy-wait block hook（取消/并发测试用）
- **TestCompressorPlugin.h** — 确定性前馈压缩器，单极点数学有文档，是 τ 估计的 ground truth（误差 <10%）
- **CommandParserStubs.cpp** — 仅 CrashLog 桩（**记录型**：捕获 Error 级条目，提供 `clearCrashLog`/`crashLogErrorCount`/`crashLogContains` 测试辅助，断言异常保护路径有日志）；EditorCrashGuard 不再是桩——真实 `EditorCrashGuard.cpp` 编译进测试目标（/EHa，2026-08-08 块 C 任务 2）

## CONVENTIONS

- `TEST_CASE` 描述性命名 + tags（如 `[timeconstants][known-attack]`）
- Arrange/Act/Assert 注释分节
- DSP 浮点断言用 `Catch::Approx(...).margin(...)`
- 回调完成标志用 `std::atomic<bool>` 按引用捕获
- 断言前 `ensureMessageManager()` + `flushMessageManager(timeoutMs)` 排空 `MessageManager::callAsync`
- .wav fixture 运行时用 `juce::WavAudioFormat` 自生成到临时目录，**无外部素材**（P2-14）
- 噪声固定 seed 42；每个测试删除自己创建的临时导出文件
- 测试 target 同样 /W4 /permissive- /WX（警告即错误）
- TDD：先写头文件（RED 阶段）再实现

## GOTCHAS

- `PipeServerTests` 创建**真实** `\\.\pipe\PluginLab`：必须串行运行，且同时不能有其他 PluginLab 实例
- 禁外部音频素材、禁测试源里写 main()
- 不属于 ctest 的验证：`tools/verify_export.py`、`tools/reverse_derive.py`（stdlib-only Python，真插件验收，手动跑）
