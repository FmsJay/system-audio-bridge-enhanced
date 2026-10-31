"""Milestone 5: measure plugin cost, bypass, dropouts, latency and dry recovery in the open REAPER.

Runs reaper/Measure-FX.lua in REAPER (a temporary project tab with a silent track, your project
is left alone) and samples REAPER's process CPU from outside while it steps through its phases.

  python Measure-FX.py [--fx "ReaEQ (Cockos)" --fx "ReaComp (Cockos)"] [--instances 8] [--seconds 6]
                       [--json results.json]

CPU is REAPER's whole process, as a percentage of one core, averaged over each phase (the first
second after a change is skipped). Xruns are REAPER's audio (yellow) and media (red) underruns.
"""
import argparse
import ctypes
from ctypes import wintypes
import json
import os
from pathlib import Path
import subprocess
import sys
import time

HERE = Path(__file__).resolve().parent
TEMP = Path(os.environ["TEMP"])
k32 = ctypes.windll.kernel32
k32.OpenProcess.restype = wintypes.HANDLE
k32.GetProcessTimes.argtypes = [wintypes.HANDLE] + [ctypes.POINTER(wintypes.FILETIME)] * 4


def reaper_process():
    out = subprocess.run(["powershell", "-NoProfile", "-Command",
                          "Get-Process reaper -ErrorAction SilentlyContinue | Select-Object -First 1 | "
                          "ForEach-Object { \"$($_.Id)|$($_.Path)\" }"], capture_output=True, text=True).stdout.strip()
    if not out:
        sys.exit("REAPER is not running.")
    pid, path = out.split("|", 1)
    return int(pid), path


def cpu_seconds(handle):
    times = [wintypes.FILETIME() for _ in range(4)]
    if not k32.GetProcessTimes(handle, *[ctypes.byref(t) for t in times]):
        raise OSError("GetProcessTimes failed")
    kernel, user = times[2], times[3]
    return ((kernel.dwHighDateTime << 32 | kernel.dwLowDateTime) + (user.dwHighDateTime << 32 | user.dwLowDateTime)) / 1e7


parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
parser.add_argument("--fx", action="append", help="FX name as REAPER's FX browser shows it")
parser.add_argument("--instances", type=int, default=8)
parser.add_argument("--seconds", type=float, default=6)
parser.add_argument("--json", type=Path)
args = parser.parse_args()
fx = args.fx or ["ReaEQ (Cockos)", "ReaComp (Cockos)", "ReaVerbate (Cockos)"]

pid, exe = reaper_process()
(TEMP / "vm-measure-config.txt").write_text(
    "\n".join([f"fx={f}" for f in fx] + [f"instances={args.instances}", f"seconds={args.seconds}"]) + "\n", encoding="utf-8")
log = TEMP / "vm-measure-log.txt"
log.unlink(missing_ok=True)
handle = k32.OpenProcess(0x1000, False, pid)   # PROCESS_QUERY_LIMITED_INFORMATION
subprocess.Popen([exe, "-nonewinst", str(HERE / "reaper" / "Measure-FX.lua")])

samples, phase, phase_start, lines_seen = {}, None, 0.0, 0
last_cpu, last_t = cpu_seconds(handle), time.time()
deadline = time.time() + 30 + args.seconds * 8
results, setup, error = [], "", None
while time.time() < deadline:
    time.sleep(0.25)
    now, cpu = time.time(), cpu_seconds(handle)
    if phase and now - phase_start > 1.0:
        samples.setdefault(phase, []).append(100.0 * (cpu - last_cpu) / (now - last_t))
    last_cpu, last_t = cpu, now
    lines = log.read_text(encoding="utf-8", errors="replace").splitlines() if log.exists() else []
    for line in lines[lines_seen:]:
        parts = line.split("\t")
        if line.startswith("phase\t"):
            phase, phase_start = parts[1], now
        elif line.startswith("result\t"):
            name, xa, xm, latency, pdc = parts[1], int(parts[2]), int(parts[3]), float(parts[4]), int(parts[5])
            cpu_list = samples.get(name, [])
            results.append({"phase": name, "cpu_avg_pct": round(sum(cpu_list) / len(cpu_list), 1) if cpu_list else None,
                            "cpu_peak_pct": round(max(cpu_list), 1) if cpu_list else None,
                            "audio_xruns": xa, "media_xruns": xm, "output_latency_ms": latency,
                            "fx_pdc_samples": pdc if pdc >= 0 else None})
        elif line.startswith("setup"):
            setup = line
        elif line.startswith("error"):
            error = line[6:]
    lines_seen = len(lines)
    if "done" in lines:
        break
k32.CloseHandle(handle)

if error:
    sys.exit(f"REAPER reported: {error}")
if not results:
    sys.exit("No results; is ReaScript allowed to run, and did REAPER show an error?")
print(f"{setup}  ({args.instances} x {', '.join(fx)})")
print(f"{'phase':30} {'CPU avg':>8} {'peak':>7} {'xruns':>6} {'latency':>9} {'PDC':>6}")
for r in results:
    cpu_text = f"{r['cpu_avg_pct']:.1f}%" if r["cpu_avg_pct"] is not None else "-"
    peak_text = f"{r['cpu_peak_pct']:.1f}%" if r["cpu_peak_pct"] is not None else "-"
    print(f"{r['phase']:30} {cpu_text:>8} {peak_text:>7} {r['audio_xruns'] + r['media_xruns']:>6} "
          f"{r['output_latency_ms']:>7.2f}ms {r['fx_pdc_samples'] if r['fx_pdc_samples'] is not None else '-':>6}")
if args.json:
    args.json.write_text(json.dumps({"setup": setup, "fx": fx, "instances": args.instances, "phases": results}, indent=1))
