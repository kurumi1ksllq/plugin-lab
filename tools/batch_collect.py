"""
batch_collect.py — CLI batch driver for the PluginLab measurement pipeline.

Orchestrates the full batch flow against a running (or freshly launched)
Plugin Lab instance over the named pipe (see tools/pipe_client.py):

    wait for plugin scan -> loadPlugin -> dataset battery ->
    reverse_derive report (tools/reverse_derive.py) -> batch summary

Stdlib only — pipe_client is the repo's single pywin32-dependent module and
is imported lazily inside the functions that need it, so --dry-run works even
when pywin32 is broken.

Usage:
    python tools/batch_collect.py --plugin "NAME" [--out DIR] [--types LIST]
        [--config FILE] [--launch] [--quit]
    python tools/batch_collect.py --all [--out DIR] [--types LIST]
        [--config FILE] [--limit N] [--launch] [--quit]
    python tools/batch_collect.py --dry-run [--all|--plugin NAME] [--config FILE]

Exit codes: 0 = all plugins ok (or dry-run validated), 1 = run/validation
failure, 2 = pipe_client/pywin32 unavailable, 130 = interrupted (partial
summary still written).

Config schema (validated in --dry-run; unknown keys anywhere -> error):
    {"plugins": {
        "<PLUGIN name>": {
            "scan": {"param_id": "...", "values": [0.0, 1.0], "type": "frequency_response"},
            "compression_family": {"levels_db": [-12, 0], "speeds": [0.5, 1, 2]},
            "expected": {"freq": 1000, "gain": 6, "q": 1}
        }}}
Plugin keys match the cache PLUGIN name case-insensitively; "expected" keys
are a subset of freq/gain/q/threshold/ratio/attack_ms/release_ms.
"""
import argparse
import csv
import json
import os
import re
import subprocess
import sys
import time
import types
import xml.etree.ElementTree as ET
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path

# ---------------------------------------------------------------------------
# Constants
# ---------------------------------------------------------------------------

DEFAULT_TYPES = ["frequency_response", "harmonic", "compression", "gr_timeline"]

REPO_ROOT = Path(__file__).resolve().parent.parent
APP_EXE = REPO_ROOT / "build" / "PluginLab_artefacts" / "Release" / "Plugin Lab.exe"
PIPE_NAME = r"\\.\pipe\PluginLab"

CONNECT_TIMEOUT_SEC = 30.0
SCAN_TIMEOUT_SEC = 300.0
LOAD_TIMEOUT_SEC = 60.0
DATASET_TIMEOUT_SEC = 600.0
RD_TIMEOUT_SEC = 60.0

PLUGIN_ENTRY_KEYS = {"scan", "compression_family", "expected"}
SCAN_KEYS = {"param_id", "values", "type"}
SCAN_TYPES = {"frequency_response", "harmonic", "compression"}   # gr_timeline rejected by app
CF_KEYS = {"levels_db", "speeds"}
EXPECTED_KEYS = {"freq", "gain", "q", "threshold", "ratio", "attack_ms", "release_ms"}
EXPECTED_FLAGS = {
    "freq": "--expected-freq",
    "gain": "--expected-gain",
    "q": "--expected-q",
    "threshold": "--expected-threshold",
    "ratio": "--expected-ratio",
    "attack_ms": "--expected-attack-ms",
    "release_ms": "--expected-release-ms",
}


# ---------------------------------------------------------------------------
# Small data carriers
# ---------------------------------------------------------------------------


@dataclass(frozen=True)
class PluginInfo:
    name: str
    file: str


@dataclass(frozen=True)
class PlanEntry:
    name: str
    file: str
    slug: str
    cfg: dict        # resolved per-plugin config block (may be {})


# ---------------------------------------------------------------------------
# Path helpers
# ---------------------------------------------------------------------------


def normalize_path(path: str) -> str:
    """Normalize a path for blacklist comparison: forward slashes, lowercase."""
    return path.replace("\\", "/").lower()


def make_slug(name: str) -> str:
    """Lowercase slug: runs of non-alphanumerics become a single hyphen."""
    return re.sub(r"[^a-z0-9]+", "-", name.lower()).strip("-")


def cache_path() -> Path:
    """%APPDATA%/PluginLab/pluginlist.xml (falls back to the home directory)."""
    return Path(os.environ.get("APPDATA", Path.home())) / "PluginLab" / "pluginlist.xml"


def check_out_dir_creatable(out_dir: Path) -> tuple[bool, str]:
    """Best-effort check that --out can be created, WITHOUT creating it."""
    if out_dir.exists():
        if not out_dir.is_dir():
            return False, "exists but is not a directory"
        return os.access(out_dir, os.W_OK), "directory is not writable"
    parent = out_dir.parent
    if not parent.exists():
        return False, f"parent directory does not exist: {parent}"
    return os.access(parent, os.W_OK), "parent directory is not writable"


# ---------------------------------------------------------------------------
# Plugin cache (pluginlist.xml)
# ---------------------------------------------------------------------------


def parse_plugin_cache(cache: Path) -> tuple[list[PluginInfo], set[str]]:
    """Parse pluginlist.xml into (plugins, blacklist).

    <PLUGIN name=... file=.../> children of the root plus <BLACKLISTED
    id=<path>/> entries; blacklist ids are compared against plugin file
    paths via normalize_path on both sides (case-insensitive, tolerant of
    raw vs normalized separators).
    """
    try:
        tree = ET.parse(cache)
    except (OSError, ET.ParseError) as exc:
        print(f"ERROR: cannot parse plugin cache {cache}: {exc}")
        sys.exit(1)
    plugins: list[PluginInfo] = []
    blacklist: set[str] = set()
    for child in tree.getroot():
        if child.tag == "PLUGIN":
            name = child.get("name", "")
            file = child.get("file", "")
            if name and file:
                plugins.append(PluginInfo(name=name, file=file))
        elif child.tag == "BLACKLISTED":
            entry = child.get("id", "")
            if entry:
                blacklist.add(normalize_path(entry))
    return plugins, blacklist


# ---------------------------------------------------------------------------
# Per-plugin config (--config FILE)
# ---------------------------------------------------------------------------


def validate_config(data: object) -> dict:
    """Validate the config document; returns the plugins dict.

    Raises ValueError naming the offending key — any unknown key anywhere
    in the document is an error.
    """
    if not isinstance(data, dict):
        raise ValueError("config must be a JSON object")
    unknown = set(data) - {"plugins"}
    if unknown:
        raise ValueError(f"unknown top-level key(s): {sorted(unknown)}")
    plugins = data.get("plugins") or {}
    if not isinstance(plugins, dict):
        raise ValueError("'plugins' must be an object")
    for name, entry in plugins.items():
        if not isinstance(entry, dict):
            raise ValueError(f"plugin {name!r}: entry must be an object")
        unknown = set(entry) - PLUGIN_ENTRY_KEYS
        if unknown:
            raise ValueError(f"plugin {name!r}: unknown key(s): {sorted(unknown)}")
        _validate_scan(name, entry.get("scan"))
        _validate_compression_family(name, entry.get("compression_family"))
        _validate_expected(name, entry.get("expected"))
    return plugins


def _is_number(value: object) -> bool:
    """True for int/float excluding bool (bool is an int subclass)."""
    return isinstance(value, (int, float)) and not isinstance(value, bool)


def _validate_scan(name: str, scan: object) -> None:
    if scan is None:
        return
    if not isinstance(scan, dict):
        raise ValueError(f"plugin {name!r}: 'scan' must be an object")
    unknown = set(scan) - SCAN_KEYS
    if unknown:
        raise ValueError(f"plugin {name!r}: unknown scan key(s): {sorted(unknown)}")
    param_id = scan.get("param_id")
    if not isinstance(param_id, str) or not param_id:
        raise ValueError(f"plugin {name!r}: scan.param_id must be a non-empty string")
    values = scan.get("values")
    if not isinstance(values, list) or not values or not all(_is_number(v) for v in values):
        raise ValueError(f"plugin {name!r}: scan.values must be a non-empty list of numbers")
    scan_type = scan.get("type")
    if scan_type is not None and scan_type not in SCAN_TYPES:
        raise ValueError(f"plugin {name!r}: scan.type must be one of "
                         f"{sorted(SCAN_TYPES)} (gr_timeline is rejected by the app)")


def _validate_compression_family(name: str, cf: object) -> None:
    if cf is None:
        return
    if not isinstance(cf, dict):
        raise ValueError(f"plugin {name!r}: 'compression_family' must be an object")
    unknown = set(cf) - CF_KEYS
    if unknown:
        raise ValueError(f"plugin {name!r}: unknown compression_family key(s): "
                         f"{sorted(unknown)}")
    for key in ("levels_db", "speeds"):
        value = cf.get(key)
        if value is not None and (not isinstance(value, list) or not value
                                  or not all(_is_number(v) for v in value)):
            raise ValueError(f"plugin {name!r}: compression_family.{key} "
                             "must be a non-empty list of numbers")


def _validate_expected(name: str, expected: object) -> None:
    if expected is None:
        return
    if not isinstance(expected, dict):
        raise ValueError(f"plugin {name!r}: 'expected' must be an object")
    unknown = set(expected) - EXPECTED_KEYS
    if unknown:
        raise ValueError(f"plugin {name!r}: unknown expected key(s): {sorted(unknown)}")
    for key, value in expected.items():
        if not _is_number(value):
            raise ValueError(f"plugin {name!r}: expected.{key} must be a number")


def load_config(config_path: Path | None) -> dict:
    """Load + validate the --config file; returns the plugins dict (may be {})."""
    if config_path is None:
        return {}
    try:
        with open(config_path, "r", encoding="utf-8") as f:
            data = json.load(f)
    except (OSError, json.JSONDecodeError) as exc:
        print(f"ERROR: cannot read config {config_path}: {exc}")
        sys.exit(1)
    try:
        return validate_config(data)
    except ValueError as exc:
        print(f"ERROR: invalid config {config_path}: {exc}")
        sys.exit(1)


def plugin_config(plugins_cfg: dict, name: str) -> dict:
    """Case-insensitive config lookup for a plugin name (returns {} when absent)."""
    for key, value in plugins_cfg.items():
        if key.lower() == name.lower():
            return value
    return {}


def parse_types(text: str) -> list[str]:
    """Split a --types list; raises ValueError for unknown/empty types."""
    types = [t.strip() for t in text.split(",") if t.strip()]
    unknown = [t for t in types if t not in DEFAULT_TYPES]
    if not types or unknown:
        raise ValueError(f"types must be a comma-separated subset of {DEFAULT_TYPES}")
    return types


# ---------------------------------------------------------------------------
# Named-pipe helpers (pipe_client imported lazily)
# ---------------------------------------------------------------------------


def load_pipe_client() -> types.ModuleType:
    """Import pipe_client lazily; exit 2 when it cannot be imported."""
    try:
        import pipe_client
    except ImportError as exc:
        print(f"ERROR: cannot import pipe_client: {exc}")
        sys.exit(2)
    return pipe_client


def request(pc: types.ModuleType, handle: int, payload: dict,
            timeout_sec: float) -> dict:
    """Send one JSON-line request, read one response line, parse it."""
    pc.send_line(handle, json.dumps(payload, ensure_ascii=False))
    raw = pc.read_line(handle, timeout_sec=timeout_sec)
    try:
        resp = json.loads(raw)
    except json.JSONDecodeError as exc:
        raise RuntimeError(f"invalid JSON response to {payload.get('cmd')}: "
                           f"{raw[:200]!r}") from exc
    return resp


def connect_with_deadline(pc: types.ModuleType, timeout_sec: float) -> int:
    """Poll pipe_client.connect until success or ``timeout_sec`` elapses.

    Raises TimeoutError (pipe never available) or RuntimeError (pywin32
    missing — the caller maps that to exit 2).
    """
    deadline = time.monotonic() + timeout_sec
    while True:
        try:
            return pc.connect(retries=2, retry_delay_sec=0.5)
        except TimeoutError:
            if time.monotonic() >= deadline:
                raise TimeoutError(f"cannot connect to {PIPE_NAME} within "
                                   f"{timeout_sec:.0f}s — is the app running?") from None
            time.sleep(0.5)


def wait_scan_done(pc: types.ModuleType, handle: int,
                   timeout_sec: float = SCAN_TIMEOUT_SEC) -> None:
    """Poll getScanStatus every 2s until done==true; abort past the deadline."""
    deadline = time.monotonic() + timeout_sec
    printed_progress = False
    while True:
        status = request(pc, handle, {"cmd": "getScanStatus"}, timeout_sec=30.0)
        if not status.get("ok"):
            raise RuntimeError(f"getScanStatus failed: {status.get('error') or 'unknown'}")
        if status.get("done"):
            if printed_progress:
                print()
            print(f"plugin scan done: {status.get('count', 0)} plugins, "
                  f"{status.get('blacklisted', 0)} blacklisted")
            return
        progress = float(status.get("progress") or 0.0)
        detail = f"{progress * 100:.0f}%"
        if status.get("currentFile"):
            detail += f" {status['currentFile']}"
        if status.get("hangCount"):
            detail += f" ({status['hangCount']} hung)"
        print(f"\rscanning... {detail:<70}", end="", flush=True)
        printed_progress = True
        time.sleep(2.0)
        if time.monotonic() >= deadline:
            raise TimeoutError(f"plugin scan did not finish within {timeout_sec:.0f}s")


def load_plugin(pc: types.ModuleType, handle: int, file: str) -> tuple[bool, str]:
    """loadPlugin with one retry after 3s. Returns (ok, name_or_error).

    ConnectionError (the app process died — a plugin crashed the host)
    PROPAGATES: retrying against a dead pipe is pointless; the caller aborts
    the batch.
    """
    for attempt in (1, 2):
        try:
            resp = request(pc, handle, {"cmd": "loadPlugin", "path": file},
                           timeout_sec=LOAD_TIMEOUT_SEC)
            if resp.get("ok"):
                return True, str(resp.get("name") or "")
            error = str(resp.get("error") or "load failed")
        except (TimeoutError, RuntimeError) as exc:
            error = str(exc)
        if attempt == 1:
            print(f"  loadPlugin failed ({error}) — retrying in 3s")
            time.sleep(3.0)
    return False, error


# ---------------------------------------------------------------------------
# App launch / quit (WM_CLOSE only — no GUI automation)
# ---------------------------------------------------------------------------


def launch_app() -> int:
    """Popen the app executable (cwd = repo root); returns the PID."""
    if not APP_EXE.exists():
        print(f"ERROR: app executable not found: {APP_EXE}")
        sys.exit(1)
    proc = subprocess.Popen([str(APP_EXE)], cwd=str(REPO_ROOT))
    print(f"launched {APP_EXE.name} (PID {proc.pid})")
    return proc.pid


def find_app_pids() -> list[int]:
    """Locate running 'Plugin Lab.exe' processes via tasklist (CSV parse)."""
    try:
        result = subprocess.run(
            ["tasklist", "/FI", "IMAGENAME eq Plugin Lab.exe", "/FO", "CSV", "/NH"],
            capture_output=True, text=True, encoding="utf-8", errors="replace",
            timeout=30.0)
    except (OSError, subprocess.TimeoutExpired) as exc:
        print(f"warning: tasklist failed: {exc}")
        return []
    pids: list[int] = []
    for row in csv.reader(result.stdout.splitlines()):
        if len(row) >= 2 and row[0] == "Plugin Lab.exe":
            try:
                pids.append(int(row[1]))
            except ValueError:
                pass
    return pids


def quit_app(pc: types.ModuleType, launched_pid: int | None) -> None:
    """Post WM_CLOSE to the app: launched PID, else discovered via tasklist.

    After posting WM_CLOSE the app is expected to exit promptly (the visible-
    window filter in close_app_by_pid avoids the hidden-window close hang);
    poll up to 10s, then force-kill via taskkill as a fallback. A missing
    process/window is a warning, never an error.
    """
    pids = [launched_pid] if launched_pid is not None else find_app_pids()
    if not pids:
        print("--quit: no Plugin Lab process found — nothing to close")
        return
    for pid in pids:
        try:
            if pc.close_app_by_pid(pid):
                print(f"--quit: posted WM_CLOSE to PID {pid}")
                for _ in range(20):          # up to 10s for a clean exit
                    time.sleep(0.5)
                    if not _pid_alive(pid):
                        print(f"--quit: PID {pid} exited cleanly")
                        break
                else:
                    print(f"--quit: PID {pid} still alive after 10s — "
                          "force-killing (taskkill /F)")
                    subprocess.run(["taskkill", "/PID", str(pid), "/F"],
                                   capture_output=True, text=True, timeout=15)
            else:
                print(f"--quit: warning — no window found for PID {pid}")
        except RuntimeError as exc:
            print(f"--quit: warning — cannot close PID {pid}: {exc}")


def _pid_alive(pid: int) -> bool:
    """Stdlib-only process liveness check via tasklist (PID CSV row present)."""
    result = subprocess.run(["tasklist", "/FI", f"PID eq {pid}",
                             "/FO", "CSV", "/NH"],
                            capture_output=True, text=True, errors="replace",
                            timeout=15)
    return str(pid) in result.stdout


# ---------------------------------------------------------------------------
# Plugin plan
# ---------------------------------------------------------------------------


def resolve_plugins(plugins: list[PluginInfo], blacklist: set[str],
                    plugin_name: str | None, limit: int | None,
                    plugins_cfg: dict) -> list[PlanEntry]:
    """Select the plugin plan: explicit name or --all (blacklist + limit)."""
    if plugin_name is not None:
        for info in plugins:
            if info.name.lower() == plugin_name.lower():
                if normalize_path(info.file) in blacklist:
                    print(f"ERROR: plugin {info.name!r} is blacklisted in the "
                          "plugin cache — remove it from pluginlist.xml first")
                    sys.exit(1)
                return [PlanEntry(info.name, info.file, make_slug(info.name),
                                  plugin_config(plugins_cfg, info.name))]
        print(f"ERROR: plugin not found in cache: {plugin_name!r}")
        sys.exit(1)
    selected = [info for info in plugins if normalize_path(info.file) not in blacklist]
    if limit is not None:
        selected = selected[:limit]
    return [PlanEntry(info.name, info.file, make_slug(info.name),
                      plugin_config(plugins_cfg, info.name)) for info in selected]


# ---------------------------------------------------------------------------
# Per-plugin pipeline
# ---------------------------------------------------------------------------


def wait_plugin_loaded(pc: types.ModuleType, handle: int,
                       timeout_sec: float = 45.0) -> None:
    """Wait until the async plugin load completes on the app's message thread.

    loadPlugin responds ok:true BEFORE the actual load finishes (the load runs
    via a callAsync on the message thread, up to the app's 30s load timeout) —
    so a following dataset command races it and fails with "no session or
    plugin". getParams answers ok:false "no plugin loaded" until the instance
    is set: poll it until ok:true or the deadline. Raises RuntimeError on
    timeout (the load effectively failed).
    """
    deadline = time.monotonic() + timeout_sec
    while True:
        resp = request(pc, handle, {"cmd": "getParams"}, timeout_sec=30.0)
        if resp.get("ok"):
            return
        if time.monotonic() >= deadline:
            raise RuntimeError(f"load did not complete within {timeout_sec:.0f}s "
                               f"(last: {resp.get('error') or 'unknown'})")
        time.sleep(0.5)


def run_reverse_derive(dataset_path: Path, expected: dict) -> tuple[int | None, str]:
    """Run tools/reverse_derive.py; returns (exit_code_or_None, report_text).

    Expected values map to --expected-* flags; a timeout yields exit None
    with the timeout noted inside the report.
    """
    cmd = [sys.executable, "tools/reverse_derive.py", str(dataset_path)]
    for key in EXPECTED_FLAGS:
        if key in expected:
            cmd += [EXPECTED_FLAGS[key], str(expected[key])]
    try:
        result = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8",
                                errors="replace", cwd=str(REPO_ROOT),
                                timeout=RD_TIMEOUT_SEC)
        rc, stdout, stderr = result.returncode, result.stdout, result.stderr
    except subprocess.TimeoutExpired as exc:
        rc = None
        stdout = str(exc.stdout or "")
        stderr = f"[reverse_derive timed out after {RD_TIMEOUT_SEC:.0f}s]"
    report = stdout
    if stderr.strip():
        report += "\n--- stderr ---\n" + stderr
    return rc, report


def _fail_entry(entry: PlanEntry, reason: str, started: float) -> dict:
    """Summary entry for a plugin that never produced a verified dataset."""
    return {
        "name": entry.name, "slug": entry.slug, "ok": False,
        "types": None, "scan": False, "compression_family": False,
        "elapsed_sec": round(time.monotonic() - started, 1),
        "reverse_derive_exit": None, "skip_reason": reason,
    }


class AppGoneError(RuntimeError):
    """The Plugin Lab process died (a plugin crashed the host) — the batch
    cannot continue; the caller aborts and writes the partial summary."""


def process_one(pc: types.ModuleType, handle: int, entry: PlanEntry,
                out_dir: Path, types_override: list[str] | None) -> dict:
    """Run the full per-plugin pipeline; returns a summary entry dict.

    ok == dataset ok:true AND reverse_derive completed with exit 0.
    """
    started = time.monotonic()
    plugin_dir = out_dir / entry.slug
    try:
        plugin_dir.mkdir(parents=True, exist_ok=True)
    except OSError as exc:
        return _fail_entry(entry, f"mkdir failed: {exc}", started)
    dataset_path = (plugin_dir / "dataset.json").resolve()

    # ConnectionError = the app process died (a plugin crashed the host):
    # abort the batch (AppGoneError). Command-level failures (timeout / bad
    # response) become per-plugin fail entries and the batch continues.
    try:
        ok, name_or_error = load_plugin(pc, handle, entry.file)
        if not ok:
            return _fail_entry(entry, f"loadPlugin: {name_or_error}", started)
        print(f"  loaded {entry.name!r}" + (f" ({name_or_error})" if name_or_error else ""))

        # loadPlugin answers before the message-thread load finishes — wait
        # for the instance to be set before the dataset battery.
        try:
            wait_plugin_loaded(pc, handle)
        except RuntimeError as exc:
            return _fail_entry(entry, f"loadPlugin: {exc}", started)

        payload: dict = {"cmd": "dataset", "path": str(dataset_path)}
        if types_override is not None:
            payload["types"] = types_override
        if entry.cfg.get("scan"):
            payload["scan"] = entry.cfg["scan"]
        if entry.cfg.get("compression_family"):
            payload["compression_family"] = entry.cfg["compression_family"]

        resp = request(pc, handle, payload, timeout_sec=DATASET_TIMEOUT_SEC)
        if not resp.get("ok"):
            return _fail_entry(entry, f"dataset: {resp.get('error') or 'failed'}", started)

        requested = types_override or DEFAULT_TYPES
        resp_types = resp.get("types")
        flags = {t: bool(resp_types.get(t, False)) if isinstance(resp_types, dict) else True
                 for t in requested}
        scan_ok = bool(resp.get("scan", False))
        cf_ok = bool(resp.get("compression_family", False))

        rc, report = run_reverse_derive(dataset_path, entry.cfg.get("expected") or {})
        try:
            (plugin_dir / "report.txt").write_text(report, encoding="utf-8")
        except OSError as exc:
            print(f"  warning: cannot write report for {entry.name!r}: {exc}")

        return {
            "name": entry.name, "slug": entry.slug, "ok": rc == 0,
            "types": flags, "scan": scan_ok, "compression_family": cf_ok,
            "elapsed_sec": round(time.monotonic() - started, 1),
            "reverse_derive_exit": rc, "skip_reason": None,
        }
    except ConnectionError as exc:
        raise AppGoneError(f"app died during {entry.name!r}: {exc}") from exc
    except (TimeoutError, RuntimeError) as exc:
        return _fail_entry(entry, f"{type(exc).__name__}: {exc}", started)


def format_one_line(result: dict, entry: PlanEntry, requested: list[str]) -> str:
    """One-line status, e.g. '[ok] Pro-Q 4 — 4/4 types, scan✓, rd exit 0 (12.3s)'."""
    tag = "[ok] " if result["ok"] else "[FAIL] "
    line = f"{tag}{entry.name}"
    if result["skip_reason"]:
        return f"{line} — skipped: {result['skip_reason']} ({result['elapsed_sec']:.1f}s)"
    parts: list[str] = []
    types = result["types"] or {}
    ok_types = sum(1 for t in requested if types.get(t))
    parts.append(f"{ok_types}/{len(requested)} types")
    if entry.cfg.get("scan"):
        parts.append("scan✓" if result["scan"] else "scan✗")
    if entry.cfg.get("compression_family"):
        parts.append("cf✓" if result["compression_family"] else "cf✗")
    parts.append(f"rd exit {result['reverse_derive_exit']}")
    return f"{line} — {', '.join(parts)} ({result['elapsed_sec']:.1f}s)"


# ---------------------------------------------------------------------------
# Batch summary
# ---------------------------------------------------------------------------


def write_summary(out_dir: Path, entries: list[dict]) -> Path:
    """Write out/summary.json and print a tally line."""
    ok_count = sum(1 for e in entries if e["ok"])
    doc = {
        "generated_at": datetime.now().astimezone().isoformat(),
        "out_dir": str(out_dir.resolve()),
        "plugins": entries,
        "ok_count": ok_count,
        "fail_count": len(entries) - ok_count,
    }
    path = out_dir / "summary.json"
    path.write_text(json.dumps(doc, indent=2, ensure_ascii=False) + "\n",
                    encoding="utf-8")
    print(f"summary: {path} ({ok_count} ok, {len(entries) - ok_count} failed)")
    return path


# ---------------------------------------------------------------------------
# Modes
# ---------------------------------------------------------------------------


def run_dry_run(args: argparse.Namespace) -> int:
    """Plan-only mode: parse cache, validate config, print the plan. Exit 0."""
    cache = cache_path()
    if not cache.exists():
        print(f"ERROR: plugin cache not found: {cache}")
        return 1
    plugins_cfg = load_config(args.config)
    plugins, blacklist = parse_plugin_cache(cache)
    plan = resolve_plugins(plugins, blacklist, args.plugin, args.limit, plugins_cfg)
    if not plan:
        print("ERROR: no plugins to process")
        return 1

    print(f"plugin cache: {cache}")
    if args.config:
        print(f"config: valid ({len(plugins_cfg)} plugin(s) configured)")
    else:
        print("config: none")
    if args.all:
        print(f"plugins: {len(plan)} selected "
              f"(of {len(plugins)} total, {len(blacklist)} blacklisted)")
    else:
        print(f"plugins: {len(plan)} selected")

    for i, entry in enumerate(plan, 1):
        cfg = entry.cfg
        types_str = ",".join(args.types) if args.types else "all 4"
        scan = cfg.get("scan")
        scan_str = (f"scan {scan['param_id']} x{len(scan['values'])}" if scan else "-")
        cf_str = "compression_family" if cfg.get("compression_family") else "-"
        expected = cfg.get("expected")
        exp_str = ",".join(sorted(expected)) if expected else "-"
        print(f"  [{i:>2}] {entry.name:<30} -> {entry.slug:<26} "
              f"types={types_str} scan={scan_str} cf={cf_str} expected={exp_str}")

    known = {p.name.lower() for p in plugins}
    for key in plugins_cfg:
        if key.lower() not in known:
            print(f"  warning: config plugin {key!r} not found in cache (skipped)")

    out_dir = REPO_ROOT / args.out
    ok, reason = check_out_dir_creatable(out_dir)
    if not ok:
        print(f"ERROR: output dir not creatable: {out_dir.resolve()} ({reason})")
        return 1
    print(f"output dir: {out_dir.resolve()} (creatable: yes)")
    return 0


def run(args: argparse.Namespace) -> int:
    """Full batch flow; returns the process exit code."""
    cache = cache_path()
    plugins_cfg = load_config(args.config)
    out_dir = REPO_ROOT / args.out

    pc: types.ModuleType | None = None
    handle: int | None = None
    launched_pid: int | None = None
    entries: list[dict] = []

    try:
        pc = load_pipe_client()
        if args.launch:
            launched_pid = launch_app()
        try:
            handle = connect_with_deadline(pc, CONNECT_TIMEOUT_SEC)
        except RuntimeError as exc:
            print(f"ERROR: {exc}")
            return 2
        except TimeoutError as exc:
            print(f"ERROR: {exc}")
            return 1
        print(f"connected to {PIPE_NAME}")

        wait_scan_done(pc, handle, SCAN_TIMEOUT_SEC)

        plugins, blacklist = parse_plugin_cache(cache)
        plan = resolve_plugins(plugins, blacklist, args.plugin, args.limit,
                               plugins_cfg)
        if not plan:
            print("ERROR: no plugins to process")
            return 1
        print(f"processing {len(plan)} plugin(s) -> {out_dir.resolve()}")

        for entry in plan:
            try:
                result = process_one(pc, handle, entry, out_dir, args.types)
            except AppGoneError as exc:
                print(f"  ABORT: {exc}")
                entries.append(_fail_entry(entry, f"app died: {exc}",
                                           time.monotonic()))
                break
            entries.append(result)
            print(format_one_line(result, entry, args.types or DEFAULT_TYPES))

        write_summary(out_dir, entries)
        ok_count = sum(1 for e in entries if e["ok"])
        fail_count = len(entries) - ok_count
        print(f"batch complete: {ok_count} ok, {fail_count} failed "
              f"({len(entries)} total)")
        return 0 if ok_count > 0 and fail_count == 0 else 1

    except KeyboardInterrupt:
        print("\ninterrupted — writing summary of completed plugins")
        if entries:
            write_summary(out_dir, entries)
        return 130
    except (TimeoutError, ConnectionError, RuntimeError) as exc:
        print(f"ERROR: {exc}")
        return 1
    finally:
        if pc is not None:
            if handle is not None:
                try:
                    pc.close(handle)
                except Exception as exc:      # teardown must not mask the outcome
                    print(f"warning: pipe close failed: {exc}")
            if args.quit:
                quit_app(pc, launched_pid)
        elif args.quit:
            print("--quit: skipped (pipe_client unavailable)")


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Batch-drive the PluginLab measurement pipeline "
                    "(scan wait -> loadPlugin -> dataset -> reverse_derive)")
    parser.add_argument("--plugin", metavar="NAME",
                        help="measure one plugin (case-insensitive name match)")
    parser.add_argument("--all", action="store_true",
                        help="measure every non-blacklisted plugin in cache order")
    parser.add_argument("--out", default="out", metavar="DIR",
                        help="output directory relative to the repo root (default: out)")
    parser.add_argument("--types", metavar="LIST",
                        help="comma-separated battery types (default: all 4)")
    parser.add_argument("--config", metavar="FILE", help="per-plugin config JSON")
    parser.add_argument("--limit", type=int, metavar="N",
                        help="with --all: first N non-blacklisted plugins")
    parser.add_argument("--launch", action="store_true",
                        help="launch the app executable before connecting")
    parser.add_argument("--quit", action="store_true",
                        help="close the app after the run (WM_CLOSE)")
    parser.add_argument("--dry-run", action="store_true",
                        help="plan only: parse cache, validate config, print plan")
    args = parser.parse_args()

    if args.plugin and args.all:
        parser.error("--plugin and --all are mutually exclusive")
    if not args.plugin and not args.all:
        parser.error("either --plugin NAME or --all is required")
    if args.limit is not None and not args.all:
        parser.error("--limit requires --all")
    if args.types is not None:
        try:
            args.types = parse_types(args.types)
        except ValueError as exc:
            parser.error(str(exc))
    return args


def main() -> int:
    # The per-plugin summary uses ✓/✗ glyphs — ensure non-ASCII output can
    # never crash the driver on a console codepage that cannot encode them.
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8", errors="replace")
        except (AttributeError, ValueError):
            pass
    args = parse_args()
    if args.dry_run:
        if args.launch or args.quit:
            print("note: --launch/--quit are ignored in --dry-run")
        return run_dry_run(args)
    return run(args)


if __name__ == "__main__":
    sys.exit(main())
