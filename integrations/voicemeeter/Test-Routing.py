"""Live test of Reaper-Routing.py's feedback check and apply, using only the idle Stereo Input 4.

Creates a loop on purpose (Stereo Input 4 -> B3, with a test preset that records B3), checks that it
is reported as FEEDBACK, applies the preset to fix it, rolls back, and returns Stereo Input 4 to
its original routing. Takes a throw-away profile store directory (must not exist) for the rollback.
"""
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import Profiles as P  # noqa: E402

store = Path(sys.argv[1])
if store.exists():
    sys.exit(f"{store} already exists; give a new directory.")
store.mkdir(parents=True)
presets = store / "presets.json"
presets.write_text(json.dumps({"presets": [{
    "id": "test-loop", "title": "test: record B3, return into Stereo Input 4", "kind": "device", "driver": "WASAPI",
    "input": "Voicemeeter Out B3", "input_channels": [1, 2], "output": "(none)", "output_channels": [1, 2],
    "recorded_bus": "B3", "strip": 3, "hear_on": ["A3"], "reaper": "test only", "voicemeeter": "test only"}]}))
env = dict(os.environ, VM_PROFILES=str(store), PYTHONIOENCODING="utf-8")
failures = 0


def run(*args):
    out = subprocess.run([sys.executable, str(HERE / "Reaper-Routing.py"), "--presets", str(presets), *args],
                         capture_output=True, text=True, encoding="utf-8", env=env, timeout=120)
    return out.returncode, out.stdout + out.stderr


def fresh(key):
    """Read after another process's write; the mixer publishes it a moment later."""
    time.sleep(0.4)
    return P.get_float(key)


def check(ok, what, detail=""):
    global failures
    print(("ok  " if ok else "FAIL"), what)
    if not ok:
        print("     " + detail.replace("\n", "\n     "))
        failures += 1


P.connect()
keys = ["Strip[3].A3", "Strip[3].B3"]
original = {k: P.get_float(k) for k in keys}
try:
    P.set_float("Strip[3].B3", 1.0)
    P.set_float("Strip[3].A3", 0.0)
    time.sleep(0.6)   # sets are applied asynchronously
    code, text = run("check", "test-loop")
    check(code == 1 and "FEEDBACK: strip 4 routes to B3" in text, "a return strip routed to the recorded bus is FEEDBACK", text)
    check("not routed to ['A3']" in text, "missing monitor bus is a warning", text)

    code, text = run("apply", "test-loop")
    check("Preview only" in text and "Strip[3].B3 -> 0" in text and "Strip[3].A3 -> 1" in text, "apply previews the fix", text)
    code, text = run("apply", "test-loop", "--yes")
    check(code == 0 and "[ok]" in text and fresh("Strip[3].B3") == 0 and fresh("Strip[3].A3") == 1,
          "apply removes the loop and routes to A3", text)

    rollback = subprocess.run([sys.executable, str(HERE / "Profiles.py"), "rollback", "--yes"], capture_output=True,
                              text=True, encoding="utf-8", env=env, timeout=120)
    check(rollback.returncode == 0 and fresh("Strip[3].B3") == 1 and fresh("Strip[3].A3") == 0,
          "Profiles.py rollback restores the pre-apply routing", rollback.stdout + rollback.stderr)
finally:
    for k, v in original.items():
        P.set_float(k, v)
    time.sleep(0.6)
    restored = {k: P.get_float(k) for k in keys}
    check(restored == original, "Stereo Input 4 back to its original routing", str(restored))
    P.remote.VBVMR_Logout()
    shutil.rmtree(store, ignore_errors=True)
print("ALL PASSED" if failures == 0 else f"{failures} FAILED")
sys.exit(1 if failures else 0)
