"""Stops live tests and engine restarts from touching what the softphone bridge is using.

The bridge takes a hardware strip for the incoming call (VBAN stream CallToLaptop) and a bus for the
outgoing mic (stream LaptopMic), so a strip that was idle last week may carry a call today.

  python Mixer_Guard.py     read-only report of which hardware strips are free right now
"""
import sys

VBAN_IN_STREAMS = 8   # slot 9 is the text/command slot, and its values read back wrong
BUSES = ["A1", "A2", "A3", "A4", "A5", "B1", "B2", "B3"]


def strip_problem(P, strip):
    """Why this hardware strip is not free for a test, or None if it is."""
    device = P.get_string(f"Strip[{strip}].device.name")
    if device:
        return f"it has the input device {device!r}"
    routed = [b for b in BUSES if P.get_float(f"Strip[{strip}].{b}") > 0.5]
    if routed:
        return f"it is routed to {routed}"
    return vban_user(P, strip)


def vban_user(P, strip):
    """Why an incoming VBAN stream makes this strip busy, or None."""
    for i in range(VBAN_IN_STREAMS):
        if P.get_float(f"vban.instream[{i}].on") > 0.5 and int(P.get_float(f"vban.instream[{i}].route")) == strip:
            return f"VBAN stream {P.get_string(f'vban.instream[{i}].name')!r} feeds it"
    return None


def bridge_configured(P):
    """True when this mixer is set up as the softphone bridge's laptop side."""
    return (P.get_string("vban.outstream[0].name") == "LaptopMic"
            or P.get_string("vban.instream[0].name") == "CallToLaptop")


def require_free_strip(P, strip):
    problem = strip_problem(P, strip)
    if problem:
        sys.exit(f"Refusing: Stereo Input {strip + 1} is in use because {problem}. Nothing was changed.")


if __name__ == "__main__":
    from pathlib import Path
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    import Profiles as P
    P.connect()
    try:
        for s in range(5):
            problem = strip_problem(P, s)
            print(f"Stereo Input {s + 1}: {'free' if not problem else 'in use, ' + problem}")
        print("softphone bridge configured:", bridge_configured(P))
    finally:
        P.remote.VBVMR_Logout()
