"""Regenerate the README screenshots (docs/img/*.png) on the desktop emulator.

Runs the parent project's TestDasmReal probe: the full 32 MB flash dump resumed at the MAIN
MENU, DASM launched from its menu icon with this directory's DASM.g3a swapped in, then a key
script; each shot is the add-in's whole 396x224 frame. DASM browses only our own add-in
(DASM.g3a itself), so no Casio code is shown. Needs the parent repo next to this one
(../casio-cg50 on the Mac, may/cg50 at the office) with os/flash_dump/flash_full_32mb.bin and
cg50_state_32mb.bin.  Run:  python3 tools/readme_shots.py
"""
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
DASM = os.path.normpath(os.path.join(HERE, ".."))
OUT = os.path.join(DASM, "docs", "img")
PARENTS = [os.path.join(DASM, "..", "..", "casio-cg50"), os.path.join(DASM, "..", "..", "..", "may", "cg50")]

# key = "row-col" (re/KEYMAP.md). The first DOWN is swallowed while DASM starts up.
DOWN, UP, EXE, EXIT = "2-7", "3-7", "2-1", "3-7"
F1, F2, F3, F4, F5, F6 = "6-9", "5-9", "4-9", "3-9", "2-9", "1-9"
OPTN, VARS, XOT = "5-8", "4-8", "6-6"

# (key, screenshot name or None); DASM.g3a is entry 20 of the picker on the 32 MB dump
SCRIPT = [(DOWN, None)] * 20 + [(DOWN, "picker"), (EXE, None), (DOWN, "listing"),
          (F3, "hex"), (F3, None), (F5, "header"), (F5, None), (OPTN, "small-font"), (OPTN, None),
          (VARS, None), (DOWN, None), (DOWN, None), (DOWN, "functions"), (EXE, "function"),
          (XOT, "references")]


def main():
    parent = next((os.path.normpath(p) for p in PARENTS if os.path.isdir(os.path.join(p, "emu_go"))), None)
    if not parent:
        sys.exit("the emulator repository was not found")
    keys = ",".join(k for k, _ in SCRIPT)
    env = dict(os.environ, DASM_LAUNCH="1", DASM_SWAP=os.path.join(DASM, "DASM.g3a"), DASM_KEYS=keys)
    r = subprocess.run(["go", "-C", os.path.join(parent, "emu_go"), "test", "-tags", "probe",
                        "-run", "TestDasmReal", "-count=1", "."], env=env, capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit(r.stdout[-3000:] + r.stderr[-3000:])
    os.makedirs(OUT, exist_ok=True)
    tmp = tempfile.gettempdir()
    for i, (_, name) in enumerate(SCRIPT):
        if name:
            src = os.path.join(tmp, f"cg50_real_{i + 1:02d}_full.png")
            shutil.copyfile(src, os.path.join(OUT, name + ".png"))
            print("wrote docs/img/" + name + ".png")


if __name__ == "__main__":
    main()
