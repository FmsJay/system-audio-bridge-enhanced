"""End-to-end test of vm-softphone.exe with no call, no mixer and nothing sent to JAMES-MAIN.

Runs the real helper with --stub-mixer (values held in memory), a temporary state file, and a UDP
listener on localhost standing in for the host. Checks the VBAN-Text bytes, the status block the banner
reads, a real hotkey press (F24, which nothing else uses), a taken hotkey, the menu messages, a stalled
reader, shutdown, and what happens when the mixer process dies.
"""
import ctypes as c
from ctypes import wintypes as w
import mmap
import os
from pathlib import Path
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time

BIN = Path(__file__).resolve().parent / "build-tools" / "Release"
u = c.windll.user32
k = c.windll.kernel32
u.FindWindowExW.argtypes = [w.HWND, w.HWND, w.LPCWSTR, w.LPCWSTR]
u.FindWindowExW.restype = w.HWND
u.PostMessageW.argtypes = [w.HWND, w.UINT, w.WPARAM, w.LPARAM]
HWND_MESSAGE = w.HWND(-3)
WM_APP = 0x8000
SET_AUTO, FLIP, SHUTDOWN = WM_APP + 10, WM_APP + 11, WM_APP + 12
F24, F23 = 0x87, 0x86
MIC_OPEN, UNKNOWN, BY_HAND, ARMED, AUTO, ALIVE, OPENED_AUTO, KEY_TAKEN, NOT_SET_UP, WAITING = [1 << i for i in range(10)]
LISTEN, TALK = "Strip[2].B3=0; Strip[2].Mute=1;", "Strip[2].B3=1; Strip[2].Mute=0;"
failures = 0


def check(ok, what, detail=""):
    global failures
    print(("ok  " if ok else "FAIL"), what, detail if not ok else "")
    failures += not ok


class Host:
    """Stands in for JAMES-MAIN: records every VBAN-Text datagram."""
    def __init__(self):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.bind(("127.0.0.1", 0))
        self.sock.settimeout(0.2)
        self.port = self.sock.getsockname()[1]
        self.packets = []
        self.stop = False
        threading.Thread(target=self.run, daemon=True).start()

    def run(self):
        while not self.stop:
            try:
                packet = self.sock.recv(2048)
                self.packets.append((time.perf_counter(), packet))
            except socket.timeout:
                pass

    def texts(self):
        return [p[28:].decode() for _, p in self.packets]

    def clear(self):
        self.packets.clear()


class Rig:
    def __init__(self, instance, *extra, parent=None, env=None):
        self.instance = instance
        self.dir = Path(tempfile.mkdtemp(prefix="softphone-test-"))
        self.state = self.dir / "live-state.txt"
        self.log = self.dir / "log.txt"
        self.host = Host()
        self.count = 100
        self.proc = subprocess.Popen(
            [str(BIN / "vm-softphone.exe"), "--stub-mixer", "--instance", instance, "--host", "127.0.0.1",
             "--port", str(self.host.port), "--state", str(self.state), "--log", str(self.log),
             "--parent", str(parent or os.getpid()), *extra], env=env)
        for _ in range(50):
            time.sleep(0.1)
            self.hwnd = u.FindWindowExW(HWND_MESSAGE, None, "VmChromaSoftphoneHelper" + instance, None)
            if self.hwnd:
                break
        self.shm = None
        for _ in range(50):
            try:
                self.shm = mmap.mmap(-1, 464, tagname="Local\\VmChromaSoftphoneStatus" + instance, access=mmap.ACCESS_READ)
                break
            except OSError:
                time.sleep(0.1)

    def status(self):
        raw = self.shm[:]
        # Layout: magic, version, sequence, flags, call, padding, heartbeat, hotkey[8], detail[200].
        magic, version, seq, flags, call = struct.unpack_from("<IIiII", raw, 0)
        heartbeat = struct.unpack_from("<Q", raw, 24)[0]
        hotkey = raw[32:48].decode("utf-16-le").split("\0")[0]
        detail = raw[48:448].decode("utf-16-le").split("\0")[0]
        return dict(magic=magic, seq=seq, flags=flags, call=call, heartbeat=heartbeat, hotkey=hotkey, detail=detail)

    def flags(self):
        return self.status()["flags"]

    def write(self, state, text="", settle=0.45):
        self.count += 1
        tmp = self.state.with_suffix(".tmp")
        tmp.write_text(f"{self.count}|{state}|{text}", encoding="utf8")
        os.replace(tmp, self.state)
        time.sleep(settle)

    def post(self, msg, wparam=0, settle=0.3):
        u.PostMessageW(self.hwnd, msg, wparam, 0)
        time.sleep(settle)

    def log_lines(self):
        return self.log.read_text(encoding="utf8", errors="replace").splitlines() if self.log.exists() else []

    def stop(self):
        if self.proc.poll() is None:
            self.post(SHUTDOWN, settle=0)
            try:
                self.proc.wait(5)
            except subprocess.TimeoutExpired:
                self.proc.kill()
        self.host.stop = True


def press(vk):
    u.keybd_event(vk, 0, 0, 0)
    u.keybd_event(vk, 0, 2, 0)
    time.sleep(0.35)


# ---- the normal flow
r = Rig("T1", "--hotkey", "F24")
time.sleep(1.5)
s = r.status()
check(s["magic"] == 0x53504831, "publishes a status block")
check(r.hwnd is not None and r.proc.poll() is None, "runs with a message-only window")
check(not r.flags() & (WAITING | NOT_SET_UP), "stub mixer counts as set up")
check(r.flags() & AUTO and not r.flags() & MIC_OPEN, "starts with automatic on and the mic off")
check(r.host.texts()[:2] == [LISTEN, LISTEN], "tells the host to listen at startup, twice", str(r.host.texts()))
beats = r.status()["heartbeat"]
time.sleep(0.5)
check(r.status()["heartbeat"] > beats, "heartbeat keeps moving")

packet = r.host.packets[0][1]
check(packet[:4] == b"VBAN" and packet[4] == 0x52 and packet[7] == 0x10, "VBAN-Text header matches vban-text.ps1")
check(packet[8:24] == b"Command1".ljust(16, b"\0"), "stream name is Command1 in 16 bytes")
check(packet[28:].decode() == LISTEN, "body is the host script")
check(struct.unpack_from("<I", r.host.packets[1][1], 24)[0] != struct.unpack_from("<I", packet, 24)[0], "repeat has its own frame number")

# Leftover file content must not open the mic: first read is ignored until the count changes.
r.state.write_text("500|person|left behind", encoding="utf8")
time.sleep(0.6)
check(not r.flags() & MIC_OPEN, "a leftover person line does not open the mic")
r.count = 500
r.host.clear()
r.write("call", "ringing now")
check(r.flags() & ARMED and not r.flags() & MIC_OPEN, "call arms the stream with the mic off")
check(r.status()["call"] == 2 and r.status()["detail"] == "ringing now", "status shows Ringing and the transcript", str(r.status()))
check(TALK not in r.host.texts(), "host stays in listen while ringing")
r.write("person", "hello? café")
check(r.flags() & MIC_OPEN and r.flags() & OPENED_AUTO, "person opens the mic")
check(r.status()["detail"] == "hello? café", "UTF-8 transcript survives", r.status()["detail"])
check(r.host.texts() == [TALK, TALK], "host told to talk, twice", str(r.host.texts()))
gaps = [b[0] - a[0] for a, b in zip(r.host.packets, r.host.packets[1:])]
check(all(0.05 < g < 0.35 for g in gaps), "repeat follows about 100 ms later", str(gaps))
order = [l for l in r.log_lines() if "Bus[7].Mute=0" in l or TALK in l]
check(order and TALK in order[0], "host told before the bus unmutes", str(order))

r.host.clear()
r.write("machine")
check(not r.flags() & MIC_OPEN and r.flags() & ARMED, "machine closes the mic, stream stays up")
lines = r.log_lines()
mute = max(i for i, l in enumerate(lines) if "Bus[7].Mute=1" in l)
listen = max(i for i, l in enumerate(lines) if LISTEN in l)
check(mute < listen, "bus muted before the host is told")

# ---- a real hotkey press
press(F24)
check(r.flags() & MIC_OPEN and r.flags() & BY_HAND, "a real F24 press opens the mic by hand")
r.write("machine", "still here")
r.write("person")
r.write("machine")
check(r.flags() & MIC_OPEN, "automatic leaves a hand-opened mic alone")
press(F24)
check(not r.flags() & MIC_OPEN, "a second press closes it")
r.write("idle")
check(not r.flags() & (BY_HAND | ARMED), "hang-up ends by-hand and stops the stream")

# ---- the menu's messages
r.post(FLIP)
check(r.flags() & MIC_OPEN, "the menu's flip works like the hotkey")
r.write("call")
check(not r.flags() & MIC_OPEN and not r.flags() & BY_HAND, "a new call closes a hand-opened mic")
r.post(SET_AUTO, 0)
check(not r.flags() & AUTO, "the menu can turn automatic off")
r.write("person")
check(not r.flags() & MIC_OPEN, "with automatic off, person does not open the mic")
r.post(SET_AUTO, 1)
r.write("idle")

# ---- monitor dies while the mic is open automatically
r.write("call")
r.write("person")
check(r.flags() & MIC_OPEN, "mic open before the monitor stops")
time.sleep(4.2)
check(r.flags() & MIC_OPEN, "still open after 4 s of silence")
time.sleep(1.5)
check(not r.flags() & MIC_OPEN and not r.flags() & ALIVE, "closed after 5 s of silence")

# ---- missing state file is just no data, and F24 still works
r.state.unlink()
time.sleep(0.5)
check(r.proc.poll() is None, "a missing state file doesn't stop it")
press(F24)
check(r.flags() & MIC_OPEN, "the hotkey works with no monitor")
time.sleep(6)
check(r.flags() & MIC_OPEN, "a hand-opened mic stays with no monitor")
press(F24)

# ---- shutdown restores listen mode
r.write("call")
r.write("person")
r.host.clear()
r.post(SHUTDOWN, settle=0)
try:
    r.proc.wait(5)
    exited = True
except subprocess.TimeoutExpired:
    exited = False
check(exited, "exits on the shutdown message")
time.sleep(0.3)
check(r.host.texts() == [LISTEN, LISTEN] or r.host.texts() == [LISTEN], "host told to listen on the way out", str(r.host.texts()))
lines = r.log_lines()
stream_off = max(i for i, l in enumerate(lines) if "vban.outstream[0].on=0" in l)
check(stream_off < max(i for i, l in enumerate(lines) if LISTEN in l), "stream off before the host is told", str(lines[-4:]))
check(any("stopped, confirmed" in l for l in lines), "the restore was read back and confirmed")
transcript_leaked = any("caf" in l or "hello" in l or "ringing now" in l for l in lines)
check(not transcript_leaked, "the transcript is never written to the log")
r.host.stop = True

# ---- the mixer dies: the host must still go back to listen
fake_mixer = subprocess.Popen([sys.executable, "-c", "import time; time.sleep(60)"])
r = Rig("T2", "--hotkey", "F24", parent=fake_mixer.pid)
time.sleep(1.5)
r.write("call")
r.write("call")
r.write("person")
check(r.flags() & MIC_OPEN, "mic open when the mixer dies")
r.host.clear()
fake_mixer.kill()
try:
    r.proc.wait(5)
    exited = True
except subprocess.TimeoutExpired:
    exited = False
time.sleep(0.2)
check(exited, "the helper leaves when the mixer dies")
check(r.host.texts() == [LISTEN, LISTEN], "and tells the host to listen first, twice", str(r.host.texts()))
r.host.stop = True

# ---- a taken hotkey is reported, and picked up once it's free
user32 = c.WinDLL("user32")
holder_done = threading.Event()
holder_got = []


def hold_key():
    holder_got.append(user32.RegisterHotKey(None, 77, 0, F23))
    holder_done.wait()
    user32.UnregisterHotKey(None, 77)


t = threading.Thread(target=hold_key, daemon=True)
t.start()
time.sleep(0.3)
r = Rig("T3", "--hotkey", "F23")
time.sleep(1.0)
check(holder_got == [1], "this test holds F23")
check(r.flags() & KEY_TAKEN, "a taken hotkey is flagged")
check(r.status()["hotkey"] == "F23", "the status names the key")
r.post(FLIP)
check(r.flags() & MIC_OPEN, "the menu still works with the key taken")
r.post(FLIP)
holder_done.set()
time.sleep(5.6)
check(not r.flags() & KEY_TAKEN, "the key is picked up once it's free")
r.stop()

# ---- not set up: nothing on the mixer is touched
env = dict(os.environ, VM_SOFTPHONE_STUB_NOT_SET_UP="1")
r = Rig("T4", "--hotkey", "F24", env=env)
time.sleep(1.5)
check(r.flags() & NOT_SET_UP, "bridge-not-set-up is reported")
check(not any("mixer " in l for l in r.log_lines()), "nothing on the mixer is touched", str(r.log_lines()))
check(r.host.texts() == [], "and nothing goes to the host", str(r.host.texts()))
r.post(FLIP)
check(not r.flags() & MIC_OPEN, "the mic can't open until it's set up")
r.stop()

# ---- only one helper per instance
a = Rig("T5", "--hotkey", "F24")
time.sleep(1.0)
second = subprocess.Popen([str(BIN / "vm-softphone.exe"), "--stub-mixer", "--instance", "T5", "--host", "127.0.0.1",
                           "--state", str(a.state)])
try:
    second.wait(3)
    check(second.returncode == 0, "a second helper exits at once")
except subprocess.TimeoutExpired:
    second.kill()
    check(False, "a second helper exits at once")
a.stop()
check(any("Bus[7].Mute=0" in l for l in a.log_lines()[-4:]), "exit with the mic closed unmutes the bus, as setup left it", str(a.log_lines()[-4:]))

# ---- a stalled share: opening the path succeeds and the read never returns
k.CreateNamedPipeW.restype = w.HANDLE
k.CreateNamedPipeW.argtypes = [w.LPCWSTR, w.DWORD, w.DWORD, w.DWORD, w.DWORD, w.DWORD, w.DWORD, c.c_void_p]
pipe_name = rf"\\.\pipe\softphone-stall-{os.getpid()}"
pipe = k.CreateNamedPipeW(pipe_name, 3, 0, 255, 4096, 4096, 0, None)
r = Rig("T6", "--hotkey", "F24", "--state", pipe_name)   # the later --state wins
time.sleep(1.5)
beats = r.status()["heartbeat"]
press(F24)
check(r.flags() & MIC_OPEN, "the hotkey works while the share is stalled")
time.sleep(0.5)
check(r.status()["heartbeat"] > beats, "status keeps publishing while the share is stalled")
press(F24)
check(not r.flags() & MIC_OPEN, "and closes again")
start = time.perf_counter()
r.post(SHUTDOWN, settle=0)
try:
    r.proc.wait(5)
    check(time.perf_counter() - start < 3, "exit doesn't wait for the stalled read")
except subprocess.TimeoutExpired:
    r.proc.kill()
    check(False, "exit doesn't wait for the stalled read")
r.host.stop = True
k.CloseHandle(pipe)

print("ALL PASSED" if failures == 0 else f"{failures} FAILED")
sys.exit(1 if failures else 0)
