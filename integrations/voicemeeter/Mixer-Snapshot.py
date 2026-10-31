"""Small Remote API helper for Deploy-Trial.ps1.

  save <file>      write the strip/bus routing and level parameters to a JSON file
  compare <file>   compare the live mixer with a saved file (exit 1 on any difference)
  shutdown         ask the running mixer to shut down cleanly
"""
import ctypes
import json
import os
from pathlib import Path
import sys
import time

FIELDS = (("Strip", ["Gain", "Mute", "Solo", "Mono", "A1", "A2", "A3", "A4", "A5", "B1", "B2", "B3"]),
          ("Bus", ["Gain", "Mute", "Mono"]))
remote = ctypes.WinDLL(str(Path(os.environ["ProgramFiles(x86)"]) / "VB/Voicemeeter/VoicemeeterRemote64.dll"))
remote.VBVMR_GetParameterFloat.argtypes = [ctypes.c_char_p, ctypes.POINTER(ctypes.c_float)]
remote.VBVMR_SetParameterFloat.argtypes = [ctypes.c_char_p, ctypes.c_float]


def snapshot():
    result = {}
    for kind, fields in FIELDS:
        for index in range(8):
            for field in fields:
                name = f"{kind}[{index}].{field}"
                value = ctypes.c_float()
                if remote.VBVMR_GetParameterFloat(name.encode(), ctypes.byref(value)) != 0:
                    raise RuntimeError(f"Cannot read {name}")
                result[name] = round(value.value, 3)
    return result


def connect(timeout=20.0):
    if remote.VBVMR_Login() < 0:
        sys.exit("Remote API login failed.")
    end = time.time() + timeout
    kind = ctypes.c_long()
    while time.time() < end:   # a freshly started mixer takes a few seconds to answer
        if remote.VBVMR_GetVoicemeeterType(ctypes.byref(kind)) == 0:
            remote.VBVMR_IsParametersDirty()
            time.sleep(0.5)
            return
        time.sleep(0.5)
    sys.exit("No running mixer answered the Remote API.")


command = sys.argv[1] if len(sys.argv) > 1 else ""
connect()
try:
    if command == "save":
        Path(sys.argv[2]).write_text(json.dumps(snapshot(), indent=1))
    elif command == "compare":
        saved = json.loads(Path(sys.argv[2]).read_text())
        live = snapshot()
        diff = {k: (v, live.get(k)) for k, v in saved.items() if abs(v - live.get(k, 1e9)) > 0.01}
        print(f"{len(saved)} parameters compared; differences: {diff or 'none'}")
        sys.exit(1 if diff else 0)
    elif command == "shutdown":
        if remote.VBVMR_SetParameterFloat(b"Command.Shutdown", 1.0) != 0:
            sys.exit("Mixer refused Command.Shutdown.")
        time.sleep(1.0)   # parameters are applied asynchronously; logging out at once can drop it
    else:
        sys.exit(__doc__)
finally:
    remote.VBVMR_Logout()
