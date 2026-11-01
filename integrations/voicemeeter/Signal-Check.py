"""Known-signal checks against the running Potato mixer.

The Remote API level meters (VBVMR_GetLevel) read 0 on every channel with the Chroma trial even
while audio flows, so these checks record the B buses through their "Voicemeeter Out Bn" Windows
recording endpoints instead. "Voicemeeter Out A1-A5" and "Voicemeeter In 1-5" belong to the
separately licensed VAIO extension and carry nothing until it is activated; with it, Strip[n].VAIO
feeds "Voicemeeter In n+1" into hardware strip n and Bus[n].VAIO publishes bus An on "Out An".

  buses    Record B1-B3 for a second each and report that the engine is delivering audio.
  proof    Milestone 1 known signal and milestone 4 isolation without the extension. Adds A2 and B3
           to AUX Input (its A1 stays on), plays 700 Hz into "Voicemeeter AUX Input" and 1300 Hz
           into "Voicemeeter Input" together, and records B3: the AUX tone must arrive and the
           Voicemeeter Input tone (still A1 only) must not. Refused on a softphone bridge mixer,
           where B3 carries the mic to the call; use proof-a there.
  proof-a  The same on hardware buses, with the extension: feeds "Voicemeeter In 5" into the free
           Stereo Input 5 and routes it first to A1, then to A3 only, while "Voicemeeter Input"
           stays on A1. Out A1 and Out A3 are recorded at the same time in each pass. Refused if
           Stereo Input 5 is in use. The tones are faintly audible, so don't run it during a call.
Everything changed is restored afterwards, even on error.
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
sys.path.insert(0, str(HERE))
import Mixer_Guard  # noqa: E402
import Profiles  # noqa: E402  (same DLL; Mixer_Guard reads through it)


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


def meters_together(endpoints, seconds, *freqs):
    """Record several endpoints over the same window."""
    procs = {e: subprocess.Popen([str(VMTONE), "--meter", e, str(seconds), *map(str, freqs)],
                                 stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True) for e in endpoints}
    result = {}
    for e, p in procs.items():
        text = p.communicate(timeout=60)[0]
        result[e] = {int(float(h)): float(v) for h, v in re.findall(r"hz ([\d.]+): (-?[\d.]+) dBFS", text)}
    return result


def run_proof_a(dbfs):
    strip, in_ep, other_ep = 4, "Voicemeeter In 5", OTHER_ENDPOINT
    out_a1, out_a3 = "Voicemeeter Out A1", "Voicemeeter Out A3"
    keys = [f"Strip[{strip}].{k}" for k in BUSES + ["VAIO", "Mute", "Gain"]] + ["Bus[0].VAIO", "Bus[2].VAIO"]
    saved = {k: get(k) for k in keys}
    other = routes(OTHER_STRIP)
    print(f"Voicemeeter Input routes: {other}")
    if "A1" not in other or "A3" in other:
        raise RuntimeError("Needs Voicemeeter Input on A1 and off A3; routing differs.")
    passes = {}
    try:
        for k in ("Bus[0].VAIO", "Bus[2].VAIO", f"Strip[{strip}].VAIO"):
            set_param(k, 1.0)
        set_param(f"Strip[{strip}].Mute", 0.0)
        set_param(f"Strip[{strip}].Gain", 0.0)
        for target in ("A1", "A3"):
            for b in BUSES:
                set_param(f"Strip[{strip}].{b}", 1.0 if b == target else 0.0)
            time.sleep(0.8)
            if get(f"Strip[{strip}].VAIO") < 0.5:
                raise RuntimeError("Strip VAIO was refused: the VAIO extension licence is not active.")
            players = [subprocess.Popen([str(VMTONE), ep, str(dbfs), "1.8", str(hz)], stdout=subprocess.DEVNULL)
                       for ep, hz in ((in_ep, 700), (other_ep, 1300))]
            time.sleep(0.5)
            passes[f"In 5 -> {target}"] = meters_together([out_a1, out_a3], 0.8, 700, 1300)
            for p in players:
                p.wait(timeout=30)
    finally:
        for k, v in saved.items():
            set_param(k, v)
        time.sleep(0.8)
        restored = {k: get(k) for k in keys} == saved

    # Out An is post-fader, so a tone should arrive at its level plus that bus's fader, within 4 dB.
    fader = {out_a1: get("Bus[0].Gain"), out_a3: get("Bus[2].Gain")}

    def present(db, out):
        return abs(db - (dbfs + fader[out])) < 4

    def absent(db):
        return db < -70

    a1, a3 = passes["In 5 -> A1"], passes["In 5 -> A3"]
    checks = {
        "In 5 on A1 reaches Out A1": present(a1[out_a1][700], out_a1),
        "In 5 on A1 not on Out A3": absent(a1[out_a3][700]),
        "In 5 on A3 reaches Out A3": present(a3[out_a3][700], out_a3),
        "In 5 on A3 leaves Out A1": absent(a3[out_a1][700]),
        "Voicemeeter Input stays on Out A1": present(a1[out_a1][1300], out_a1) and present(a3[out_a1][1300], out_a1),
        "Voicemeeter Input never on Out A3": absent(a1[out_a3][1300]) and absent(a3[out_a3][1300]),
        "restored": restored,
    }
    summary = {"bus_faders_db": fader, "levels_db": passes, "checks": checks, "pass": all(checks.values())}
    print(json.dumps(summary, indent=1))
    return summary


parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
parser.add_argument("mode", choices=["buses", "proof", "proof-a"])
parser.add_argument("--dbfs", type=float, default=-40.0)
parser.add_argument("--json", type=Path, help="Also save the result here (outside the repository).")
args = parser.parse_args()
if not VMTONE.exists():
    sys.exit("Build the tools first: tools\\Build-Tools.ps1")
if remote.VBVMR_Login() != 0:
    sys.exit("A running Voicemeeter mixer is required.")
try:
    time.sleep(0.5)
    if args.mode == "proof" and Mixer_Guard.bridge_configured(Profiles):
        sys.exit("Refusing: on this mixer B3 carries the mic to the softphone call. Use proof-a.")
    if args.mode == "proof-a":
        Mixer_Guard.require_free_strip(Profiles, 4)
    result = {"buses": run_buses, "proof": lambda: run_proof(args.dbfs), "proof-a": lambda: run_proof_a(args.dbfs)}[args.mode]()
    if args.json:
        args.json.write_text(json.dumps(result, indent=1))
finally:
    remote.VBVMR_Logout()
if isinstance(result, dict) and result.get("pass") is False:
    sys.exit(1)
