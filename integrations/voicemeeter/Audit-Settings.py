"""Read-only mixer snapshot and native settings-export audit. Does not load settings."""
import argparse
import ctypes
import hashlib
import json
import os
from pathlib import Path
import time
import xml.etree.ElementTree as ET

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--output", type=Path, required=True,
                    help="New private output directory; never commit its contents.")
args = parser.parse_args()
args.output.mkdir(parents=True, exist_ok=False)
dll = Path(os.environ["ProgramFiles(x86)"]) / "VB/Voicemeeter/VoicemeeterRemote64.dll"
remote = ctypes.WinDLL(str(dll))
remote.VBVMR_GetParameterFloat.argtypes = [ctypes.c_char_p, ctypes.POINTER(ctypes.c_float)]
remote.VBVMR_SetParameterStringW.argtypes = [ctypes.c_char_p, ctypes.c_wchar_p]
if remote.VBVMR_Login() != 0:
    raise RuntimeError("A running mixer is required.")

def snapshot():
    remote.VBVMR_IsParametersDirty()
    result = {}
    for kind, fields in (
        ("Strip", ["Gain", "Mute", "Solo", "Mono", "A1", "A2", "A3", "A4", "A5", "B1", "B2", "B3"]),
        ("Bus", ["Gain", "Mute", "Mono"]),
    ):
        for index in range(8):
            for field in fields:
                name = f"{kind}[{index}].{field}"
                value = ctypes.c_float()
                status = remote.VBVMR_GetParameterFloat(name.encode(), ctypes.byref(value))
                if status != 0:
                    raise RuntimeError(f"Cannot read {name}: {status}")
                result[name] = value.value
    return result

try:
    time.sleep(0.5)
    before = snapshot()
    reports = []
    for index in range(2):
        output = (args.output / f"settings-{index + 1}.xml").resolve()
        status = remote.VBVMR_SetParameterStringW(b"Command.Save", str(output))
        if status != 0:
            raise RuntimeError(f"Settings export rejected: {status}")
        time.sleep(1)
        data = output.read_bytes()
        if not data:
            raise RuntimeError("Empty settings export.")
        parse_error = None
        try:
            ET.fromstring(data)
        except ET.ParseError as error:
            parse_error = str(error)
        reports.append({"file": output.name, "bytes": len(data),
                        "sha256": hashlib.sha256(data).hexdigest(),
                        "xml_parse_error": parse_error})
    after = snapshot()
    changed = [key for key in before if abs(before[key] - after[key]) > 0.0001]
    report = {"exports": reports, "changed_parameters": changed,
              "snapshot": after, "native_restore_tested": False}
    (args.output / "audit.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
    print(json.dumps({"exports": reports, "changed_parameter_count": len(changed),
                      "native_restore_tested": False}, indent=2))
finally:
    remote.VBVMR_Logout()
