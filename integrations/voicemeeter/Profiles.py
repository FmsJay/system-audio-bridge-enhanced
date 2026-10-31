"""Versioned Voicemeeter Potato profiles with per-machine hardware mapping, preview and rollback.

A profile holds strip/bus routing, levels, labels and hardware device choices. Devices are stored
by *logical* name ("Headset", "Speakers"), and each machine maps logical names to its own device
names in devices.json, so one profile works on both PCs. Profiles are applied through the Remote
API one parameter at a time, never through the mixer's native settings Load, and every apply first
saves a rollback snapshot.

  save NAME [--note TEXT]             save the live mixer as the next version of NAME
  list                                profiles, versions, and version clashes between machines
  preview NAME [--version N]          what apply would change here, and any device conflicts
  apply NAME [--version N] --yes      apply (with --only Strip[3] Bus[1] ... to limit it)
  rollback [FILE] --yes               re-apply the latest (or given) rollback snapshot
  map LOGICAL "DEVICE NAME"           map a logical device name on this machine
  map                                 show this machine's mapping and the devices it can see

The store defaults to %VM_PROFILES% or Documents\\Voicemeeter\\profiles. Profiles contain private
machine configuration: keep the store out of Git.
"""
import argparse
import ctypes
import datetime
import json
import os
from pathlib import Path
import re
import socket
import sys
import time

FORMAT = 1
HOST = socket.gethostname().upper()
STRIP_FLOATS = ["Gain", "Mute", "Solo", "Mono", "A1", "A2", "A3", "A4", "A5", "B1", "B2", "B3"]
BUS_FLOATS = ["Gain", "Mute", "Mono"]
# VAIO extension switches: Strip[n].VAIO feeds "Voicemeeter In n+1" into hardware strip n,
# Bus[n].VAIO publishes A(n+1) on "Voicemeeter Out A(n+1)". Without the licence they read 0.
VAIO_SLOTS = [f"Strip[{i}].VAIO" for i in range(5)] + [f"Bus[{i}].VAIO" for i in range(5)]
HARDWARE = [f"Strip[{i}]" for i in range(5)] + [f"Bus[{i}]" for i in range(5)]
DEVICE_KINDS = {1: "mme", 3: "wdm", 4: "ks", 5: "asio"}

remote = ctypes.WinDLL(str(Path(os.environ["ProgramFiles(x86)"]) / "VB/Voicemeeter/VoicemeeterRemote64.dll"))
remote.VBVMR_GetParameterFloat.argtypes = [ctypes.c_char_p, ctypes.POINTER(ctypes.c_float)]
remote.VBVMR_SetParameterFloat.argtypes = [ctypes.c_char_p, ctypes.c_float]
remote.VBVMR_GetParameterStringW.argtypes = [ctypes.c_char_p, ctypes.c_wchar_p]
remote.VBVMR_SetParameterStringW.argtypes = [ctypes.c_char_p, ctypes.c_wchar_p]
for fn in (remote.VBVMR_Input_GetDeviceDescW, remote.VBVMR_Output_GetDeviceDescW):
    fn.argtypes = [ctypes.c_long, ctypes.POINTER(ctypes.c_long), ctypes.c_wchar_p, ctypes.c_wchar_p]


# ---------------------------------------------------------------- mixer access
def connect():
    if remote.VBVMR_Login() < 0:
        sys.exit("Remote API login failed.")
    kind = ctypes.c_long()
    for _ in range(40):
        if remote.VBVMR_GetVoicemeeterType(ctypes.byref(kind)) == 0:
            if kind.value != 3:
                sys.exit("Profiles are for Voicemeeter Potato.")
            remote.VBVMR_IsParametersDirty()
            time.sleep(0.3)
            return
        time.sleep(0.5)
    sys.exit("No running mixer answered the Remote API.")


def refresh():
    """The Remote API caches parameters and updates the cache on the poll *after* the one that
    reports a change, so poll until the dirty flag clears before reading."""
    for _ in range(20):
        if remote.VBVMR_IsParametersDirty() <= 0:
            return
        time.sleep(0.01)


def get_float(name):
    refresh()
    value = ctypes.c_float()
    if remote.VBVMR_GetParameterFloat(name.encode(), ctypes.byref(value)) != 0:
        raise RuntimeError(f"Cannot read {name}")
    return round(value.value, 3)


def get_string(name):
    refresh()
    text = ctypes.create_unicode_buffer(512)
    if remote.VBVMR_GetParameterStringW(name.encode(), text) != 0:
        raise RuntimeError(f"Cannot read {name}")
    return text.value


def set_float(name, value):
    if remote.VBVMR_SetParameterFloat(name.encode(), ctypes.c_float(value)) != 0:
        raise RuntimeError(f"Mixer refused {name} = {value}")


def set_string(name, value):
    if remote.VBVMR_SetParameterStringW(name.encode(), value) != 0:
        raise RuntimeError(f"Mixer refused {name} = {value!r}")


def visible_devices():
    """(kind, name) pairs the mixer offers for inputs and outputs."""
    found = {"in": set(), "out": set()}
    for side, count, desc in (("in", remote.VBVMR_Input_GetDeviceNumber, remote.VBVMR_Input_GetDeviceDescW),
                              ("out", remote.VBVMR_Output_GetDeviceNumber, remote.VBVMR_Output_GetDeviceDescW)):
        for i in range(count()):
            kind = ctypes.c_long()
            name, hwid = ctypes.create_unicode_buffer(512), ctypes.create_unicode_buffer(512)
            if desc(i, ctypes.byref(kind), name, hwid) == 0 and kind.value in DEVICE_KINDS:
                found[side].add((DEVICE_KINDS[kind.value], name.value))
    return found


def read_live():
    remote.VBVMR_IsParametersDirty()
    params = {}
    for i in range(8):
        for f in STRIP_FLOATS:
            params[f"Strip[{i}].{f}"] = get_float(f"Strip[{i}].{f}")
        params[f"Strip[{i}].Label"] = get_string(f"Strip[{i}].Label")
        for f in BUS_FLOATS:
            params[f"Bus[{i}].{f}"] = get_float(f"Bus[{i}].{f}")
        params[f"Bus[{i}].Label"] = get_string(f"Bus[{i}].Label")
    for key in VAIO_SLOTS:
        params[key] = get_float(key)
    devices = {}
    for slot in HARDWARE:
        name = get_string(f"{slot}.device.name")
        kind = DEVICE_KINDS.get(int(get_float(f"{slot}.device.type")), "wdm")
        devices[slot] = {"name": name, "kind": kind} if name else None
    return params, devices


# ---------------------------------------------------------------- store
def store_dir():
    root = Path(os.environ.get("VM_PROFILES") or Path(os.path.expanduser("~")) / "Documents" / "Voicemeeter" / "profiles")
    root.mkdir(parents=True, exist_ok=True)
    return root


def load_map():
    path = store_dir() / "devices.json"
    return json.loads(path.read_text(encoding="utf-8")) if path.exists() else {}


def save_map(mapping):
    (store_dir() / "devices.json").write_text(json.dumps(mapping, indent=1, ensure_ascii=False), encoding="utf-8")


def versions(name):
    """{version: [files]} for a profile; more than one file per version is a clash between machines."""
    result = {}
    for f in (store_dir() / name).glob("v*.json"):
        m = re.match(r"v(\d+)-", f.name)
        if m:
            result.setdefault(int(m.group(1)), []).append(f)
    return dict(sorted(result.items()))


def load_profile(name, version=None):
    found = versions(name)
    if not found:
        sys.exit(f"No profile named {name!r} in {store_dir()}")
    version = version or max(found)
    files = found.get(version)
    if not files:
        sys.exit(f"{name} has no version {version}; it has {list(found)}")
    if len(files) > 1:
        sys.exit(f"{name} v{version} was saved on more than one machine: "
                 + ", ".join(f.name for f in files) + ". Rename or delete one, then retry.")
    return json.loads(files[0].read_text(encoding="utf-8")), files[0]


def logical_for(device_name, mapping):
    for logical, local in mapping.get(HOST, {}).items():
        if local == device_name:
            return logical
    return device_name


# ---------------------------------------------------------------- planning
def plan(profile, only=None):
    """Changes needed to make the live mixer match the profile here, plus unresolved conflicts."""
    live_params, live_devices = read_live()
    mapping = load_map().get(HOST, {})
    available = visible_devices()
    in_scope = (lambda key: any(key == o or key.startswith(o + ".") for o in only)) if only else (lambda key: True)

    changes, conflicts = [], []
    for slot, want in profile["devices"].items():
        if not in_scope(slot) or want is None:
            continue
        local = mapping.get(want["logical"], want["logical"])
        side = "in" if slot.startswith("Strip") else "out"
        have = live_devices.get(slot)
        if have and have["name"] == local and have["kind"] == want["kind"]:
            continue   # already set, even if the device is unplugged right now
        if (want["kind"], local) not in available[side]:
            hint = "" if want["logical"] in mapping else f' (map it: Profiles.py map "{want["logical"]}" "<device>")'
            conflicts.append(f"{slot}: no {want['kind'].upper()} device for {want['logical']!r} on {HOST}{hint}; left as is")
        else:
            changes.append(("device", slot, have and f"{have['kind']}:{have['name']}", (want["kind"], local)))

    for key, value in profile["parameters"].items():
        if not in_scope(key) or key not in live_params:
            continue
        current = live_params[key]
        if isinstance(value, str) or isinstance(current, str):
            if value != current:
                changes.append(("string", key, current, value))
        elif abs(value - current) > 0.01:
            changes.append(("float", key, current, value))
    return changes, conflicts


def show(changes, conflicts):
    for kind, key, old, new in changes:
        new_text = f"{new[0]}:{new[1]}" if kind == "device" else new
        print(f"  {key:22} {old!s:>28}  ->  {new_text}")
    for c in conflicts:
        print(f"  CONFLICT {c}")
    if not changes and not conflicts:
        print("  nothing to change")


def snapshot_profile(note):
    params, devices = read_live()
    mapping = load_map()
    return {
        "format": FORMAT, "host": HOST, "note": note,
        "saved": datetime.datetime.now().isoformat(timespec="seconds"),
        "parameters": params,
        "devices": {slot: d and {"logical": logical_for(d["name"], mapping), "kind": d["kind"]}
                    for slot, d in devices.items()},
    }


def apply_changes(changes):
    for kind, key, _, new in changes:   # devices first, so levels land on the final devices
        if kind == "device":
            set_string(f"{key}.device.{new[0]}", new[1])
    if any(k == "device" for k, *_ in changes):
        time.sleep(3.0)   # a device change restarts the audio engine
    for kind, key, _, new in changes:
        if kind == "float":
            set_float(key, new)
        elif kind == "string":
            set_string(key, new)
    time.sleep(0.6)


def write_rollback():
    folder = store_dir() / "_rollback"
    folder.mkdir(exist_ok=True)
    path = folder / f"{datetime.datetime.now():%Y%m%d-%H%M%S}-{HOST}.json"
    path.write_text(json.dumps(snapshot_profile("rollback"), indent=1, ensure_ascii=False), encoding="utf-8")
    return path


def apply_profile(profile, only, label):
    changes, conflicts = plan(profile, only)
    print(f"{label}:")
    show(changes, conflicts)
    if not changes:
        return 0
    rollback = write_rollback()
    print(f"Rollback snapshot: {rollback}")
    apply_changes(changes)
    remaining, _ = plan(profile, only)
    if remaining:
        print("These did not take effect:")
        show(remaining, [])
        print(f"Undo with: Profiles.py rollback \"{rollback}\" --yes")
        return 1
    print(f"Applied {len(changes)} change(s).")
    return 0


# ---------------------------------------------------------------- commands
def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)
    p = sub.add_parser("save"); p.add_argument("name"); p.add_argument("--note", default="")
    sub.add_parser("list")
    for cmd in ("preview", "apply"):
        p = sub.add_parser(cmd); p.add_argument("name"); p.add_argument("--version", type=int)
        p.add_argument("--only", nargs="+", help='limit to these slots, e.g. "Strip[3]" "Bus[1]"')
        if cmd == "apply":
            p.add_argument("--yes", action="store_true")
    p = sub.add_parser("rollback"); p.add_argument("file", nargs="?", type=Path); p.add_argument("--yes", action="store_true")
    p = sub.add_parser("map"); p.add_argument("logical", nargs="?"); p.add_argument("device", nargs="?")
    args = parser.parse_args()

    if args.command == "list":
        for folder in sorted(d for d in store_dir().iterdir() if d.is_dir() and not d.name.startswith("_")):
            found = versions(folder.name)
            parts = []
            for v, files in found.items():
                parts.append(f"v{v}" + (" CLASH: " + ", ".join(f.name for f in files) if len(files) > 1 else f" ({files[0].stem.split('-', 1)[1]})"))
            print(f"{folder.name}: " + "; ".join(parts))
        return 0

    if args.command == "map" and args.logical and args.device:
        mapping = load_map()
        mapping.setdefault(HOST, {})[args.logical] = args.device
        save_map(mapping)
        print(f"{HOST}: {args.logical!r} -> {args.device!r}")
        return 0

    connect()
    try:
        if args.command == "map":
            print(json.dumps(load_map().get(HOST, {}), indent=1, ensure_ascii=False))
            for side, found in visible_devices().items():
                print(f"{side}: " + "; ".join(f"{k}:{n}" for k, n in sorted(found)))
            return 0

        if args.command == "save":
            existing = versions(args.name)
            version = max(existing, default=0) + 1
            folder = store_dir() / args.name
            folder.mkdir(exist_ok=True)
            profile = snapshot_profile(args.note)
            profile["name"], profile["version"] = args.name, version
            path = folder / f"v{version:04d}-{HOST}.json"
            path.write_text(json.dumps(profile, indent=1, ensure_ascii=False), encoding="utf-8")
            print(f"Saved {path}")
            return 0

        if args.command == "preview":
            profile, path = load_profile(args.name, args.version)
            print(f"{path.name} (saved on {profile['host']} {profile['saved']}) on {HOST}:")
            show(*plan(profile, args.only))
            return 0

        if args.command == "apply":
            profile, path = load_profile(args.name, args.version)
            if not args.yes:
                print("Preview only; add --yes to apply.")
                show(*plan(profile, args.only))
                return 0
            return apply_profile(profile, args.only, f"Applying {path.name}")

        if args.command == "rollback":
            path = args.file or max((store_dir() / "_rollback").glob("*.json"), default=None)
            if not path:
                sys.exit("No rollback snapshots.")
            profile = json.loads(Path(path).read_text(encoding="utf-8"))
            if not args.yes:
                print(f"Preview of rolling back to {path}; add --yes to apply.")
                show(*plan(profile))
                return 0
            return apply_profile(profile, None, f"Rolling back to {Path(path).name}")
    finally:
        remote.VBVMR_Logout()


if __name__ == "__main__":
    sys.exit(main())
