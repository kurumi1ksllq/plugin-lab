# Plugin Lab — 项目知识库

**更新:** 2026-08-11 · **Commit:** 020e798 · **Branch:** main

## OVERVIEW

VST3 插件黑盒测量实验室（Windows 桌面 GUI，C++20 + JUCE 9 + CMake + Catch2）。AI 通过 Named Pipe IPC（`\\.\pipe\PluginLab`）驱动 GUI：加载插件 → 测量（扫频/谐波/压缩/GR 时间线）→ JSON 导出 → 反推插件参数。纯黑盒原则——不依赖插件内部先验知识。**开发目标（最终目标）：让 AI 用测量数据反推插件"处理方式"作为后续 VST 开发规格——见 `DESIGN.md` 开头「开发目标」章节**。设计/状态文档：`DESIGN.md`（方法论+协议）、`STATUS.md`（决策史+阶段记录）、`SPEC.md`（8 类导出 JSON schema）。

## STRUCTURE

```
PluginLab/
├── source/            # 全部生产代码（73 文件，9 模块，含块 D 新增 child/）——见 source/AGENTS.md
│   ├── Main.cpp       # 入口 + 装配中枢（1829 行 god file）
│   ├── host/          # VST3 扫描/加载（/EHa 崩溃保护 + 黑名单）
│   ├── signal/        # 信号生成器接口 + 7 生成器
│   ├── capture/       # 采集管线（SweepRunner 冻结 / MeasurementSession）
│   ├── scan/          # 参数扫描引擎 ScanEngine
│   ├── analysis/      # 6 分析器 + Export JSON 层
│   ├── ipc/           # Named Pipe 服务器 + 命令解析
│   ├── ui/            # PlotWidget + PluginEditorWindow
│   └── utils/         # FftHelper / MathUtils / CrashLog
├── tests/             # Catch2 单元测试(270/270 绿,计数以 tests/AGENTS.md 为准)——见 tests/AGENTS.md
├── tools/             # VST3Scanner(死代码但仍在构建) + CMakeLists + PS/Python 工具脚本
├── samples/take01.wav # vocal 测试素材（已入库）
├── SPEC.md            # 工程文档：8 类导出 JSON schema + IPC 协议契约（原 docs/data-schema.md）
├── DESIGN.md          # 设计文档（方法论 + 开发目标）
└── STATUS.md          # 状态文档（决策史 + 阶段记录 + 已知限制）
```

> 文档体系（2026-08-11 定）：本地仅 `SPEC.md`（工程）/ `DESIGN.md`（设计）/ `STATUS.md`（状态）+ `AGENTS.md`（开发约束）；**需求与待开发全部走 GitHub issue**（已完成计划/路线图内容在 git 历史与 PR 记录中，本地不另存）。

## WHERE TO LOOK

| 任务                      | 位置                                                 | 备注                                        |
| ------------------------- | ---------------------------------------------------- | ------------------------------------------- |
| 插件扫描/加载/崩溃保护    | `source/host/PluginManager.cpp`                      | /EHa + Pianoteq 黑名单                      |
| 测量执行（4 类型 × 4 源） | `source/capture/MeasurementSession.*`                | 类型: freq/harmonic/compression/grTimeline  |
| 参数扫描                  | `source/scan/ScanEngine.*`                           | 快照/恢复/取消 RAII                         |
| 信号生成（新增生成器）    | `source/signal/`                                     | 实现 `SignalGenerator` 接口                 |
| JSON 导出/格式化          | `source/analysis/Export.cpp`                         | 手写转义，非 juce::JSON                     |
| 录制 WAV 导出（6 声道多轨）| `source/analysis/WavExporter.*`                      | `exportTracks` 24-bit `[dry,wet,bypass=dry]` |
| 参数自动化录制/回放        | `source/capture/ParameterTimeline.*`                 | AudioProcessorListener + rate 可配置回放    |
| IPC 命令                  | `source/ipc/` + `SPEC.md`                | 协议契约（含 `getScanStatus` 扫描状态快照） |
| 实时曲线渲染              | `source/ui/PlotWidget.cpp`                           | 增量绘制 + 50ms 节流                        |
| 崩溃日志/minidump         | `source/utils/CrashLog.cpp`                          | `%TEMP%\pluginlab_crashlog.txt`             |
| 导出 JSON 反推验证        | `tools/reverse_derive.py`、`tools/verify_export.py` | stdlib-only Python                         |
| IPC 手动客户端            | `tools/ipc_client.ps1`                               | NamedPipe 客户端，可配超时                  |
| 测试设施（假插件）        | `tests/TestPlugin.h`、`tests/TestCompressorPlugin.h` | 确定性 ground truth                         |

## CODE MAP

| 符号                                            | 类型         | 位置               | 角色                                                                 |
| ----------------------------------------------- | ------------ | ------------------ | -------------------------------------------------------------------- |
| `PluginLabApplication` / `MainContentComponent` | class        | `source/Main.cpp`  | 入口；装配 PluginManager/Session/CommandParser/PipeServer/专用线程  |
| `PluginManager`                                 | class        | `source/host/`     | VST3 扫描（专用一次性线程）+ loadPlugin + createEditorSafe（/EHa）  |
| `CommandParser::handleCommand`                  | method       | `source/ipc/`      | 命令唯一入口（GUI 与 IPC 双路径汇聚）                                |
| `PipeServer`                                    | class        | `source/ipc/`      | `\\.\pipe\PluginLab`，后台线程，JSON 行协议                          |
| `MeasurementSession`                            | class        | `source/capture/`  | 测量编排（type + source），51 符号                                   |
| `SweepRunner`                                   | class        | `source/capture/`  | 冻结的 generate→process→capture 管线（不改）                         |
| `ScanEngine`                                    | class        | `source/scan/`     | 参数多轮扫描，返回曲线族                                             |
| `Export` / `datasetToJSON`                      | namespace/fn | `source/analysis/` | 手写 JSON + 数据包聚合                                               |
| `PlotWidget`                                    | class        | `source/ui/`       | EQ/压缩/谐波/GR 四种图                                               |

## COMMANDS

```bash
# 构建（MSVC，/W4 /permissive- /WX /utf-8 警告即错误）
cmake -S . -B build && cmake --build build --config Release
# 运行
build\PluginLab_artefacts\Release\Plugin Lab.exe
# 测试（BUILD_TESTS 默认 OFF；连跑 2 次验稳定）
cmake -S . -B build -DBUILD_TESTS=ON && cmake --build build --config Release
ctest --test-dir build -C Release --timeout 180
# CI 见 .github/workflows/build.yml（build-and-test：Windows/MSVC 构建 + 测试，PR 门禁）
# 无 Makefile、无 package.json scripts（勿引用）
```

## WORKFLOW（GitHub 标准化开发流程）

- **永远不在 main 上开发**：main 有分支保护（服务端强制）——直接 push 被拒，一切变更走 PR
- 分支命名：`feat/*`、`fix/*`、`chore/*`、`docs/*`（如 `feat/ipc-export-wav`）；本地旧分支 `feat/phase1-*`、`feat/phase2-*` 为历史遗留
- 流程：`git checkout -b feat/xxx` → 提交（Conventional Commits）→ `git push -u origin feat/xxx` → `gh pr create` → CI 绿 → 合并（squash，合后自动删分支）
- CI 门禁：required status check `build-and-test`（`ctest` 连跑 2 次，/W4 /WX 零警告）
- 合并策略：仅 squash + 线性历史；禁 force push / 禁删保护分支

## CONVENTIONS

- **无 clang-format**——风格人工维护：JUCE 惯例（PascalCase、`jmax/jmin`、`JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR`、`//===` 80 列分栏）
- **新增 .cpp 必须同时加进根 `CMakeLists.txt` 和 `tests/CMakeLists.txt` 的 target_sources**（无静态库，双编译）
- 命名带单位：`thresholdDB`、`grDB`、`attackSec`、`carrierStartHz`；测试常量 `k` 前缀
- 头文件 `#pragma once`；include 相对当前文件（`"../analysis/Export.h"`）
- 错误处理：生产路径不用 C++ 异常——IPC 返回 `{"ok":false,"error":"..."}`；崩溃保护用 `/EHa` + `catch(...)` + CRASH_LOG
- JSON 手写 raw string literal + `escapeJsonString`（曾因 `juce::JSON` 转义 bug 弃用）
- Git：Conventional Commits（`feat(ipc): ...`），提交消息英文、文档中文；分支/PR/CI 流程见 WORKFLOW 节
- 线程铁律：`prepareToPlay/processBlock` 必须在测量线程；编辑器创建必须在消息线程（`callAsync`）；扫描/加载用**专用一次性 `std::thread`**（析构不 join，`release()` 放弃；**禁 `std::thread::detach`**）；UI 刷新用 AsyncUpdater

## ANTI-PATTERNS (THIS PROJECT)

- **空 catch / 吞异常**——违规点 `source/Main.cpp:659`（`catch(...){return 0;}`）与 `:674`（空 catch，ListBox 回调）；每个 catch 必须 CRASH_LOG（参照 `PluginManager.cpp:221,345`）
- **递归锁同一 mutex**（曾致 std::system_error 必现崩溃）——锁内只拷贝，锁外加载
- **`JUCE_TRY`/`JUCE_CATCH_EXCEPTION`**——会重抛，禁止（用 /EHa + catch(...)）
- **`getTotalLength()` 返回 -1**——无限长会触发 SweepRunner 静默 10s 兜底
- **改 `SweepRunner`**——冻结边界；测量目的感知放 MeasurementSession，为什么测放上层
- **每 block 刷 UI**——必须 AsyncUpdater ~50ms 节流
- **混用 THD/IMD 信号**（多音谐波峰交叠）、**过度设计**（"不做过度设计"为明示原则）
- **/WX 下的任何编译警告**——提交门禁

## NOTES

- 模块边界由根 CMakeLists.txt 单一清单强制（目录不自包含）——**例外**：`tools/` 自含 CMakeLists.txt（构建 VST3Scanner，`add_subdirectory(tools)` 无条件）；`VST3Scanner` 运行时死代码（扫描在进程内），但每次构建仍编译并复制到产物目录
- `JUCE_DISABLE_ASSERTIONS=1` 仅 MSVC 下 PluginLab target 全配置生效（JUCE 9 扫描器断言消息线程）；`unit_tests` 不带此定义
- vocal 素材 `samples/take01.wav`（48k/16bit/stereo/17.0s，**已入库**）
- 构建产物：`build/`（MSVC）与 `cmake-build-debug/`（CLion Ninja）并存
- 知识库分层：本文件（根）+ `source/AGENTS.md` + 各模块子文件 + `tests/AGENTS.md`
- **插件缓存**：`%APPDATA%/PluginLab/pluginlist.xml`（根 `version=1`；`loadCache/saveCache` 见 PluginManager；原子写 temp+replaceFileIn）；**黑名单随缓存往返**（`<BLACKLISTED>` 子元素）；**死马踏板** `%APPDATA%/PluginLab/deadMansPedal`（挂起/崩溃插件下次自动黑名单）
- 扫描/加载线程模型：专用一次性 `std::thread`（**析构不 join**，放弃用 `release()`）；worker 不触碰宿主成员（shared_ptr 状态 + callAsync alive-guard）；扫描看门狗（无进展 60s→黑名单+abandon，上限 3 次）；热扫增量靠 `cacheIsCurrent()`（内层 DLL mtime 基准）
