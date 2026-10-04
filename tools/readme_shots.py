"""Regenerate the README screenshots (docs/img/*.png) on the desktop emulator.

Runs the emulator project's TestDasmReal probe: the full 32 MB flash dump resumed at the MAIN
MENU, DASM launched from its menu icon with this directory's DASM.g3a swapped in, then a key
script; each shot is the add-in's whole 396x224 frame. DASM browses only its own code
(DASM.g3a itself), so no Casio code is shown. Needs the emulator project (path in
tools/emulator_path.txt, see emulator_path.py) with a 32 MB dump of your own calculator
(os/flash_dump/flash_full_32mb.bin + cg50_state_32mb.bin there). The picker position in SCRIPT
assumes that dump's file list. The debugger shots come from the emulator's TestMonitorDasmGint
probe: DASM's F2 screen, then its monitor stopped inside KeyProbe (a small gint add-in of ours
on that dump).  Run:  python3 tools/readme_shots.py
"""
import os
import shutil
import subprocess
import sys
import tempfile

from emulator_path import emulator_path

HERE = os.path.dirname(os.path.abspath(__file__))
DASM = os.path.normpath(os.path.join(HERE, ".."))
OUT = os.path.join(DASM, "docs", "img")

# key = "row-col" (re/KEYMAP.md). The first DOWN is swallowed while DASM starts up.
DOWN, UP, EXE, EXIT = "2-7", "1-8", "2-1", "3-7"
F1, F2, F3, F4, F5, F6 = "6-9", "5-9", "4-9", "3-9", "2-9", "1-9"
OPTN, VARS, XOT, FD = "5-8", "4-8", "6-6", "5-5"

# (key, screenshot name or None); DASM.g3a is entry 20 of the picker on the 32 MB dump
SCRIPT = [(DOWN, None)] * 20 + [(DOWN, "picker"), (EXE, None), (DOWN, "listing"),
          (FD, "listing-light"), (FD, None),
          (F3, "hex"), (F3, None), (F5, "header"), (F5, None), (OPTN, "small-font"), (OPTN, None),
          (VARS, None), (DOWN, None), (DOWN, None), (DOWN, "functions"), (EXE, "function"),
          (XOT, "references")]

# the debugger: TestMonitorDasmGint's panel shots ($TMPDIR/cg50_mondbg_gint_<shot>.png)
DEBUGGER = [("01_explain", "debugger-arm"), ("06_gint_step", "debugger")]


def main():
    emu = emulator_path()
    keys = ",".join(k for k, _ in SCRIPT)
    env = dict(os.environ, DASM_LAUNCH="1", DASM_SWAP=os.path.join(DASM, "DASM.g3a"), DASM_KEYS=keys)
    r = subprocess.run(["go", "-C", os.path.join(emu, "emu_go"), "test", "-tags", "probe",
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

    env = dict(os.environ, MONDBG_DASM=os.path.join(DASM, "DASM.g3a"))
    r = subprocess.run(["go", "-C", os.path.join(emu, "emu_go"), "test", "-tags", "probe",
                        "-run", "^TestMonitorDasmGint$", "-count=1", "."], env=env, capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit(r.stdout[-3000:] + r.stderr[-3000:])
    for shot, name in DEBUGGER:
        shutil.copyfile(os.path.join(tmp, f"cg50_mondbg_gint_{shot}.png"), os.path.join(OUT, name + ".png"))
        print("wrote docs/img/" + name + ".png")


if __name__ == "__main__":
    main()
