"""Live test of Profiles.py against the running mixer, touching only the idle Stereo Input 4.

Uses a throw-away profile store (given as the only argument, must not exist yet): save, no-op
preview, an edited version with routing/level/label changes plus an unmapped device, a scoped
apply limited to Strip[3], rollback, a hardware mapping preview, and a version clash.
"""
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

HERE = Path(__file__).resolve().parent
store = Path(sys.argv[1])
if store.exists():
    sys.exit(f"{store} already exists; give a new directory.")
env = dict(os.environ, VM_PROFILES=str(store), PYTHONIOENCODING="utf-8")
failures = 0


def run(*args):
    out = subprocess.run([sys.executable, str(HERE / "Profiles.py"), *args], capture_output=True, text=True,
                         encoding="utf-8", env=env, timeout=120)
    return out.returncode, out.stdout + out.stderr


def check(ok, what, detail=""):
    global failures
    print(("ok  " if ok else "FAIL"), what)
    if not ok:
        print("     " + detail.replace("\n", "\n     "))
        failures += 1


def strip3():
    code, text = run("preview", "test", "--version", "1", "--only", "Strip[3]")
    return code, text


try:
    code, text = run("save", "test", "--note", "baseline")
    check(code == 0 and "v0001-" in text, "save creates v1", text)
    code, text = run("preview", "test")
    check(code == 0 and "nothing to change" in text, "previewing the live state changes nothing", text)

    v1 = next((store / "test").glob("v0001-*.json"))
    profile = json.loads(v1.read_text(encoding="utf-8"))
    original = {k: profile["parameters"][k] for k in ("Strip[3].A2", "Strip[3].Gain", "Strip[3].Label")}
    profile["version"] = 2
    profile["parameters"].update({"Strip[3].A2": 1.0, "Strip[3].Gain": -6.0, "Strip[3].Label": "Profile test"})
    profile["devices"]["Bus[3]"] = {"logical": "Studio Monitors", "kind": "wdm"}
    v2 = v1.with_name(v1.name.replace("v0001", "v0002"))
    v2.write_text(json.dumps(profile, indent=1), encoding="utf-8")

    code, text = run("preview", "test")
    check("Strip[3].A2" in text and "Strip[3].Gain" in text and "Profile test" in text, "preview lists the three changes", text)
    check("CONFLICT Bus[3]" in text and "Studio Monitors" in text, "unmapped device is a conflict, not a change", text)

    code, text = run("apply", "test", "--only", "Strip[3]")
    check("Preview only" in text, "apply without --yes only previews", text)

    code, text = run("apply", "test", "--only", "Strip[3]", "--yes")
    check(code == 0 and "Applied 3 change(s)" in text and "Bus[3]" not in text, "scoped apply changes only Strip[3]", text)
    code, text = run("preview", "test", "--version", "2", "--only", "Strip[3]")
    check("nothing to change" in text, "live Strip[3] now matches v2", text)

    code, text = run("rollback", "--yes")
    check(code == 0 and "Applied 3 change(s)" in text, "rollback restores the three values", text)
    code, text = run("preview", "test", "--version", "1")
    check("nothing to change" in text, "live mixer matches v1 again", text)

    code, text = run("map", "Studio Monitors", "Speakers (TONEX)")
    code, text = run("preview", "test", "--only", "Bus[3]")
    check("Bus[3]" in text and "wdm:Speakers (TONEX)" in text and "CONFLICT" not in text,
          "mapping resolves the logical device on this machine", text)

    shutil.copy(v2, v2.with_name("v0002-OTHERPC.json"))
    code, text = run("list")
    check("v2 CLASH" in text, "list reports a version saved on two machines", text)
    code, text = run("preview", "test")
    check(code != 0 and "more than one machine" in text, "a clashing version is refused", text)
    print("original Strip[3] values:", original)
finally:
    shutil.rmtree(store, ignore_errors=True)
print("ALL PASSED" if failures == 0 else f"{failures} FAILED")
sys.exit(1 if failures else 0)
