# Plugin Lab — 当前状态 (2026-08-01)

## 已验证通过 ✅

- **全量验收（真实 GUI 模拟测试，92 插件）**：
  - 91/92 插件覆盖测试，**90 成功（98.9%）**
  - 0 崩溃、0 无编辑器
  - 唯一失败：Pianoteq 9（黑名单拦截，见下）
- **独立插件窗口**：点击列表 → 弹出独立顶层窗口承载插件编辑器
- **窗口大小 = 插件大小**：客户区尺寸 == 编辑器尺寸（4 插件验证通过）
  - Auto-Tune Pro: 编辑器 1617x965 == 客户区 1617x965 ✅
  - AutoPitchK: 500x400 ✅ / Melodyne: 812x600 ✅ / PluginDoctor: 1000x750 ✅
  - （窗口总尺寸比编辑器大一个原生标题栏 ≈16x39，属正常）
- **窗口固定不可调整**（setResizable(false)）
- **单实例切换**：点新插件 → 旧窗口关、旧实例卸载 → 新窗口开，始终 ≤1 窗口
- **关窗即卸载**：X → 编辑器销毁 + 实例释放，主窗口不受影响
- **UI 全程响应**：扫描/加载后台线程，无卡死
- 构建通过：/W4 /WX 零错误

## 关键修复（真实测试发现）

1. **递归锁 bug（T1 前）：`loadPlugin()` 持有 listLock 又调 `getPluginDescription()` 内部再锁同一 mutex → std::system_error → 列表点击必失败**。修复：去掉外层锁。
2. **Pianoteq 9 崩溃（oracle 分析确认）**：插件在 createPluginInstance 内部调用 ExitProcess/TerminateProcess，绕过所有 try/catch + SEH + minidump，宿主进程无征兆退出（无 dmp、无 crash 事件，仅 RADAR_PRE_LEAK_64 副作用）。
   - **对策**：`PluginManager::loadPlugin` 加黑名单拦截（Pianoteq 7/8/9 匹配），返回 nullptr + 警告日志，宿主不再被杀
   - 长期方案：进程外托管（ChildProcessCoordinator，成本高，未实施）

## 构建 & 运行

```
build:  cmake --build build --config Release
exe:    build\PluginLab_artefacts\Release\Plugin Lab.exe
cmake:  D:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe
```

## 关键架构决策

1. **扫描/加载全部后台线程**（std::thread + MessageManager::callAsync）→ UI 永不阻塞
2. **编辑器创建必须在消息线程**（JUCE 硬断言）——createEditorSafe 移到 callAsync 块内
3. **线程安全**：std::mutex listLock 保护 KnownPluginList；先拷贝 desc、释放锁、再加载（**禁止递归锁同一 mutex**）
4. **独立窗口**：PluginEditorWindow（DocumentWindow 子类，原生标题栏 + setContentOwned(editor,true) + setResizable(false,false) → 窗口内容区 == 编辑器尺寸）
5. **生命周期**：窗口析构必须显式 clearContentComponent()（防止编辑器在插件实例之后析构的 UB）
6. **崩溃保护**：PluginManager.cpp /EHa + catch(...)；禁止 JUCE_TRY/JUCE_CATCH_EXCEPTION（会重抛）
7. **黑名单**：已知杀进程插件在进入 DLL 前拦截

## 历史崩溃根因（已修复）

- 0xe06d7363 = C++ 异常逃逸（JUCE callAsync/消息回调未捕获）
- 0xc0000409 = std::terminate（后台 std::thread 抛异常）
- 0xc0000005 = 访问越界（VST3 DLL 崩溃，/EHa 捕获）
- 递归锁 = std::system_error（点击列表必失败）
- Pianoteq = 插件自杀 ExitProcess（黑名单拦截）

## 调试模式（可用）

- **日志**：`%TEMP%\pluginlab_crashlog.txt`（Win32 写入+FlushFileBuffers，崩溃安全）
- **UDP 实时流**：127.0.0.1:43210
- **minidump**：`%TEMP%\pluginlab_crash.dmp`（崩溃时自动生成）
- **监控脚本**：`D:\Documents\PluginLab\monitor.ps1`
- **GUI 自动化验收**：`C:\Users\admin\AppData\Local\Temp\opencode\gui_test_full8.py`（UIA select + 日志解析 + 崩溃重启）

## 待办（下一步）

- T2 稳定加固（EditorCrashGuard /EHa TU、Generic 编辑器兜底、观察者指针清理）— 当前已足够稳定，可按需实施
- 恢复 IPC 管道控制（当前注释掉）
- 测量引擎接入 UI（PlotWidget 已写好未接线）
- signal/capture/analysis 模块已写好但未在 UI 中使用

## 目录结构

```
source/
├── Main.cpp              # 主窗口 + 后台线程扫描/加载 + 独立窗口管理
├── host/PluginManager    # VST3 扫描/加载（/EHa + 黑名单）
├── ui/PluginEditorWindow # 独立插件编辑器窗口（DocumentWindow 子类）
├── signal/               # 信号生成器 (SineSweep/MultiTone/ToneBurst/Impulse)
├── capture/              # 采集引擎 (CaptureBuffer/SweepRunner/MeasurementSession)
├── analysis/             # 分析引擎 (FreqResponse/Harmonic/CompressionCurve/Export)
├── ipc/                  # Named Pipe 控制 (PipeServer/CommandParser/Protocol)
├── ui/PlotWidget         # 绘图组件
└── utils/                # FftHelper/MathUtils/CrashLog
tools/VST3Scanner         # 独立扫描工具（当前未使用，主进程内扫描）
monitor.ps1               # 实时监控脚本
DESIGN.md                 # 设计文档
```
