#!/usr/bin/env python3
"""Differential test of the SH4AL-DSP parts of the decoder against GNU objdump
(`sh-elf-objdump -m sh4al-dsp`, from the fxSDK toolchain), over generated opcodes:

  A. every 16-bit word except the 32-bit prefixes 0xF800-0xFBFF;
  B. 0xF800 followed by every second word (all the parallel-processing operations);
  C. every first word 0xF800-0xFBFF (all the data-transfer fields) with a set of second words.

Compared exactly (after the normalisation below): the whole 0xF--- space, and the slots the DSP
changes elsewhere (sts/lds/stc/ldc of the DSP and repeat registers, setrc ldrc ldrs ldre,
clrdmxy setdmx setdmy). For every other word only the validity has to agree (both decode it, or
both print .word): the printers differ in cosmetics there, and tools/verify_decoder.py covers
them against the emulator's disassembler.

Known objdump bug (counted, not failed): with both transfer modes 0 (nopx nopy) but a store bit
(5 for X, 4 for Y) set, print_insn_ddt looks up a movx/movy entry that doesn't exist, runs past
them in its table and prints "pcmp x0,y0" (e.g. 0xF010, 0xF020). We print those as invalid.

Normalisation: whitespace runs -> one space; objdump's "! <word>" comments dropped; every hex
number compared by value; objdump prints pshl/psha's 7-bit immediate unsigned (we print it
signed); objdump prints a reserved DSP register as a number ("movs.w @-r4,0x0"), which counts
as an invalid instruction, as does any ".word" in its line.

  python3 tools/verify_dsp.py            (needs gcc and sh-elf-objdump on PATH or in ~/.local/bin)
"""
import os, re, shutil, struct, subprocess, sys, collections

WORK = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
OUT = os.path.join(WORK, "build-host")
os.makedirs(OUT, exist_ok=True)
OBJDUMP = shutil.which("sh-elf-objdump") or os.path.expanduser("~/.local/bin/sh-elf-objdump")

PPI_SAMPLES = [0xB100, 0x0127, 0x1F87, 0x4000, 0x7FFF, 0x6A5B, 0x8800, 0x9D1A, 0xA2C5, 0xFD07]

def words():
    seq = []                                           # (addr, [word...]) in file order
    for w in range(0x10000):
        if (w & 0xFC00) != 0xF800:
            seq.append([w])
    for g in range(0x10000):
        seq.append([0xF800, g])
    for f in range(0xF800, 0xFC00):
        for g in PPI_SAMPLES:
            seq.append([f, g])
    return seq

seq = words()
blob = b"".join(struct.pack(">" + "H" * len(s), *s) for s in seq)
image = os.path.join(OUT, "dsp_all.bin")
open(image, "wb").write(blob)

# ---- ours
exe = os.path.join(OUT, "dectest")
subprocess.check_call(["gcc", "-O2", "-Wall", "-Wextra", "-o", exe,
                       os.path.join(WORK, "tools", "dectest.c"), os.path.join(WORK, "src", "sh4dec.c")])
ours = {}
for line in subprocess.run([exe, image, "0", "0", hex(len(blob)), "len"], capture_output=True,
                           text=True, check=True).stdout.splitlines():
    addr, rest = line.split(": ", 1)
    ours[int(addr, 16)] = rest.split("   ", 1)[1] if "   " in rest else ""

# ---- objdump
theirs = {}
LINE = re.compile(r"^\s*([0-9a-f]+):\t((?:[0-9a-f]{2} )+)\s*\t?(.*)$")
out = subprocess.run([OBJDUMP, "-D", "-b", "binary", "-m", "sh4al-dsp", "-EB", image],
                     capture_output=True, text=True, check=True).stdout
for line in out.splitlines():
    m = LINE.match(line)
    if m:
        theirs[int(m.group(1), 16)] = m.group(3)

HEX = re.compile(r"0x[0-9a-f]+")

def norm(t):
    t = t.split("!")[0]
    t = re.sub(r"\s+", " ", t).strip()
    t = HEX.sub(lambda m: str(int(m.group(0), 16)), t)
    t = re.sub(r"^(p?sh[al]) #(\d+)", lambda m: f"{m.group(1)} #{int(m.group(2)) - 128 if int(m.group(2)) >= 64 else int(m.group(2))}", t)
    return t

def their_invalid(t):
    if ".word" in t:
        return True
    # a reserved DSP register, printed as a bare number ("movs.w @-r4,0x0", "padd x0,y0,0x0")
    toks = re.split(r"[\s,]+", t)
    if re.match(r"(movs|movx|movy|p[a-z]+|dc[tf])", toks[0]) and toks[0] not in ("pref", "prefi"):
        return any(re.fullmatch(r"0x[0-9a-f]+", x) for x in toks[1:])
    return False

def exact_slot(ws):
    w = ws[0]
    if (w & 0xF000) == 0xF000:
        return True
    lo = w & 0xF00F
    if lo in (0x000A, 0x4002, 0x4006, 0x400A) and ((w >> 4) & 15) in (5, 6, 7, 8, 9, 10, 11):
        return True
    if lo in (0x0002, 0x4003, 0x4007, 0x400E) and ((w >> 4) & 15) in (5, 6, 7):
        return True
    if lo == 0x4004 and ((w >> 4) & 15) in (1, 3):
        return True
    if w in (0x0088, 0x0098, 0x00C8):
        return True
    return (w & 0xFF00) in (0x8200, 0x8A00, 0x8C00, 0x8E00)

fails = collections.defaultdict(list)
counts = collections.Counter()
addr = 0
for ws in seq:
    a = addr
    addr += 2 * len(ws)
    o, t = ours.get(a), theirs.get(a)
    if o is None or t is None:
        fails["missing line"].append((ws, o, t)); continue
    f = ws[0] & 0x3FF
    if (ws[0] & 0xF400) == 0xF000 and (f & 0xF) == 0 and (f & 0x30) and "pcmp" in (t if len(ws) == 1 else t.split("\t", 2)[-1]):
        counts["objdump nopy bug"] += 1
        continue
    o_bad = o.startswith(".word") or o.startswith(".long")
    t_bad = their_invalid(t)
    if exact_slot(ws):
        counts["exact"] += 1
        if o_bad or t_bad:
            if o_bad != t_bad:
                fails["validity (exact slot)"].append((ws, o, t))
        elif norm(o) != norm(t):
            fails["text"].append((ws, o, t))
    else:
        counts["validity only"] += 1
        if o_bad != t_bad:
            fails["validity"].append((ws, o, t))

print(f"compared {counts['exact'] + counts['validity only']} instructions: {counts['exact']} exactly, "
      f"{counts['validity only']} for validity only; skipped {counts['objdump nopy bug']} (objdump nopy bug)")
for cat, lst in fails.items():
    print(f"\n{cat}: {len(lst)}")
    for ws, o, t in lst[:25]:
        print(f"  {' '.join(f'{w:04x}' for w in ws):10}  ours: {o!s:44} objdump: {t}")
sys.exit(1 if fails else 0)
