r"""GUI 点击路径回归测试（issue #66）。

背景
----
PluginLab 的 GUI 按钮（Measure/Scan/Record/Stop TL/Play 等）是 `commandParser->handleCommand`
的薄包装（消息线程同步调用），与 IPC 路径汇聚同一 handler——命令逻辑已被 CommandParserTests
覆盖，但 **UI 面（按钮使能状态、重入守卫、状态标签、Stop 行为）零回归保护**（STATUS.md
2026-08-10 记录，issue #66）。本文件为该 UI 面补自动化回归。

设计
----
- **不抢鼠标/不抢焦点**：全程使用 pywinauto UIA 后端 + InvokePattern（`invoke()` /
  `select()`），绝不调用 `click()`（移动物理鼠标）或 `set_focus()`（抢键盘焦点）。
- **崩溃重启**：会话级 fixture 监控 app 进程；测试中途崩溃 → 失败并附 CrashLog 尾部诊断；
  测试之间崩溃 → 自动重启一次（继承旧 gui_test_full8.py 的 resilience 模式）。
- **CI 可跑**：应用无 AudioDeviceManager（纯内存测量），GitHub Actions windows-latest
  可直接启动。CI 无插件 → S3-S5 自动 skip（列表为空）；本地真机 → 全链路。
- **依赖**：pywinauto（非 stdlib——tools/ 其余套件保持 stdlib-only，本文件缺失时
  模块级 skip，不破坏 `python -m pytest tools/`）。

场景
----
S1  initial_state     启动 → 窗口出现；扫描完成后状态含 "plugins found"；无插件时
                       Record/Stop TL/Play **disabled**（issue #66 暴露的使能状态 bug）；
                       Measure 四按钮 + Scan VST3 enabled。
S2  no_plugin_guards  无插件点 Freq Response → "Load a plugin first"；点 Stop TL → "Not recording"。
S3  load_and_measure  选插件行 → "Loaded:"；Record/Play 变 enabled；Freq Response →
                       "Measuring..." → 完成后测量按钮恢复 enabled。
S4  record_stop_play  Record → "Recording timeline..."；Stop TL → "Timeline exported"
                       + pluginlab_timeline.json 落盘；Play → "Playback done" + *_play.wav 落盘。
S5  reentry_guard     测量中点 Record → "Busy - wait for the current job"（重入守卫）。
                      带重试：频响扫描 ~5s，若在 Record 点击前完成则重试。

用法
----
    $env:PLUGINLAB_EXE = "build\PluginLab_artefacts\Release\Plugin Lab.exe"
    $env:PLUGINLAB_GUI_PLUGIN = "FabFilter Pro-C 3"   # 可选，省略则自动挑选
    python -m pytest tools/gui_test.py -q

环境变量：`PLUGINLAB_EXE`（app 路径，缺省 build 树 Release）、`PLUGINLAB_GUI_PLUGIN`
（全链路测试的插件显示名，缺省自动挑选：Pro-Q 4 > 其余 FabFilter > 首个非跳过
插件；AI 修复/分析套件一律跳过）；脚本自身向 app 注入
`PLUGINLAB_GUI_MEASURE_DELAY_MS=3000`（生产侧测试 seam——拉伸 Measuring 窗口使
瞬态断言确定性成立，见 Main.cpp startMeasurement）。
"""

import glob
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time

import pytest

try:
    from pywinauto import Application
    HAVE_PYWINAUTO = True
except ImportError:
    HAVE_PYWINAUTO = False

if not HAVE_PYWINAUTO:
    pytest.skip(
        "pywinauto not installed — GUI test requires it (pip install pywinauto)",
        allow_module_level=True,
    )


# ---------------------------------------------------------------------------
# 路径解析（pytest 选项 / 环境变量 / 默认）
# ---------------------------------------------------------------------------

def _repo_root() -> str:
    return os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def _resolve_exe() -> str:
    from_env = os.environ.get("PLUGINLAB_EXE")
    if from_env:
        return from_env
    for candidate in ("build/PluginLab_artefacts/Release/Plugin Lab.exe",
                      "build/PluginLab_artefacts/Debug/Plugin Lab.exe",
                      "cmake-build-debug/PluginLab_artefacts/Debug/Plugin Lab.exe"):
        path = os.path.join(_repo_root(), candidate)
        if os.path.isfile(path):
            return path
    return os.path.join(_repo_root(), "build/PluginLab_artefacts/Release/Plugin Lab.exe")


EXE = _resolve_exe()
if not os.path.isfile(EXE):
    pytest.skip(f"Plugin Lab.exe not found at {EXE} — build the app first (issue #66 GUI test)",
                allow_module_level=True)

PLUGIN_OVERRIDE = os.environ.get("PLUGINLAB_GUI_PLUGIN")

# 自动选择规则（用户指示 2026-08-23）：AI 修复/分析套件不反映传统 EQ/comp 处理链，
# 不参与自动选择；首选 FabFilter 家族（Pro-Q 4 优先）。
#   - UADx 系列：processBlock 抛未知异常（issue #68）
#   - PolyMAX：乐器；magic.CURVE：编辑器消息重入致静默退出（STATUS 已知限制）
#   - RX/Ozone（iZotope）、Auto-*（Antares）、Melodyne（Celemony）：AI 修复/音高分析套件
_SKIP_PREFIXES = ("UADx", "PolyMAX", "RX ", "Ozone", "Auto-", "Melodyne", "magic.CURVE")
_SKIP_MANUFACTURERS = ("izotope", "antares", "celemony")


def _plugin_skipped(name: str, manufacturer: str) -> bool:
    mfr = manufacturer.lower()
    return (name.startswith(_SKIP_PREFIXES)
            or any(s in mfr for s in _SKIP_MANUFACTURERS))

# ---------------------------------------------------------------------------
# 会话级 fixture：启动 app + 崩溃重启 + 清理
# ---------------------------------------------------------------------------


class AppSession:
    """封装 Plugin Lab 进程 + 主窗口，提供 UIA 交互 helper。"""

    MAIN_WINDOW_TITLE = "Plugin Lab"

    def __init__(self, exe: str):
        self.exe = exe
        self.app_dir = tempfile.mkdtemp(prefix="pluginlab_gui_test_")
        self.proc = None
        self.app = None
        self.main = None
        self.scan_complete = False

    def launch(self, timeout: float = 60.0):
        # PLUGINLAB_GUI_MEASURE_DELAY_MS：测量离线完成（5s 扫频 <1s 墙钟），
        # "Measuring..." 瞬态窗口对 UIA 轮询太窄——生产侧测试 seam 拉伸该窗口
        # （guard 已武装 + 测量按钮已禁用）。3000ms 是给 UIA 慢树遍历留的余量：
        # 单次 descendants 枚举可达数秒，窗口必须宽于一次完整查询。
        env = dict(os.environ, PLUGINLAB_GUI_MEASURE_DELAY_MS="3000")
        self.proc = subprocess.Popen([self.exe], cwd=self.app_dir, env=env)
        self.app = Application(backend="uia").connect(process=self.proc.pid, timeout=timeout)
        self.main = self.app.window(title=self.MAIN_WINDOW_TITLE)
        self.main.wait("visible", timeout=timeout)
        return self

    def alive(self) -> bool:
        return self.proc is not None and self.proc.poll() is None

    def relaunch(self, timeout: float = 60.0):
        """测试之间崩溃后的自动重启（继承旧脚本的崩溃重启模式）。"""
        self.shutdown()
        self.launch(timeout=timeout)

    def wait_scan_done(self, timeout: float = 90.0):
        """等待初始扫描完成（状态含 'plugins found' 或 'Scan error'）。幂等——
        完成后置位，后续测试不再重复等待（状态标签随后会被交互覆盖）。"""
        if self.scan_complete:
            return
        ok = wait_for(
            lambda: ("plugins found" in self.status_text() or "Scan error" in self.status_text()),
            timeout=timeout, interval=0.5, desc="initial plugin scan done",
        )
        if not ok:
            pytest.fail(f"initial scan never finished; status={self.status_text()!r}")
        ensure_alive(self)
        self.scan_complete = True

    def ensure_loaded(self, plugin: str, timeout: float = 60.0):
        """等待状态标签出现 'Loaded: <plugin>'（加载完成信号）。"""
        ok = wait_for(lambda: plugin in self.status_text(), timeout=timeout,
                      desc=f"plugin {plugin!r} loaded")
        if not ok:
            pytest.fail(f"plugin {plugin!r} never loaded; status={self.status_text()!r}")
        ensure_alive(self)

    def shutdown(self):
        if not self.alive():
            return
        try:
            self.main.close()
        except Exception:
            pass
        try:
            self.proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            self.proc.wait(timeout=10)

    def crashlog_tail(self, n: int = 15) -> str:
        """%TEMP%\\pluginlab_crashlog.txt 尾部（崩溃诊断）。"""
        path = os.path.join(tempfile.gettempdir(), "pluginlab_crashlog.txt")
        if not os.path.isfile(path):
            return "(no crashlog)"
        try:
            with open(path, "r", encoding="utf-8", errors="replace") as f:
                lines = f.read().splitlines()
            return "\n".join(lines[-n:])
        except OSError as e:
            return f"(crashlog unreadable: {e})"

    # --- UIA helpers（全部 invoke-only，无鼠标）---

    def status_text(self) -> str:
        """读主窗口状态标签文本。JUCE Label 以 Text 控件暴露，Name = 标签内容本身
        （不是组件名 "Status"）；状态标签在树序中先于其他 Text（scanEstimateLabel 等），
        故取首个非空 Name 的 Text。读不到返回 ""。"""
        try:
            for el in self.main.descendants(control_type="Text"):
                name = el.window_text() or ""
                if name.strip():
                    return name.strip()
        except Exception:
            pass
        return ""

    def button(self, title: str):
        return self.main.child_window(title=title, control_type="Button")

    def invoke(self, title: str):
        """InvokePattern 触发按钮——不移动物理鼠标。"""
        btn = self.button(title)
        btn.wait("exists", timeout=10)
        btn.invoke()

    def button_enabled(self, title: str) -> bool:
        try:
            btn = self.button(title)
            return bool(btn.exists(timeout=0.5) and btn.is_enabled())
        except Exception:
            return False

    def plugins_from_cache(self):
        """从 app 的插件缓存 XML 读取按行序排列的 (插件名, 厂商) 列表（行序 =
        knownPlugins 序 = 缓存 XML 序；JUCE ListBox 行经 UIA 只暴露 "Row N" 无名字，
        名字只能来自缓存）。注意缓存序非字母序且会被 app 重扫描重写。缓存缺失/
        解析失败 → 空列表。"""
        cache = os.path.join(os.environ.get("APPDATA", ""), "PluginLab", "pluginlist.xml")
        if not os.path.isfile(cache):
            return []
        try:
            import xml.etree.ElementTree as ET
            root = ET.parse(cache).getroot()
            return [(el.attrib.get("name", ""), el.attrib.get("manufacturer", ""))
                    for el in root.findall("PLUGIN") if el.attrib.get("name")]
        except (ET.ParseError, OSError):
            return []

    def plugin_names_from_cache(self):
        return [name for name, _ in self.plugins_from_cache()]

    def select_plugin_row(self, index: int) -> bool:
        """SelectionItemPattern 选中第 index 行（0 基；UIA 行标题为 1 基 "Row N"）
        ——不移动鼠标。注意：JUCE ListBox 虚拟化，仅视口内约 40 行物化为 UIA
        元素，且 JUCE List 不实现 IScrollProvider 无法程序化滚动——深索引行
        不存在时返回 False，调用方应走 load_via_ipc 兜底。"""
        try:
            lst = self.main.child_window(control_type="List")
            target = f"Row {index + 1}"
            items = [it for it in lst.items() if it.window_text().startswith("Row ")]
            for it in items:
                if it.window_text() == target:
                    it.select()
                    return True
            return False
        except Exception:
            return False

    def load_via_ipc(self, name: str) -> None:
        """经 app 自身 IPC 管道发 loadPlugin（GUI 行选择被虚拟化挡住时的兜底）。

        GUI 与 IPC 双路径汇聚同一 handleCommand——加载后状态标签/按钮使能等
        GUI 断言仍全部经 UIA 观察，被测 GUI 面不受影响。协议：JSON 行，
        {"cmd":"loadPlugin","path":"<显示名>"}（名字大小写不敏感寻址）。"""
        import json as _json
        import threading
        payload = (_json.dumps({"cmd": "loadPlugin", "path": name}) + "\n").encode("utf-8")
        pipe = open(r"\\.\pipe\PluginLab", "r+b", buffering=0)
        # 看门狗：app 卡死时管道读会永久阻塞——定时关句柄强制解除（15s 远超
        # loadPlugin 的正常响应时间），把挂起转化为显式失败。
        watchdog = threading.Timer(15.0, pipe.close)
        watchdog.daemon = True
        try:
            watchdog.start()
            pipe.write(payload)
            response = b""
            while not response.endswith(b"\n"):
                chunk = pipe.read(4096)
                if not chunk:
                    break
                response += chunk
        except (OSError, ValueError) as e:
            raise RuntimeError(f"IPC loadPlugin({name!r}) pipe error "
                               f"(watchdog fired or app wedged): {e}") from e
        finally:
            watchdog.cancel()
            try:
                pipe.close()
            except OSError:
                pass
        if b'"ok":true' not in response:
            raise RuntimeError(f"IPC loadPlugin({name!r}) failed: {response!r}")

    def load_plugin(self, name: str, index: int) -> None:
        """加载插件：UIA 行选择优先（纯点击路径）；目标行未物化 → IPC 兜底。"""
        if self.select_plugin_row(index):
            return
        self.load_via_ipc(name)


@pytest.fixture(scope="session")
def app():
    session = AppSession(EXE)
    session.launch()
    try:
        yield session
    finally:
        session.shutdown()
        shutil.rmtree(session.app_dir, ignore_errors=True)


# ---------------------------------------------------------------------------
# 公共等待 helper
# ---------------------------------------------------------------------------

def wait_for(predicate, timeout: float, interval: float = 0.3, desc: str = "condition"):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if predicate():
            return True
        time.sleep(interval)
    return False


def wait_status(app, fragment: str, timeout: float = 30.0) -> str:
    """轮询状态标签直到包含 fragment；超时未出现 → fail（附最终文本，便于诊断）。"""
    ok = wait_for(lambda: fragment in app.status_text(), timeout=timeout,
                  desc=f"status contains {fragment!r}")
    if not ok:
        pytest.fail(f"status never contained {fragment!r}; final={app.status_text()!r}")
    return app.status_text()


def ensure_alive(app):
    if not app.alive():
        pytest.fail(f"Plugin Lab crashed mid-test\n--- crashlog tail ---\n{app.crashlog_tail()}")


def skip_unless_cache(app):
    """S3-S5 共用守卫：插件缓存不可读/空 → skip（全链路测试需要真 VST3）。"""
    if not app.plugin_names_from_cache():
        pytest.skip("plugin cache unreadable/empty — full-chain GUI tests need a real VST3")


def wait_measurement_done(app, timeout: float = 60.0):
    """等待测量结束（状态离开 'Measuring...' 且测量按钮恢复 enabled）。"""
    ok = wait_for(
        lambda: "Measuring..." not in app.status_text()
                and app.button_enabled("Freq Response"),
        timeout=timeout, interval=0.5, desc="measurement complete",
    )
    if not ok:
        pytest.fail(f"measurement never finished; status={app.status_text()!r}")
    ensure_alive(app)


def observe_measuring_disabled(app, timeout: float = 30.0) -> bool:
    """单次轮询内组合观察：状态处于 'Measuring...' **且** Freq Response 已禁用。

    UIA 树遍历单次可达数秒——把两个事实放进同一次轮询原子观察，避免
    「先等状态再读按钮」两次慢查询之间测量已完成的时间差竞态。
    """
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if "Measuring..." in app.status_text() and not app.button_enabled("Freq Response"):
            return True
        time.sleep(0.2)
    return False


# ---------------------------------------------------------------------------
# S1 初始状态（CI + 本地）
# ---------------------------------------------------------------------------

def test_initial_state(app):
    """启动 → 窗口出现；扫描完成；无插件时 Record/Stop TL/Play disabled，测量按钮 enabled。"""
    ensure_alive(app)

    assert app.main.window_text().strip() == AppSession.MAIN_WINDOW_TITLE, \
        f"main window title={app.main.window_text()!r}"

    app.wait_scan_done()
    status = app.status_text()
    assert re.search(r"\d+ plugins found", status), f"status={status!r}"

    # issue #66 核心断言：无插件时时间线按钮必须 disabled（setTimelineButtons 生命周期 bug）
    assert not app.button_enabled("Record"), "Record must be disabled without a plugin"
    assert not app.button_enabled("Stop TL"), "Stop TL must be disabled without a plugin"
    assert not app.button_enabled("Play"), "Play must be disabled without a plugin"

    # 测量按钮 + 扫描按钮在无插件时可用
    for btn in ("Freq Response", "Harmonic", "Compression", "GR", "Scan VST3"):
        assert app.button_enabled(btn), f"{btn} must be enabled"
    ensure_alive(app)


# ---------------------------------------------------------------------------
# S2 无插件守卫（CI + 本地）
# ---------------------------------------------------------------------------

def test_no_plugin_guards(app):
    """无插件时点击测量/时间线按钮 → 状态标签给出来源提示。"""
    ensure_alive(app)

    app.wait_scan_done()

    app.invoke("Freq Response")
    assert wait_status(app, "Load a plugin first", timeout=5.0), \
        f"status={app.status_text()!r}"
    ensure_alive(app)


# ---------------------------------------------------------------------------
# S3 加载 + 测量（本地真插件）
# ---------------------------------------------------------------------------

def _pick_plugin(app):
    """选插件行：返回 (插件名, 行号)。优先级：PLUGINLAB_GUI_PLUGIN 指定 >
    Pro-Q 4（STATUS 真机验收惯例 + #28 反推闭环目标插件）> 其余 FabFilter
    （传统 EQ/dynamics/limiting 族）> 首个非跳过插件。AI 修复/分析套件与
    已知不稳定插件一律跳过（见 _SKIP_* 常量）；缓存不可读/空 → skip。"""
    entries = app.plugins_from_cache()
    if not entries:
        pytest.skip("plugin cache unreadable/empty — full-chain GUI tests need a real VST3")

    names = [name for name, _ in entries]
    if PLUGIN_OVERRIDE:
        if PLUGIN_OVERRIDE not in names:
            pytest.fail(f"plugin {PLUGIN_OVERRIDE!r} not in cache list {names[:10]}...")
        return PLUGIN_OVERRIDE, names.index(PLUGIN_OVERRIDE)

    for i, (name, _) in enumerate(entries):
        if name == "Pro-Q 4":
            return name, i
    for i, (name, manufacturer) in enumerate(entries):
        if manufacturer.lower() == "fabfilter" and not _plugin_skipped(name, manufacturer):
            return name, i
    for i, (name, manufacturer) in enumerate(entries):
        if not _plugin_skipped(name, manufacturer):
            return name, i
    pytest.skip("all cached plugins are skipped families — set PLUGINLAB_GUI_PLUGIN to override")


def test_load_and_measure(app):
    """选中插件行 → 'Loaded:'；Record/Play 变 enabled；Freq Response 跑通并恢复按钮。"""
    ensure_alive(app)
    app.wait_scan_done()

    plugin, row = _pick_plugin(app)
    # UIA 行选择优先；FabFilter 等深索引行常被 ListBox 虚拟化挡住 → IPC 兜底。
    app.load_plugin(plugin, row)
    app.ensure_loaded(plugin)
    ensure_alive(app)

    # 加载后时间线按钮应 enabled（issue #66 生命周期接线）
    assert app.button_enabled("Record"), "Record must be enabled after load"
    assert app.button_enabled("Play"), "Play must be enabled after load"

    app.invoke("Freq Response")
    # 瞬态窗口原子观察（observe_measuring_disabled）：Measuring 期间测量按钮必禁用。
    assert observe_measuring_disabled(app, timeout=30.0), \
        f"buttons never observed disabled during 'Measuring...' (status={app.status_text()!r})"

    wait_measurement_done(app, timeout=60.0)
    for btn in ("Freq Response", "Harmonic", "Compression", "GR"):
        assert app.button_enabled(btn), f"{btn} must be re-enabled after measurement"
    ensure_alive(app)


# ---------------------------------------------------------------------------
# S4 记录 → 停止 → 回放（本地真插件）
# ---------------------------------------------------------------------------

def test_record_stop_play(app):
    """Record → Stop TL（导出 pluginlab_timeline.json）→ Play（回放 + wav）。"""
    ensure_alive(app)
    app.wait_scan_done()
    skip_unless_cache(app)

    # 自足加载：不依赖 test_load_and_measure 先行（单跑 -k 亦可复现全链）。
    plugin, row = _pick_plugin(app)
    app.load_plugin(plugin, row)
    app.ensure_loaded(plugin)

    app.invoke("Record")
    assert wait_status(app, "Recording timeline", timeout=5.0), \
        f"status={app.status_text()!r}"

    # 录制中 Stop TL 应 enabled、Record 应 disabled
    assert app.button_enabled("Stop TL"), "Stop TL must be enabled while recording"
    assert not app.button_enabled("Record"), "Record must be disabled while recording"

    time.sleep(1.0)  # 录一小段（事件可能为 0——空时间线回放仍有效，见 CommandParser playTimeline）

    app.invoke("Stop TL")
    status = wait_status(app, "Timeline exported", timeout=5.0)
    assert "Timeline exported" in status, f"status={status!r}"

    timeline_file = os.path.join(app.app_dir, "pluginlab_timeline.json")
    assert os.path.isfile(timeline_file), f"timeline not exported to {timeline_file}"

    app.invoke("Play")
    play_status = wait_status(app, "Playback done", timeout=60.0)
    assert "Playback done" in play_status, f"play failed: status={app.status_text()!r}"

    wavs = glob.glob(os.path.join(app.app_dir, "*_play.wav"))
    assert wavs, "no playback wav exported in app cwd"
    ensure_alive(app)


# ---------------------------------------------------------------------------
# S5 重入守卫（本地真插件）
# ---------------------------------------------------------------------------

def test_reentry_guard(app):
    """测量中点 Record → 'Busy - wait for the current job'（重入守卫）。"""
    ensure_alive(app)
    app.wait_scan_done()
    skip_unless_cache(app)

    # 自足加载：不依赖 S3/S4 先行（单跑 -k 亦可复现）。
    plugin, row = _pick_plugin(app)
    app.load_plugin(plugin, row)
    app.ensure_loaded(plugin)

    # 预解析两个按钮引用再背靠背裸 invoke：UIA 树遍历（秒级）提前到空闲期做，
    # 两次 InvokePattern 触发变成毫秒级连发——Record 落在 seam 窗口头部，
    # "Busy" 文本可见数秒而非闪现，慢轮询也能采样到。
    for attempt in range(1, 4):
        ensure_alive(app)
        freq_btn = app.button("Freq Response")
        rec_btn = app.button("Record")
        try:
            freq_btn.wait("visible ready", timeout=10)
            rec_btn.wait("visible ready", timeout=10)
        except Exception as e:
            pytest.fail(f"buttons not resolvable before fire: {e}")
        freq_btn.invoke()
        rec_btn.invoke()

        # 捕获即断言：用检测到 Busy 的同一次读取做断言——二次读取会经历秒级
        # UIA 遍历，期间状态可能已被后续事件覆盖（实测 Busy→Measuring 竞态）。
        seen_busy = None
        deadline = time.monotonic() + 45.0
        while time.monotonic() < deadline:
            st = app.status_text()
            if "Busy" in st or "Recording timeline" in st:
                seen_busy = ("Busy" in st)
                break
            time.sleep(0.2)

        if seen_busy:
            assert "Busy - wait for the current job" in st, f"status={st!r}"
            wait_measurement_done(app, timeout=60.0)
            return

        # Record 落窗太晚、守卫没拦住（或 invoke 迟到）——若真开了录制则停掉再重试
        if "Recording timeline" in st:
            app.invoke("Stop TL")
            wait_status(app, "Timeline exported", timeout=10.0)
        else:
            wait_measurement_done(app, timeout=60.0)

    pytest.fail("re-entry guard never observed: Record during measurement was not rejected "
                f"(final status={app.status_text()!r})")


if __name__ == "__main__":
    sys.exit(pytest.main([__file__, "-v"] + sys.argv[1:]))