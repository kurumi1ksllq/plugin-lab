"""
pipe_client.py — Thin Windows Named Pipe client for the PluginLab IPC protocol.

Speaks to the PluginLab GUI's pipe server (\\\\.\\pipe\\PluginLab, see
source/ipc/PipeServer.cpp and source/ipc/Protocol.h) over JSON-lines:

  - one request line per command, one response line back
    (commands like dataset/scan block for minutes — pass generous timeouts)
  - connect may race the server startup: ERROR_FILE_NOT_FOUND /
    ERROR_PIPE_BUSY are retried with WaitNamedPipe between attempts
  - the client handle is switched to PIPE_READMODE_MESSAGE | PIPE_NOWAIT
    so each ReadFile returns exactly one server message and an empty pipe
    surfaces as ERROR_NO_DATA — read_line polls that with a Python-side
    deadline (SetNamedPipeHandleState's byte-mode collection timeout is not
    supported on this message-type pipe and fails with ERROR_INVALID_PARAMETER)
  - strictly sequential by design: connect -> send one line -> read one
    response line -> (repeat) -> close

This module is the ONLY module in the repo allowed to depend on pywin32
(see docs/plan-batch-pipeline.md). It imports pywin32 lazily-friendly:
a missing pywin32 surfaces as a module-level _PYWIN32_IMPORT_ERROR and
every public function then raises RuntimeError immediately, so a batch
driver can import this module without pywin32 installed (e.g. --dry-run).

Usage:
    import pipe_client as pc
    h = pc.connect()
    try:
        pc.send_line(h, '{"cmd": "getStatus"}')
        print(pc.read_line(h, timeout_sec=30))
    finally:
        pc.close(h)
"""
import time as _time

# ---------------------------------------------------------------------------
# pywin32 — optional import; everything else is stdlib
# ---------------------------------------------------------------------------

_PIPE_NAME = r"\\.\pipe\PluginLab"
_PYWIN32_IMPORT_ERROR: str | None = None

try:
    import win32api as _win32api   # noqa: F401  (pulled in by gui/process)
    import win32con as _win32con
    import win32file as _win32file
    import win32gui as _win32gui
    import win32pipe as _win32pipe
    import win32process as _win32process
except ImportError:
    _PYWIN32_IMPORT_ERROR = "pywin32 missing — run: pip install pywin32"


def _require_pywin32() -> None:
    """Raise RuntimeError up front when pywin32 is not installed."""
    if _PYWIN32_IMPORT_ERROR is not None:
        raise RuntimeError(_PYWIN32_IMPORT_ERROR)


def _winerror(exc: Exception) -> int:
    """Extract the Windows error code from a pywin32/pywintypes error."""
    winerror = getattr(exc, "winerror", None)
    if winerror is not None:
        return int(winerror)
    if exc.args and isinstance(exc.args[0], int):
        return exc.args[0]
    return -1


# ---------------------------------------------------------------------------
# Connection lifecycle
# ---------------------------------------------------------------------------


def connect(retries: int = 10, retry_delay_sec: float = 0.5) -> int:
    """Open \\\\.\\pipe\\PluginLab, retrying while the server is not ready.

    ERROR_FILE_NOT_FOUND (2) and ERROR_PIPE_BUSY (231) mean the server is
    still starting or busy: wait WaitNamedPipe(retry_delay_sec) and retry.
    Raises TimeoutError after ``retries`` consecutive failures. Returns the
    pipe handle.
    """
    _require_pywin32()
    for attempt in range(retries + 1):
        try:
            handle = _win32file.CreateFileW(
                _PIPE_NAME,
                _win32con.GENERIC_READ | _win32con.GENERIC_WRITE,
                0,
                None,
                _win32con.OPEN_EXISTING,
                0,
                None,
            )
        except _win32file.error as exc:
            winerror = _winerror(exc)
            if winerror not in (2, 231):
                raise
            if attempt >= retries:
                break
            try:
                _win32pipe.WaitNamedPipe(_PIPE_NAME, int(retry_delay_sec * 1000))
            except _win32pipe.error as exc:
                # Pipe still busy (121) or the server vanished (2/233) —
                # loop and retry CreateFileW.
                if _winerror(exc) not in (2, 121, 233):
                    raise
            continue
        # The server pipe is PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE |
        # PIPE_WAIT. Switch this client handle to message-read + non-blocking
        # so read_line can enforce its deadline by polling ERROR_NO_DATA
        # instead of relying on SetNamedPipeHandleState's byte-mode read
        # timeout (which fails with ERROR_INVALID_PARAMETER on message pipes).
        try:
            _win32pipe.SetNamedPipeHandleState(
                handle,
                _win32con.PIPE_READMODE_MESSAGE | _win32con.PIPE_NOWAIT,
                None,
                None,
            )
        except _win32pipe.error:
            _win32file.CloseHandle(handle)
            raise
        return handle
    raise TimeoutError("pipe not available")


def close(handle: int) -> None:
    """Close a pipe handle. Benign failures are ignored, others surface."""
    _require_pywin32()
    try:
        _win32file.CloseHandle(handle)
    except _win32file.error as exc:
        # Already-closed/invalid handle (6) and broken pipe (109) are
        # expected at teardown — nothing left to do.
        if _winerror(exc) not in (6, 109):
            raise


# ---------------------------------------------------------------------------
# Line protocol
# ---------------------------------------------------------------------------


def send_line(handle: int, line: str) -> None:
    """Write one request line plus trailing newline (UTF-8).

    WriteFile is immediate — no flush is required. A pipe that the server is
    closing (109/232/233) surfaces as ConnectionError so callers can treat a
    dead host uniformly (raw pywintypes.error would escape the app-death
    handling in the batch driver).
    """
    _require_pywin32()
    try:
        _win32file.WriteFile(handle, (line + "\n").encode("utf-8"))
    except _win32file.error as exc:
        winerror = _winerror(exc)
        if winerror in (109, 232, 233):
            raise ConnectionError(f"pipe closed by server (winerror {winerror})") from exc
        raise


def read_line(handle: int, timeout_sec: float = 30.0) -> str:
    """Read one response line (one server message) within the deadline.

    The client handle runs in PIPE_READMODE_MESSAGE | PIPE_NOWAIT (set in
    connect), so each successful ReadFile returns exactly one message and an
    empty pipe surfaces as ERROR_NO_DATA (232) — poll that with a short
    sleep until the deadline, at which point TimeoutError is raised.
    ERROR_MORE_DATA (234) means the current message is larger than the
    buffer: keep accumulating. A closed pipe (109/233) raises
    ConnectionError. Returns the message WITHOUT the trailing newline.
    """
    _require_pywin32()
    deadline = _time.monotonic() + timeout_sec
    buffer = b""
    while True:
        remaining = deadline - _time.monotonic()
        if remaining <= 0:
            raise TimeoutError(f"no response within {timeout_sec:.1f}s")
        try:
            hr, data = _win32file.ReadFile(handle, 4096)
        except _win32file.error as exc:
            hr, data = _winerror(exc), b""
        if hr == 0:                      # one complete message delivered
            buffer += data
            return buffer.decode("utf-8").rstrip("\r\n")
        if hr == 234:                    # ERROR_MORE_DATA — message continues
            buffer += data
            continue
        if hr == 232:                    # ERROR_NO_DATA — nothing yet (NOWAIT)
            _time.sleep(0.02)
            continue
        if hr == 121:                    # ERROR_SEM_TIMEOUT — deadline passed
            raise TimeoutError(f"no response within {timeout_sec:.1f}s")
        if hr in (109, 233):             # ERROR_BROKEN_PIPE / NOT_CONNECTED
            raise ConnectionError(f"pipe closed by server (winerror {hr})")
        raise RuntimeError(f"ReadFile failed (winerror {hr})")


# ---------------------------------------------------------------------------
# Window management (WM_CLOSE only — zero GUI automation)
# ---------------------------------------------------------------------------


def close_app_by_pid(pid: int) -> bool:
    """Post WM_CLOSE to every VISIBLE top-level window of the given process.

    Message-only: no mouse input, no SetForegroundWindow, no keyboard.
    Only visible windows are targeted: posting WM_CLOSE to hidden system
    windows (IME, JUCE utility windows) breaks the app's close sequence and
    can hang it (verified against the real app). Returns True if at least one
    window was found (and messaged), else False.
    """
    _require_pywin32()
    hwnds: list[int] = []

    def _collect(hwnd: int, _unused: object) -> bool:
        if pid == _win32process.GetWindowThreadProcessId(hwnd)[1] \
                and _win32gui.IsWindowVisible(hwnd):
            hwnds.append(hwnd)
        return True                       # keep enumerating

    _win32gui.EnumWindows(_collect, None)
    for hwnd in hwnds:
        _win32gui.PostMessage(hwnd, _win32con.WM_CLOSE, 0, 0)
    return len(hwnds) > 0
