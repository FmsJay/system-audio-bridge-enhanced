"""Known-signal checks against the running Potato mixer.

The Remote API level meters (VBVMR_GetLevel) read 0 on every channel with the Chroma trial even
while audio flows, so these checks record the B buses through their "Voicemeeter Out Bn" Windows
recording endpoints instead. "Voicemeeter Out A1-A5" and "Voicemeeter In 1-5" belong to the
separately licensed VAIO extension and carry nothing until it is activated, so A buses cannot be
recorded here.

  buses  Record B1-B3 for a second each and report that the engine is delivering audio.
  proof  Milestone 1 known signal and milestone 4 isolation. Adds A2 and B3 to AUX Input (its
         existing A1 stays on), plays 700 Hz into "Voicemeeter AUX Input" and 1300 Hz into
         "Voicemeeter Input" at the same time, and records B3: the AUX tone must arrive and the
         Voicemeeter Input tone (still A1 only) must not. AUX routing is restored afterwards, even
         on error. Anything else playing on AUX reaches B3 for those ~2 s too.
"""
import argparse
import ctypes
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import time

HERE = Path(__file__).resolve().parent
VMTONE = HERE / "build-tools" / "Release" / "vmtone.exe"
DLL = Path(os.environ["ProgramFiles(x86)"]) / "VB/Voicemeeter/VoicemeeterRemote64.dll"
BUSES = ["A1", "A2", "A3", "A4", "A5", "B1", "B2", "B3"]
PROOF_STRIP = 6                       # Voicemeeter AUX
PROOF_ENDPOINT = "Voicemeeter AUX Input"
PROOF_ADDED = ("A2", "B3")
OTHER_ENDPOINT = "Voicemeeter Input ("
OTHER_STRIP = 5

remote = ctypes.WinDLL(str(DLL))
remote.VBVMR_GetParameterFloat.argtypes = [ctypes.c_char_p, ctypes.POINTER(ctypes.c_float)]
remote.VBVMR_SetParameterFloat.argtypes = [ctypes.c_char_p, ctypes.c_float]


def get(name):
    remote.VBVMR_IsParametersDirty()
    value = ctypes.c_float()
    if remote.VBVMR_GetParameterFloat(name.encode(), ctypes.byref(value)) != 0:
        raise RuntimeError(f"cannot read {name}")
    return value.value


def set_param(name, value):
    if remote.VBVMR_SetParameterFloat(name.encode(), ctypes.c_float(value)) != 0:
        raise RuntimeError(f"cannot set {name}")


def routes(strip):
    return [b for b in BUSES if get(f"Strip[{strip}].{b}") > 0.5]


def run(*args):
    out = subprocess.run([str(VMTONE), *map(str, args)], capture_output=True, text=True, timeout=60)
    return out.returncode, (out.stdout + out.stderr).strip()


def meter(endpoint, seconds, *freqs):
    code, text = run("--meter", endpoint, seconds, *freqs)
    if code != 0:
        raise RuntimeError(text)
    peak = float(re.search(r"peak (-?[\d.]+) dBFS", text).group(1))
    tones = {int(float(h)): float(v) for h, v in re.findall(r"hz ([\d.]+): (-?[\d.]+) dBFS", text)}
    frames = int(re.search(r": (\d+) frames", text).group(1))
    return {"frames": frames, "peak_db": peak, "tones_db": tones, "raw": text}


def run_buses():
    result = {}
    for b in ("B1", "B2", "B3"):
        m = meter(f"Voicemeeter Out {b}", 1)
        result[b] = {k: m[k] for k in ("frames", "peak_db")}
        print(f"{b}: {m['frames']} frames, peak {m['peak_db']} dBFS")
    return result


def run_proof(dbfs):
    keys = [f"Strip[{PROOF_STRIP}].{k}" for k in BUSES]
    saved = {k: get(k) for k in keys}
    other = routes(OTHER_STRIP)
    print(f"AUX Input routes before: {routes(PROOF_STRIP)}; Voicemeeter Input routes: {other}")
    if "B3" in other or get(f"Strip[{PROOF_STRIP}].Mute") > 0.5:
        raise RuntimeError("Needs Voicemeeter Input off B3 and AUX Input unmuted; routing differs.")
    baseline = meter("Voicemeeter Out B3", 1, 700, 1300)
    try:
        for b in PROOF_ADDED:
            set_param(f"Strip[{PROOF_STRIP}].{b}", 1.0)
        time.sleep(0.3)
        during_routes = routes(PROOF_STRIP)
        a = subprocess.Popen([str(VMTONE), PROOF_ENDPOINT, str(dbfs), "1.6", "700"],
                             stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        b = subprocess.Popen([str(VMTONE), OTHER_ENDPOINT, str(dbfs), "1.6", "1300"],
                             stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        time.sleep(0.4)
        during = meter("Voicemeeter Out B3", 0.8, 700, 1300)
        tones = [p.communicate(timeout=30)[0].strip() for p in (a, b)]
    finally:
        for k, v in saved.items():
            set_param(k, v)
        time.sleep(0.4)
        restored = {k: get(k) for k in keys}
    # The tone should arrive within a few dB of its level; anything under -70 dBFS is noise floor.
    arrived = during["tones_db"][700] > dbfs - 6
    leaked = during["tones_db"][1300] > -70
    summary = {
        "tone_players": tones,
        "aux_input_routes_during": during_routes,
        "voicemeeter_input_routes": other,
        "b3_baseline_db": baseline["tones_db"],
        "b3_during_db": during["tones_db"],
        "aux_tone_reached_b3": arrived,
        "voicemeeter_input_tone_leaked_to_b3": leaked,
        "restored": restored == saved,
        "pass": arrived and not leaked and restored == saved,
    }
    print(json.dumps(summary, indent=1))
    return summary


parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
parser.add_argument("mode", choices=["buses", "proof"])
parser.add_argument("--dbfs", type=float, default=-40.0)
parser.add_argument("--json", type=Path, help="Also save the result here (outside the repository).")
args = parser.parse_args()
if not VMTONE.exists():
    sys.exit("Build the tools first: tools\\Build-Tools.ps1")
if remote.VBVMR_Login() != 0:
    sys.exit("A running Voicemeeter mixer is required.")
try:
    time.sleep(0.5)
    result = run_buses() if args.mode == "buses" else run_proof(args.dbfs)
    if args.json:
        args.json.write_text(json.dumps(result, indent=1))
finally:
    remote.VBVMR_Logout()
if isinstance(result, dict) and result.get("pass") is False:
    sys.exit(1)
