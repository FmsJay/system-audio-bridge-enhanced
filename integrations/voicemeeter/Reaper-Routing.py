"""REAPER <-> Voicemeeter record/return and insert presets, with feedback checks.

  status                       REAPER's live audio device, how it maps onto Voicemeeter, and
                               whether that routing loops back into itself
  list                         the presets in reaper/routing-presets.json (explicit channels)
  check [ID ...]               check presets against the live mixer and REAPER (default: all)
  apply ID --yes               set the Voicemeeter side of a preset; saves a Profiles.py rollback
                               snapshot first (insert presets: --off turns the insert back off)

REAPER's own device is never changed: a preset says what to choose in REAPER's preferences.
The live device is read by running reaper/AudioDevice-Info.lua in the open REAPER; when REAPER
isn't running, REAPER.ini is read instead. Use --presets FILE for your own preset list.
"""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import time

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import Mixer_Guard  # noqa: E402
import Profiles as P  # noqa: E402  (shares its Remote API access and rollback snapshots)

BUSES = ["A1", "A2", "A3", "A4", "A5", "B1", "B2", "B3"]
STRIP_ENDPOINTS = {5: "Voicemeeter Input", 6: "Voicemeeter AUX Input", 7: "Voicemeeter VAIO3 Input"}
# VAIO extension: "Voicemeeter In n" feeds hardware strip n-1 while that strip's VAIO switch is on.
STRIP_ENDPOINTS.update({n - 1: f"Voicemeeter In {n}" for n in range(1, 6)})
INSERT_DRIVER = "Voicemeeter Potato Insert Virtual ASIO"
REAPER_INI = Path(r"D:\Portable Workstation\DAW\REAPER\REAPER.ini")


def insert_channels(strip):
    """1-based Potato insert channels: hardware strips are stereo (1-10), virtual strips 8-ch (11-34)."""
    return [2 * strip + 1, 2 * strip + 2] if strip < 5 else list(range(11 + 8 * (strip - 5), 19 + 8 * (strip - 5)))


def reaper_device():
    """REAPER's live device via the info script, else the device saved in REAPER.ini."""
    running = subprocess.run(["powershell", "-NoProfile", "-Command", "(Get-Process reaper -ErrorAction SilentlyContinue).Path"],
                             capture_output=True, text=True).stdout.strip().splitlines()
    info_file = Path(os.environ["TEMP"]) / "reaper-audioinfo.txt"
    if running:
        info_file.unlink(missing_ok=True)
        subprocess.Popen([running[0], "-nonewinst", str(HERE / "reaper" / "AudioDevice-Info.lua")])
        for _ in range(40):
            time.sleep(0.25)
            if info_file.exists() and info_file.read_text(encoding="utf-8", errors="replace").endswith("\n"):
                info = dict(line.split("=", 1) for line in info_file.read_text(encoding="utf-8", errors="replace").splitlines() if "=" in line)
                info["source"] = "live REAPER"
                return info
    text = REAPER_INI.read_text(encoding="utf-8", errors="replace") if REAPER_INI.exists() else ""
    ini = dict(re.findall(r"^(\w+)=(.*)$", text, re.M))
    return {"source": "REAPER.ini (REAPER not running)", "MODE": "unknown (mode=" + ini.get("mode", "?") + ")",
            "IDENT_IN": ini.get("asio_driver_name", "").strip('"'), "IDENT_OUT": "",
            "WASAPI_IN": ini.get("wasapi_driver_in", "").strip('"'), "WASAPI_OUT": ini.get("wasapi_driver_out", "").strip('"')}


def voicemeeter_role(device_name):
    """What a REAPER device name is on the Voicemeeter side: ('bus', 'B1') or ('strip', 6) or None."""
    m = re.search(r"Voicemeeter Out ([AB]\d)", device_name or "")
    if m:
        return ("bus", m.group(1))
    for strip, endpoint in STRIP_ENDPOINTS.items():
        if (device_name or "").startswith(endpoint + " ") or device_name == endpoint:
            return ("strip", strip)
    return None


def routes(strip):
    return [b for b in BUSES if P.get_float(f"Strip[{strip}].{b}") > 0.5]


def bus_has_device(bus):
    if bus.startswith("B"):
        return True
    return bool(P.get_string(f"Bus[{BUSES.index(bus)}].device.name"))


def check(preset, device):
    """(problems, warnings, notes) for one preset against the live mixer and REAPER."""
    problems, warnings, notes = [], [], []
    kind = preset["kind"]
    live_in = voicemeeter_role(device.get("IDENT_IN", "").replace("WASAPI ", ""))
    if kind in ("bridge-send", "device"):
        strip = preset["strip"]
        r = routes(strip)
        notes.append(f"strip {strip + 1} routes to {r or 'nothing'}")
        if not any(bus_has_device(b) for b in r if b.startswith("A")):
            problems.append(f"strip {strip + 1} reaches no hardware output, so the return is silent")
        missing = [b for b in preset.get("hear_on", []) if b not in r]
        if missing:
            warnings.append(f"strip {strip + 1} is not routed to {missing}")
        busy = Mixer_Guard.vban_user(P, strip)
        if busy:
            problems.append(f"strip {strip + 1} is taken: {busy}")
        if strip < 5 and P.get_float(f"Strip[{strip}].VAIO") < 0.5:
            problems.append(f"strip {strip + 1}'s VAIO switch is off, so {preset.get('endpoint')} doesn't reach it")
        loop_bus = preset.get("recorded_bus") or (live_in[1] if live_in and live_in[0] == "bus" else None)
        if loop_bus and loop_bus in r:
            problems.append(f"FEEDBACK: strip {strip + 1} routes to {loop_bus}, which REAPER records")
    if kind == "device":
        mode = device.get("MODE", "")
        active = mode == preset["driver"] and preset["input"] in device.get("IDENT_IN", "") \
            and preset["output"] in device.get("IDENT_OUT", "")
        notes.append("active in REAPER now" if active else
                     f"not active (REAPER is on {device.get('IDENT_IN') or mode}); {preset['reaper']}")
        notes.append(f"REAPER inputs {preset['input_channels']} <- {preset['input']}; outputs {preset['output_channels']} -> {preset['output']}")
    if kind == "bridge-capture":
        notes.append(f"captures the {preset['endpoint']} endpoint (whole device) before strip {preset['strip'] + 1}")
        if preset["endpoint"] in device.get("IDENT_OUT", ""):
            problems.append("FEEDBACK: REAPER also outputs to this endpoint, so the capture records REAPER itself")
    if kind == "insert":
        chans = insert_channels(preset["strip"])
        on = [c for c in chans if P.get_float(f"Patch.insert[{c - 1}]") > 0.5]
        on_insert = device.get("MODE") == "ASIO" and INSERT_DRIVER in device.get("IDENT_IN", "")
        notes.append(f"insert channels {chans[0]}-{chans[-1]}, currently {'on' if on else 'off'}")
        if on and not on_insert:
            problems.append(f"insert is on but REAPER is not on {INSERT_DRIVER}: strip {preset['strip'] + 1} is silent")
        if not on_insert:
            notes.append(f"to use: {preset['reaper']}")
    return problems, warnings, notes


def report(preset, device):
    problems, warnings, notes = check(preset, device)
    state = "PROBLEM" if problems else ("warning" if warnings else "ok")
    print(f"[{state}] {preset['id']}: {preset['title']}")
    for line in problems + warnings + notes:
        print(f"    {line}")
    return not problems


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--presets", type=Path, default=HERE / "reaper" / "routing-presets.json")
    sub = parser.add_subparsers(dest="command", required=True)
    sub.add_parser("status")
    sub.add_parser("list")
    p = sub.add_parser("check"); p.add_argument("ids", nargs="*")
    p = sub.add_parser("apply"); p.add_argument("id"); p.add_argument("--yes", action="store_true"); p.add_argument("--off", action="store_true")
    args = parser.parse_args()
    presets = {p["id"]: p for p in json.loads(args.presets.read_text(encoding="utf-8"))["presets"]}

    if args.command == "list":
        for p in presets.values():
            extra = f"  insert channels {insert_channels(p['strip'])[0]}-{insert_channels(p['strip'])[-1]}" if p["kind"] == "insert" else ""
            print(f"{p['id']} ({p['kind']}): {p['title']}{extra}\n    REAPER: {p['reaper']}\n    Voicemeeter: {p['voicemeeter']}")
        return 0

    device = reaper_device()
    P.connect()
    try:
        if args.command == "status":
            print(f"REAPER ({device['source']}): {device.get('MODE')} in '{device.get('IDENT_IN')}' out '{device.get('IDENT_OUT')}'"
                  f" at {device.get('SRATE', '?')} Hz, {device.get('BSIZE', '?')} samples, output latency {device.get('OUTPUT_LATENCY_MS', '?')} ms")
            for k in sorted(k for k in device if re.match(r"(IN|OUT)\d+$", k)):
                print(f"    {k.lower()}: {device[k]}")
            roles = [voicemeeter_role(device.get(k, "")) for k in ("IDENT_IN", "IDENT_OUT")]
            if not any(roles):
                print("REAPER's device isn't a Voicemeeter endpoint, so it can't loop through the mixer. "
                      "Use a bridge-* preset to reach Voicemeeter from REAPER.")
                return 0
            if roles[0] and roles[1] and roles[0][0] == "bus" and roles[1][0] == "strip":
                looped = roles[0][1] in routes(roles[1][1])
                print(("FEEDBACK: " if looped else "No feedback: ") + f"REAPER records {roles[0][1]} and returns into strip "
                      f"{roles[1][1] + 1}, which routes to {routes(roles[1][1])}")
                return 1 if looped else 0
            return 0

        if args.command == "check":
            chosen = args.ids or list(presets)
            unknown = [i for i in chosen if i not in presets]
            if unknown:
                sys.exit(f"Unknown preset(s): {unknown}")
            ok = [report(presets[i], device) for i in chosen]
            return 0 if all(ok) else 1

        if args.command == "apply":
            preset = presets.get(args.id) or sys.exit(f"Unknown preset {args.id}")
            busy = Mixer_Guard.vban_user(P, preset["strip"]) if "strip" in preset else None
            if busy:
                sys.exit(f"Refusing: strip {preset['strip'] + 1} is taken: {busy}. Nothing was changed.")
            changes = []
            if preset["kind"] in ("bridge-send", "device"):
                strip = preset["strip"]
                if strip < 5:
                    changes.append((f"Strip[{strip}].VAIO", 1.0))   # "Voicemeeter In n" -> hardware strip
                for bus in preset.get("hear_on", []):
                    changes.append((f"Strip[{strip}].{bus}", 1.0))
                if preset.get("recorded_bus"):
                    changes.append((f"Strip[{strip}].{preset['recorded_bus']}", 0.0))
            elif preset["kind"] == "insert":
                if not args.off and not (device.get("MODE") == "ASIO" and INSERT_DRIVER in device.get("IDENT_IN", "")):
                    sys.exit(f"Refusing: REAPER is not on {INSERT_DRIVER}, so turning the insert on would silence "
                             f"strip {preset['strip'] + 1}. {preset['reaper']}")
                changes = [(f"Patch.insert[{c - 1}]", 0.0 if args.off else 1.0) for c in insert_channels(preset["strip"])]
            else:
                print("Nothing to set in Voicemeeter for this preset. In REAPER: " + preset["reaper"])
                return 0
            changes = [(k, v) for k, v in changes if abs(P.get_float(k) - v) > 0.01]
            if not changes:
                print("Voicemeeter already matches this preset.")
                return 0 if report(preset, device) else 1
            for k, v in changes:
                print(f"  {k} -> {v:g}")
            if not args.yes:
                print("Preview only; add --yes to apply.")
                return 0
            rollback = P.write_rollback()
            for k, v in changes:
                P.set_float(k, v)
            time.sleep(0.6)
            print(f"Applied. Rollback snapshot (strip/bus routing): {rollback}")
            return 0 if report(preset, device) else 1
    finally:
        P.remote.VBVMR_Logout()


if __name__ == "__main__":
    sys.exit(main())
