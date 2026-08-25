"""probe_plugin.py — 插件参数面探针（T2 #99）。

独立实例启动 Plugin Lab → loadPlugin → getParams → 分析参数面：
- 列出全部参数（index / param_id / name / value）
- **假面检测**：host 不暴露真实参数面（elysia alpha / Pro-MB 模式）时
  给出可疑判定——参数名泛化、param_id 缺失/重复
- 输出可直接粘贴为 tools/configs/*.json 的 setup/scan 结构

用法:
    python tools/probe_plugin.py "Gem Comp76"
    python tools/probe_plugin.py "Ozone 12 Equalizer" --json

退出码: 0 = 正常; 2 = 插件加载失败/不可达; 3 = 判定为假面（可疑参数面）
"""
from __future__ import annotations

import argparse
import json
import subprocess
import sys
import time
from pathlib import Path
from typing import Any, List

REPO_ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO_ROOT / "tools"))

import pipe_client as pc  # noqa: E402

APP_EXE = REPO_ROOT / "build" / "PluginLab_artefacts" / "Release" / "Plugin Lab.exe"

# ── 假面检测参数 ──────────────────────────────────────────────────────────
# host 不暴露真实参数面时（#54 实测：elysia alpha / Pro-MB），getParams 返回
# 一组固定参数的副本：泛化名 + 无 param_id 或全 0 顺序 id + 数值重复。
GENERIC_NAME_HINTS = ("parameter ", "param ", "control ", "knob ", "slider ")
GENERIC_NAME_MIN_MATCH = 3          # 至少 N 个参数名命中泛化特征
NO_ID_MIN_FRACTION = 0.8            # 无有效 param_id 的参数占比阈值
DUPLICATE_VALUE_MAX_FRACTION = 0.8  # 数值重复的参数占比阈值


def request(pc_mod, handle: int, payload: dict, timeout_sec: float = 30.0) -> dict:
    """Send one JSON-line request, read one response line, parse it."""
    pc_mod.send_line(handle, json.dumps(payload, ensure_ascii=False))
    raw = pc_mod.read_line(handle, timeout_sec=timeout_sec)
    try:
        return json.loads(raw)
    except json.JSONDecodeError as exc:
        raise RuntimeError(f"invalid JSON response to {payload.get('cmd')}: "
                           f"{raw[:200]!r}") from exc


def _is_generic_name(name: str) -> bool:
    """True when a param name looks host-generated (no vendor meaning)."""
    low = name.lower().strip()
    return any(h in low for h in GENERIC_NAME_HINTS)


def _has_valid_param_id(param: dict) -> bool:
    """A param has a usable id when param_id is non-empty and not a bare index.

    Hosted params expose a stable vendor id (hash or 0..N index that maps to
    the plugin's own parameter list). A *fake* surface (elysia/Pro-MB) either
    omits param_id entirely or repeats a tiny fixed set.
    """
    pid = param.get("param_id")
    if pid is None or str(pid) == "":
        return False
    return True


def detect_fake_surface(params: List[dict]) -> dict:
    """Analyse a getParams list for the 'host does not expose real params' tell.

    Returns a report dict:
        {fake: bool, reasons: [str], generic_names: int, no_id: int,
         duplicate_values: int, total: int}

    Fake when ANY of:
      - >= GENERIC_NAME_MIN_MATCH params have generic names AND lack a
        usable param_id (generic names alone are not a tell — real plugins
        may call a param "Param 1"; the fake surface combines generic
        names with missing/repeated ids)
      - >= NO_ID_MIN_FRACTION params lack a usable param_id
      - >= DUPLICATE_VALUE_MAX_FRACTION params share the same value
        (a real parameter surface rarely has >80% identical values at rest)
    """
    total = len(params)
    if total == 0:
        return {"fake": True, "reasons": ["no parameters returned"],
                "generic_names": 0, "no_id": 0,
                "duplicate_values": 0, "total": 0}

    generic_without_id = sum(
        1 for p in params
        if _is_generic_name(str(p.get("name", ""))) and not _has_valid_param_id(p))
    no_id = sum(1 for p in params if not _has_valid_param_id(p))
    values = [p.get("value") for p in params if p.get("value") is not None]
    value_counts: dict = {}
    for v in values:
        value_counts[v] = value_counts.get(v, 0) + 1
    duplicate_values = sum(c - 1 for c in value_counts.values() if c > 1)

    reasons: List[str] = []
    if generic_without_id >= GENERIC_NAME_MIN_MATCH:
        reasons.append(f"{generic_without_id} generic parameter names "
                       f"without a usable param_id")
    if no_id / total >= NO_ID_MIN_FRACTION:
        reasons.append(f"{no_id}/{total} params lack a usable param_id")
    if total > 0 and duplicate_values / total >= DUPLICATE_VALUE_MAX_FRACTION:
        reasons.append(f"{duplicate_values}/{total} params share identical values")

    return {"fake": bool(reasons), "reasons": reasons,
            "generic_names": generic_without_id, "no_id": no_id,
            "duplicate_values": duplicate_values, "total": total}


def param_id_kind(params: List[dict]) -> str:
    """Classify the param_id scheme: 'index', 'hash', 'mixed', or 'none'."""
    ids = [str(p.get("param_id", "")).strip() for p in params]
    non_empty = [i for i in ids if i]
    if not non_empty:
        return "none"
    numeric = all(i.isdigit() for i in non_empty)
    sequential = (len(non_empty) > 1
                  and numeric
                  and [int(i) for i in non_empty] == list(range(len(non_empty))))
    if sequential:
        return "index"
    if numeric:
        return "hash"   # large ids (e.g. Gem hash) are not sequential
    return "mixed"


def build_config_snippet(params: List[dict]) -> str:
    """Suggest a setup/scan config skeleton from the probed parameter list.

    Picks the first few non-bypass, non-power params as setup candidates and
    prints the JSON shape a user fills in. Pure string helper (no I/O).
    """
    candidates = []
    skip_hints = ("bypass", "power", "enable", "on/off", "solo")
    for p in params:
        name = str(p.get("name", ""))
        if any(h in name.lower() for h in skip_hints):
            continue
        candidates.append({"name": name, "param_id": str(p.get("param_id", "")),
                           "value": p.get("value")})
        if len(candidates) >= 5:
            break

    snippet = {
        "setup": [{"name": c["name"], "value": 0.5} for c in candidates[:2]],
        "scan": {"param_id": (candidates[0]["param_id"] if candidates else ""),
                 "values": [0.3, 0.5, 0.7], "type": "harmonic"},
    }
    return json.dumps(snippet, ensure_ascii=False, indent=2)


def format_report(params: List[dict], fake_report: dict, kind: str) -> str:
    """Render the human-readable probe report (no I/O, testable)."""
    lines = [f"TOTAL PARAMS: {len(params)}", "=" * 80]
    for p in params:
        idx = p.get("index", "?")
        pname = p.get("name", "?")
        val = p.get("value", "?")
        pid = p.get("param_id", "")
        flag = "  <-- no param_id" if not pid else ""
        lines.append(f"[{idx:4d}] id={str(pid):<24s} value={val:<10.4f} "
                     f"name={pname}{flag}")
    lines.append("=" * 80)
    lines.append(f"param_id scheme: {kind}")
    if fake_report["fake"]:
        lines.append("FAKE SURFACE DETECTED:")
        for r in fake_report["reasons"]:
            lines.append(f"  - {r}")
        lines.append("-> host likely does not expose real params; skip collection")
    else:
        lines.append("parameter surface looks real (usable for collection)")
    return "\n".join(lines)


def main(argv: List[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Probe a plugin's parameter surface via the Plugin Lab pipe")
    parser.add_argument("plugin", help="plugin name as shown in the cache")
    parser.add_argument("--json", action="store_true",
                        help="emit a machine-readable JSON report instead of text")
    args = parser.parse_args(argv)

    proc = subprocess.Popen([str(APP_EXE)], cwd=str(REPO_ROOT))
    time.sleep(1.0)
    try:
        handle = pc.connect(retries=50, retry_delay_sec=0.5)
        # wait for the plugin scan to finish
        deadline = time.monotonic() + 300.0
        while True:
            resp = request(pc, handle, {"cmd": "getScanStatus"}, timeout_sec=30.0)
            if resp.get("done"):
                break
            if time.monotonic() >= deadline:
                print("scan timeout", file=sys.stderr)
                return 2
            time.sleep(2.0)

        resp = request(pc, handle, {"cmd": "loadPlugin", "path": args.plugin},
                       timeout_sec=60.0)
        if not resp.get("ok"):
            print(f"loadPlugin FAILED: {resp}", file=sys.stderr)
            return 2

        # getParams is async on the message thread; poll until ok
        deadline = time.monotonic() + 60.0
        params: List[dict] = []
        while True:
            pr = request(pc, handle, {"cmd": "getParams"}, timeout_sec=30.0)
            if pr.get("ok"):
                params = pr.get("params", [])
                break
            if time.monotonic() >= deadline:
                print(f"getParams timeout: {pr}", file=sys.stderr)
                return 2
            time.sleep(0.5)

        fake = detect_fake_surface(params)
        kind = param_id_kind(params)

        if args.json:
            report = {
                "plugin": args.plugin,
                "params": params,
                "param_id_scheme": kind,
                "fake_surface": fake,
                "config_snippet": build_config_snippet(params),
            }
            print(json.dumps(report, ensure_ascii=False, indent=2))
        else:
            print(format_report(params, fake, kind))
            print()
            print("config snippet (fill in values):")
            print(build_config_snippet(params))

        return 3 if fake["fake"] else 0
    finally:
        try:
            pc.close(handle)  # noqa: F821 - set in try
        except Exception:
            pass
        proc.terminate()
        try:
            proc.wait(timeout=10)
        except Exception:
            proc.kill()


if __name__ == "__main__":
    sys.exit(main())