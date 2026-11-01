"""
ui_modal_stall_test.py - the UI heartbeat must survive Windows' modal loops.

While the user drags / resizes the window or opens its system menu, Windows runs
its own message loop and the application's main loop does not run. The fieldbus
thread supervises the UI heartbeat; if the heartbeat stops for more than
HMI_WATCHDOG_MAX_AGE_MS it raises a CRITICAL "UI loop stalled" failover (and the
horn). That is a false alarm: the UI is fine, the user is just moving the window.

This test launches the real exe with the built-in simulator, keeps the window in
(1) keyboard move mode and (2) the system menu for a while, then checks that no
watchdog event reached the alarm log and the fieldbus watchdog never faulted.
It verifies that the window thread really is inside the modal loop (GetGUIThreadInfo)
before it trusts the result, so a run that never entered the loop cannot pass.

Usage (needs an interactive Windows desktop):
    python tests\\ui_modal_stall_test.py [path\\to\\industrial_hmi.exe] [hold_seconds]

Exit code: 0 = pass, 1 = fail, 2 = could not run (exe missing / blocked by Windows
Application Control / no window / modal loop not entered). A skipped run is NOT a pass.
"""
import ctypes
import os
import shutil
import subprocess
import sys
import tempfile
import time
from ctypes import wintypes

HERE = os.path.dirname(os.path.abspath(__file__))
EXE = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, "..", "industrial_hmi.exe")
HOLD = float(sys.argv[2]) if len(sys.argv) > 2 else 2.0

WM_SYSCOMMAND, WM_KEYDOWN, WM_KEYUP, WM_CLOSE = 0x0112, 0x0100, 0x0101, 0x0010
SC_MOVE, SC_KEYMENU, VK_ESCAPE = 0xF010, 0xF100, 0x1B
GUI_INMOVESIZE, GUI_INMENUMODE = 0x2, 0x4
CLASS_NAME = b"IndustrialHMI_WinClass"


class GUITHREADINFO(ctypes.Structure):
    _fields_ = [("cbSize", wintypes.DWORD), ("flags", wintypes.DWORD), ("hwndActive", wintypes.HWND),
                ("hwndFocus", wintypes.HWND), ("hwndCapture", wintypes.HWND), ("hwndMenuOwner", wintypes.HWND),
                ("hwndMoveSize", wintypes.HWND), ("hwndCaret", wintypes.HWND), ("rcCaret", wintypes.RECT)]


u32 = ctypes.WinDLL("user32", use_last_error=True)
u32.PostMessageA.argtypes = [wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM]
u32.GetWindowThreadProcessId.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.DWORD)]
u32.GetWindowThreadProcessId.restype = wintypes.DWORD
u32.GetClassNameA.argtypes = [wintypes.HWND, ctypes.c_char_p, ctypes.c_int]
u32.GetGUIThreadInfo.argtypes = [wintypes.DWORD, ctypes.POINTER(GUITHREADINFO)]
u32.SetForegroundWindow.argtypes = [wintypes.HWND]
_ENUMPROC = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
u32.EnumWindows.argtypes = [_ENUMPROC, wintypes.LPARAM]


def find_window(pid):
    """The HMI window owned by process `pid` (other HMI instances on the desktop are ignored)."""
    found = []

    def cb(h, _):
        owner = wintypes.DWORD()
        u32.GetWindowThreadProcessId(h, ctypes.byref(owner))
        if owner.value == pid:
            name = ctypes.create_string_buffer(64)
            u32.GetClassNameA(h, name, 64)
            if name.value == CLASS_NAME:
                found.append(h)
        return True

    u32.EnumWindows(_ENUMPROC(cb), 0)
    return found[0] if found else 0


def in_modal_loop(hwnd, mask):
    tid = u32.GetWindowThreadProcessId(hwnd, None)
    gti = GUITHREADINFO()
    gti.cbSize = ctypes.sizeof(GUITHREADINFO)
    return bool(u32.GetGUIThreadInfo(tid, ctypes.byref(gti))) and bool(gti.flags & mask)


def run_case(name, enter, mask):
    """Returns (ok, detail); raises RuntimeError when the case could not be exercised."""
    out = tempfile.mkdtemp(prefix="hmi_modal_")
    proc = None
    try:
        log = os.path.join(out, "alarms.log")
        rep = os.path.join(out, "report.txt")
        try:
            proc = subprocess.Popen([EXE, "--sim", "--no-discovery", "--log", log, "--report", rep,
                                     "--vault", os.path.join(out, "security.vault")])
        except OSError as e:
            raise RuntimeError("cannot start %s: %s" % (EXE, e))
        hwnd = 0
        for _ in range(100):
            hwnd = find_window(proc.pid)
            if hwnd:
                break
            if proc.poll() is not None:
                raise RuntimeError("exe exited early with code %s" % proc.returncode)
            time.sleep(0.1)
        if not hwnd:
            raise RuntimeError("HMI window not found")

        time.sleep(2.0)                      # let the simulator attach and the loops settle
        u32.SetForegroundWindow(hwnd)
        enter(hwnd)
        entered = False
        for _ in range(40):
            if in_modal_loop(hwnd, mask):
                entered = True
                break
            time.sleep(0.05)
        if not entered:
            raise RuntimeError("the window never entered the modal loop; nothing was tested")

        time.sleep(HOLD)                     # stay in the loop well past the 500 ms watchdog limit
        still_in = in_modal_loop(hwnd, mask)
        u32.PostMessageA(hwnd, WM_KEYDOWN, VK_ESCAPE, 0)
        u32.PostMessageA(hwnd, WM_KEYUP, VK_ESCAPE, 0)
        time.sleep(1.5)
        u32.PostMessageA(hwnd, WM_CLOSE, 0, 0)
        try:
            proc.wait(timeout=8)
        except subprocess.TimeoutExpired:
            raise RuntimeError("exe did not exit after WM_CLOSE")
        if not still_in:
            raise RuntimeError("the modal loop ended before the hold time; nothing was tested")

        stalled = []
        if os.path.exists(log):
            with open(log, encoding="utf-8", errors="replace") as f:
                stalled = [l.strip() for l in f if "UI loop stalled" in l or "watchdog failover" in l]
        faults = None
        if os.path.exists(rep):
            with open(rep, encoding="utf-8", errors="replace") as f:
                for l in f:
                    if l.startswith("fb_wd_fault_count="):
                        faults = int(l.split("=", 1)[1])
        if faults is None:
            raise RuntimeError("report file missing: the exe did not shut down cleanly")
        ok = not stalled and faults == 0
        detail = "fb_wd_fault_count=%d, watchdog events in log: %d" % (faults, len(stalled))
        for l in stalled:
            detail += "\n      " + l
        return ok, detail
    finally:
        if proc is not None and proc.poll() is None:
            proc.kill()
            proc.wait()
        shutil.rmtree(out, ignore_errors=True)


def main():
    if not os.path.isfile(EXE):
        print("SKIPPED: exe not found: %s" % EXE)
        return 2
    cases = [
        ("window move loop", lambda h: u32.PostMessageA(h, WM_SYSCOMMAND, SC_MOVE, 0), GUI_INMOVESIZE),
        ("system menu loop", lambda h: u32.PostMessageA(h, WM_SYSCOMMAND, SC_KEYMENU, ord(" ")), GUI_INMENUMODE),
    ]
    failed = 0
    for name, enter, mask in cases:
        result = None
        for attempt in range(1, 4):      # entering a modal loop depends on focus and timing: retry a few times
            try:
                result = run_case(name, enter, mask)
                break
            except RuntimeError as e:
                print("  attempt %d [%s] not exercised: %s" % (attempt, name, e))
        if result is None:
            print("SKIPPED [%s]: could not put the window into the modal loop" % name)
            return 2
        ok, detail = result
        print("%s [%s] held %.1f s: %s" % ("PASS" if ok else "FAIL", name, HOLD, detail))
        failed += 0 if ok else 1
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
