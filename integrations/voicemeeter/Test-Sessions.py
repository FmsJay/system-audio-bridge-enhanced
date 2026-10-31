"""End-to-end test of vm-sessions.exe against live Windows audio sessions.

Starts its own -70 dBFS tone sessions (vmtone.exe) on the three Voicemeeter virtual inputs, opens
the panel, and drives it with window messages: slider to -12 dB, arrow keys, Home/End, Mute, and
mouse-wheel scrolling. Each change is checked through `vm-sessions --dump`. Only the test's own
sessions are touched. Pass --shot to save a screenshot of the panel.
"""
import argparse
import ctypes as c
from ctypes import wintypes as w
import json
from pathlib import Path
import struct
import subprocess
import sys
import time

HERE = Path(__file__).resolve().parent
BIN = HERE / "build-tools" / "Release"
u = c.windll.user32
u.SetProcessDPIAware()
WM_KEYDOWN, WM_HSCROLL, WM_MOUSEWHEEL, BM_CLICK, BM_GETCHECK = 0x100, 0x114, 0x20A, 0xF5, 0xF0
TBM_GETPOS, TBM_SETPOS, SB_THUMBPOSITION = 0x400, 0x405, 4
VK_RIGHT, VK_HOME, VK_END = 0x27, 0x24, 0x23
u.SendMessageW.argtypes = [w.HWND, w.UINT, w.WPARAM, w.LPARAM]
u.SendMessageW.restype = c.c_ssize_t
u.PostMessageW.argtypes = [w.HWND, w.UINT, w.WPARAM, w.LPARAM]
failures = 0


def check(ok, what, detail=""):
    global failures
    print(("ok  " if ok else "FAIL"), what, detail)
    failures += not ok


def dump():
    out = subprocess.run([str(BIN / "vm-sessions.exe"), "--dump"], capture_output=True, text=True,
                         encoding="utf-8", timeout=30).stdout
    return [json.loads(line) for line in out.splitlines() if line.strip()]


def mine(pid):
    return next((s for s in dump() if s["pid"] == pid), None)


def text(h):
    buf = c.create_unicode_buffer(256)
    u.GetWindowTextW(h, buf, 256)
    return buf.value


def cls(h):
    buf = c.create_unicode_buffer(64)
    u.GetClassNameW(h, buf, 64)
    return buf.value


def children(parent):
    found = []
    proc = c.WINFUNCTYPE(w.BOOL, w.HWND, w.LPARAM)(lambda h, _: found.append(h) or True)
    u.EnumChildWindows(parent, proc, 0)
    return found


def screenshot(h, path):
    from PIL import Image
    g = c.windll.gdi32
    r = w.RECT()
    u.GetWindowRect(h, c.byref(r))
    x, y = r.right - r.left, r.bottom - r.top
    dc = u.GetDC(h)
    mem = g.CreateCompatibleDC(dc)
    bm = g.CreateCompatibleBitmap(dc, x, y)
    g.SelectObject(mem, bm)
    u.PrintWindow(h, mem, 2)
    hdr = struct.pack("IiiHHIIiiII", 40, x, -y, 1, 32, 0, 0, 0, 0, 0, 0)
    buf = c.create_string_buffer(x * y * 4)
    g.GetDIBits(mem, bm, 0, y, buf, hdr, 0)
    Image.frombytes("RGB", (x, y), buf.raw, "raw", "BGRX").save(path)


parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--shot", type=Path)
args = parser.parse_args()

endpoints = ["Voicemeeter AUX Input", "Voicemeeter Input (", "Voicemeeter VAIO3 Input"]
tones = [subprocess.Popen([str(BIN / "vmtone.exe"), endpoints[i % 3], "-70", "40", "440"],
                          stdout=subprocess.DEVNULL) for i in range(12)]
panel = None
try:
    time.sleep(1.5)
    pid = tones[0].pid
    s = mine(pid)
    check(s is not None and s["strip"].startswith("Voicemeeter AUX"), "tone session listed on AUX", str(s))
    check(sum(1 for x in dump() if x["name"] == "vmtone") == 12, "all 12 test sessions listed")

    panel = subprocess.Popen([str(BIN / "vm-sessions.exe")])
    time.sleep(2.0)
    main = u.FindWindowW("VmChromaAppSessions", None)
    check(bool(main), "panel window opened")
    canvas = next(h for h in children(main) if cls(h) == "VmChromaAppSessionsCanvas")
    controls = children(canvas)
    sliders = [h for h in controls if cls(h) == "msctls_trackbar32" and text(h) == "vmtone volume"]
    check(len(sliders) == 12, "one slider per test session", str(len(sliders)))

    # Find the slider that belongs to our first tone: set it and see which session moved.
    target, mute = None, None
    for h in sliders:
        u.SendMessageW(h, TBM_SETPOS, 1, 96)
        u.SendMessageW(canvas, WM_HSCROLL, SB_THUMBPOSITION, h)
        time.sleep(0.2)
        if abs(mine(pid)["volume"] - 0.2512) < 0.002:
            target = h
            break
        u.SendMessageW(h, TBM_SETPOS, 1, 120)
        u.SendMessageW(canvas, WM_HSCROLL, SB_THUMBPOSITION, h)
    check(target is not None, "slider at -12 dB sets the session to 0.251 (-12 dB)")

    if target:
        siblings = controls[controls.index(target):]
        value = siblings[1]
        mute = siblings[2]
        check(text(value) == "-12.0 dB", "dB value reads -12.0 dB", text(value))
        for _ in range(2):
            u.PostMessageW(target, WM_KEYDOWN, VK_RIGHT, 0)
        time.sleep(0.5)
        check(mine(pid)["slider"] == 100 and text(value) == "-10.0 dB", "two Right arrows add 1 dB each", text(value))
        u.PostMessageW(target, WM_KEYDOWN, VK_END, 0)
        time.sleep(0.4)
        check(mine(pid)["volume"] == 0 and text(value) == "Off", "End turns the app off", text(value))
        u.PostMessageW(target, WM_KEYDOWN, VK_HOME, 0)
        time.sleep(0.4)
        check(abs(mine(pid)["volume"] - 1.0) < 1e-4 and text(value) == "0.0 dB", "Home returns to 0 dB", text(value))
        u.SendMessageW(mute, BM_CLICK, 0, 0)
        time.sleep(0.3)
        check(mine(pid)["mute"] is True, "Mute box mutes the session")
        u.SendMessageW(mute, BM_CLICK, 0, 0)
        time.sleep(0.3)
        check(mine(pid)["mute"] is False, "Mute box unmutes the session")

    # Scrolling: 12+ rows don't fit the default window, so the wheel must move the rows up.
    r0 = w.RECT()
    u.GetWindowRect(sliders[-1], c.byref(r0))
    u.SendMessageW(canvas, WM_MOUSEWHEEL, (-120 << 16) & 0xFFFFFFFF, 0)
    time.sleep(0.3)
    r1 = w.RECT()
    u.GetWindowRect(sliders[-1], c.byref(r1))
    check(r1.top < r0.top, "mouse wheel scrolls the list", f"{r0.top} -> {r1.top}")

    # Wheel back to the top: the first strip header must come back into view and stay there,
    # even though a row's Mute box has keyboard focus from the clicks above.
    u.SendMessageW(canvas, WM_MOUSEWHEEL, (120 * 10 << 16) & 0xFFFFFFFF, 0)
    time.sleep(0.6)
    def top(h):
        r = w.RECT()
        u.GetWindowRect(h, c.byref(r))
        return r.top
    header = min((h for h in children(canvas) if cls(h) == "Static" and "  –  " in text(h)), key=top)
    hr, cr = w.RECT(), w.RECT()
    u.GetWindowRect(header, c.byref(hr))
    u.GetWindowRect(canvas, c.byref(cr))
    check(cr.top <= hr.top < cr.top + 40, "scrolling to the top shows the first strip header", text(header))
    if args.shot:
        screenshot(main, args.shot)
finally:
    if panel:
        panel.terminate()
    for t in tones:
        t.terminate()
print("ALL PASSED" if failures == 0 else f"{failures} FAILED")
sys.exit(1 if failures else 0)
