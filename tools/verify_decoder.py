#!/usr/bin/env python3
"""Differential test: the C decoder (tools/dectest.c) vs the reference re/sh4dis.py of the
emulator project (https://github.com/hexbinoct/casio-cg50), instruction by instruction over a
whole image, e.g. the OS of your own calculator (os.bin from your dump; none is included).
Needs gcc + python3. In the fxsdk container, with the emulator project mounted at /re:

  docker run --rm -v "<dasm>:/work" -v "<emulator>/re:/re:ro" -v "<emulator>/os/flash_dump:/os:ro" \
      fxsdk:latest python3 /work/tools/verify_decoder.py [/os/os.bin 0x80000000 [start end]]

or on the host, with the emulator path in tools/emulator_path.txt:
  python3 tools/verify_decoder.py <os.bin> 0x80000000

Normalisation (the two printers differ only in cosmetics):
  * immediates: sh4dis prints "#0x-10 ; -16" / "#0xff ; -1"; the C prints "#-16" / "#-1".
    Both sides are rewritten to "#<signed decimal>" (logic ops keep unsigned hex on both).
  * sh4dis' trailing "    ; <decimal>" comments are dropped; "; =0x..." literals are kept.
  * The SH7305 is an SH4AL-DSP: the 0xF--- space (DSP, sh4dis says "fpu ...") and the slots the
    DSP takes over elsewhere (sh4dis: fpul/fpscr; also mod rs re, setrc ldrc ldrs ldre, clrdmxy
    setdmx setdmy) are counted, not compared: tools/verify_dsp.py checks them against objdump.
Every remaining difference is listed by category with examples and a verdict has to be
given by hand (see KNOWN_PY_BUGS: cases where the Python is wrong per the SH-4A manual).
"""
import os, re, subprocess, sys, collections

if os.path.isdir("/re"):           # in the container
    sys.path.insert(0, "/re")
    WORK = "/work"
else:                              # on the host
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    from emulator_path import emulator_path
    sys.path.insert(0, os.path.join(emulator_path(), "re"))
    WORK = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
import sh4dis  # noqa: E402

image = sys.argv[1] if len(sys.argv) > 1 else "/os/os.bin"
base = int(sys.argv[2], 0) if len(sys.argv) > 2 else 0x80000000
data = open(image, "rb").read()
start = int(sys.argv[3], 0) if len(sys.argv) > 3 else base
end = int(sys.argv[4], 0) if len(sys.argv) > 4 else base + len(data)

# sh4dis maps vaddr -> file offset with (va & 0x0FFFFFFF); make that equal our base
assert (base & 0x0FFFFFFF) == 0, "sh4dis assumes base & 0x0FFFFFFF == 0"
sh4dis.set_image(data)

# ---- build + run the C harness
os.makedirs(f"{WORK}/build-host", exist_ok=True)
subprocess.check_call(["gcc", "-O2", "-Wall", "-Wextra", "-o", f"{WORK}/build-host/dectest",
                       f"{WORK}/tools/dectest.c", f"{WORK}/src/sh4dec.c"])
proc = subprocess.Popen([f"{WORK}/build-host/dectest", image, hex(base), hex(start), hex(end)],
                        stdout=subprocess.PIPE, text=True, bufsize=1 << 20)

IMM = re.compile(r"#(-?0x-?[0-9a-f]+|-?\d+)")
LOGIC = ("tst ", "and ", "xor ", "or ", "tst.b", "and.b", "xor.b", "or.b", "trapa")

def imm_to_dec(m):
    s = m.group(1)
    neg = s.startswith("-") or "0x-" in s
    digits = s.replace("-", "")
    v = int(digits, 0)
    if not neg and 128 <= v <= 255:      # sh4dis prints the raw byte for mov/add/cmp #imm8
        v -= 256
    return f"#{-v if neg else v}"

def norm(text):
    """Canonical form of one instruction text (either printer)."""
    text = text.rstrip()
    comment = ""
    if "    ;" in text:
        text, c = text.split("    ;", 1)
        c = c.strip()
        if c.startswith("=0x"):
            comment = c
    text = text.strip()
    if not text.startswith(LOGIC):
        text = IMM.sub(imm_to_dec, text)
    return text + (("  ; " + comment) if comment else "")

# Python defects, per the SH-4A software manual (we keep the C behaviour):
KNOWN_PY_BUGS = {
    "stc.l gbr,@-": "0x4n13 STC.L GBR,@-Rn missing from sh4dis",
    "stc r": "0000nnnn1mmm0010 STC Rm_BANK,Rn: sh4dis only matches m=0 (x&0xFF==0x82)",
    "movco.l": "SH-4A", "movli.l": "SH-4A", "movua.l": "SH-4A", "icbi": "SH-4A", "prefi": "SH-4A",
    "synco": "SH-4A", "stc sgr": "SH-4A", "stc dbr": "SH-4/4A", "ldc.l @r": "SH-4A dbr/sgr forms",
    "stc.l sgr": "SH-4A", "stc.l dbr": "SH-4", "ldc r": "dbr/sgr forms",
}

def dsp_slot(op):
    """A word whose meaning the DSP sets (see tools/verify_dsp.py, which checks them)."""
    lo, k = op & 0xF00F, (op >> 4) & 15
    return (op >> 12 == 0xF
            or (lo in (0x000A, 0x4002, 0x4006, 0x400A) and k in (5, 6, 7, 8, 9, 10, 11))
            or (lo in (0x0002, 0x4003, 0x4007, 0x400E) and k in (5, 6, 7))
            or (lo == 0x4004 and k in (1, 3))
            or op in (0x0088, 0x0098, 0x00C8)
            or (op & 0xFF00) in (0x8200, 0x8A00, 0x8C00, 0x8E00))

total = fpu = same = 0
diffs = collections.defaultdict(list)
pc = start
for line in proc.stdout:
    if pc >= end:
        break
    addr_s, rest = line.split(":", 1)
    assert int(addr_s, 16) == pc, (addr_s, hex(pc))
    word_s, ctext = rest.strip().split("   ", 1) if "   " in rest.strip() else (rest.strip(), "")
    op = int(word_s, 16)
    total += 1
    if dsp_slot(op):
        fpu += 1
    else:
        ptext = sh4dis.decode(op, pc)
        a, b = norm(ctext), norm(ptext)
        if a == b:
            same += 1
        else:
            key = (a.split("  ;")[0].split(" ", 1)[0], b.split(" ", 1)[0])
            if len(diffs[key]) < 4:
                diffs[key].append((pc, op, a, b))
            else:
                diffs[key].append(None)
    pc += 2
proc.wait()

print(f"image {image} base {base:#x}: {total} words, {fpu} DSP (not compared: verify_dsp.py), {same} identical, "
      f"{total - fpu - same} different")
for key, lst in sorted(diffs.items(), key=lambda kv: -len(kv[1])):
    n = len(lst)
    ex = [e for e in lst if e]
    verdict = next((v for k, v in KNOWN_PY_BUGS.items() if ex and ex[0][2].startswith(k)), "?? UNEXPLAINED")
    print(f"\n[{n:>7}] C='{key[0]}'  py='{key[1]}'   -> {verdict}")
    for pc_, op, a, b in ex[:3]:
        print(f"      {pc_:08x} {op:04x}  C: {a:<40} py: {b}")
unexplained = sum(len(l) for k, l in diffs.items()
                  if not any(l[0] and l[0][2].startswith(kk) for kk in KNOWN_PY_BUGS))
print(f"\nUNEXPLAINED differences: {unexplained}")
sys.exit(1 if unexplained else 0)
