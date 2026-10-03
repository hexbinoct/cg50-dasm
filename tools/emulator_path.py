"""Where the fx-CG50 emulator project is checked out (https://github.com/hexbinoct/casio-cg50).

DASM itself doesn't need it. A few developer tools do: readme_shots.py (the README
screenshots, taken on the emulator), and proto_funcs.py / verify_decoder.py (the emulator's
reference disassembler, re/sh4dis.py). To use them, put the path of your checkout in
tools/emulator_path.txt (copy emulator_path.example.txt; a relative path is taken from the
dasm folder), or set the CG50_EMULATOR environment variable. emulator_path.txt is git-ignored,
so each machine keeps its own.

Tools that run a calculator OS need a flash dump of your own fx-CG50, set up as the emulator
project's README describes; no firmware is included here or there.
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
DASM = os.path.normpath(os.path.join(HERE, ".."))
CONFIG = os.path.join(HERE, "emulator_path.txt")
URL = "https://github.com/hexbinoct/casio-cg50"


def emulator_path(required=True):
    """The emulator checkout's directory, or None (required=False) when it isn't set up."""
    p = os.environ.get("CG50_EMULATOR", "").strip()
    if not p and os.path.exists(CONFIG):
        with open(CONFIG) as f:
            lines = [l.strip() for l in f if l.strip() and not l.lstrip().startswith("#")]
        p = lines[0] if lines else ""
    if p:
        p = os.path.normpath(os.path.join(DASM, os.path.expanduser(p)))
        if os.path.isdir(os.path.join(p, "emu_go")):
            return p
        msg = f"{p} is not a checkout of the emulator project (it has no emu_go/ folder)"
    else:
        msg = (f"this tool needs the fx-CG50 emulator project ({URL}): put the path of your "
               f"checkout in {CONFIG} (see emulator_path.example.txt) or set CG50_EMULATOR")
    if required:
        sys.exit(msg)
    return None
