"""Full-fidelity Voicemeeter settings backup and restore through the mixer's own Save/Load.

Profiles.py covers routing, levels, labels and devices; a native settings file also holds EQ,
dynamics, effects, VBAN and patch settings. Restore uses Command.Load, so it first takes a
Profiles.py rollback snapshot and afterwards checks that the result matches the file's intent.

  save FILE                 export the live settings (Command.Save)
  restore FILE --yes        load FILE, then verify routing/levels/devices against a fresh export
  test DIR                  round trip in a new private DIR: export, mark the free Stereo Input 5,
                            load the export, and check the mark is gone and nothing else moved

Load restarts the audio engine, which would break a softphone call, so restore and test need
--no-call-in-progress on a mixer set up as the softphone bridge.

Settings files are private machine configuration: keep them out of Git.
"""
import argparse
from pathlib import Path
import sys
import time

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import Mixer_Guard  # noqa: E402
import Profiles as P  # noqa: E402


def export(path):
    path = Path(path).resolve()
    path.unlink(missing_ok=True)
    P.set_string("Command.Save", str(path))
    for _ in range(40):   # the mixer writes the file asynchronously
        time.sleep(0.25)
        if path.exists() and path.stat().st_size > 0:
            time.sleep(0.5)
            return path.read_bytes()
    raise RuntimeError(f"The mixer did not write {path}")


def load(path):
    P.set_string("Command.Load", str(Path(path).resolve()))
    time.sleep(4.0)   # a load restarts the audio engine; stay logged in and let the cache catch up
    P.refresh()
    time.sleep(0.5)


def state():
    params, devices = P.read_live()
    return params, devices


def differences(a, b):
    (pa, da), (pb, db) = a, b
    diff = {k: (pa[k], pb.get(k)) for k in pa if pa[k] != pb.get(k)}
    diff.update({k: (da[k], db.get(k)) for k in da if da[k] != db.get(k)})
    return diff


def strip_settings(xml, index):
    """The <Strip index='n' ...> line(s) of a Potato export, for a byte-level comparison of one strip."""
    text = xml.decode("utf-8", errors="replace")
    return [line.strip() for line in text.splitlines() if f"<Strip index='{index}'" in line]


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)
    p = sub.add_parser("save"); p.add_argument("file", type=Path)
    p = sub.add_parser("restore"); p.add_argument("file", type=Path); p.add_argument("--yes", action="store_true")
    p.add_argument("--no-call-in-progress", action="store_true")
    p = sub.add_parser("test"); p.add_argument("dir", type=Path); p.add_argument("--no-call-in-progress", action="store_true")
    args = parser.parse_args()
    P.connect()
    try:
        if args.command != "save" and Mixer_Guard.bridge_configured(P) and not args.no_call_in_progress:
            sys.exit("Refusing: Load restarts the audio engine and this mixer is the softphone bridge. "
                     "Rerun with --no-call-in-progress once no call is up.")
        if args.command == "save":
            print(f"Saved {len(export(args.file))} bytes to {args.file}")
            return 0

        if args.command == "restore":
            if not args.file.exists():
                sys.exit(f"{args.file} does not exist")
            if not args.yes:
                print("This replaces all live mixer settings with the file. Add --yes to continue.")
                return 0
            rollback = P.write_rollback()
            print(f"Rollback snapshot: {rollback}")
            load(args.file)
            print(f"Loaded {args.file}. Routing, levels and devices now:")
            params, devices = state()
            print(f"  {sum(1 for v in devices.values() if v)} hardware devices set; "
                  f"undo routing/levels with: Profiles.py rollback \"{rollback}\" --yes")
            return 0

        # test
        Mixer_Guard.require_free_strip(P, 4)
        args.dir.mkdir(parents=True, exist_ok=False)
        failures = 0

        def check(ok, what, detail=""):
            nonlocal failures
            print(("ok  " if ok else "FAIL"), what, detail)
            failures += not ok

        rollback = P.write_rollback()
        before = state()
        original = export(args.dir / "before.xml")
        check(len(original) > 1000, "export written", f"{len(original)} bytes")
        try:
            P.set_float("Strip[4].Gain", -6.0)
            P.set_float("Strip[4].A3", 1.0)
            P.set_string("Strip[4].Label", "LoadTest")
            time.sleep(0.8)
            marked = state()
            check(marked[0]["Strip[4].Gain"] == -6.0 and marked[0]["Strip[4].Label"] == "LoadTest",
                  "marker set on Stereo Input 5")

            load(args.dir / "before.xml")
            after = state()
            diff = differences(before, after)
            check(not diff, "after Load, routing/levels/labels/devices equal the exported state", str(diff or ""))
            reexport = export(args.dir / "after.xml")
            check(strip_settings(original, 5) == strip_settings(reexport, 5),
                  "Stereo Input 5's full settings line is identical after Load")
            changed = sum(1 for x, y in zip(original.splitlines(), reexport.splitlines()) if x != y)
            print(f"     re-export: {len(reexport)} bytes vs {len(original)}; {changed} differing lines")
        finally:
            if differences(before, state()):
                print("Restoring through Profiles.py rollback")
                import subprocess
                subprocess.run([sys.executable, str(HERE / "Profiles.py"), "rollback", str(rollback), "--yes"])
        print("ALL PASSED" if failures == 0 else f"{failures} FAILED")
        return 1 if failures else 0
    finally:
        P.remote.VBVMR_Logout()


if __name__ == "__main__":
    sys.exit(main())
