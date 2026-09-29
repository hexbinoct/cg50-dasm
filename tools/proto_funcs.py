"""Host prototype of DASM's function detection (what src/analysis.c implements), scored
against the ELF symbol table of our own build.

Recursive traversal: explore each known function from its start, following fall-through,
bt/bf/bra targets and gcc jump tables (mova + mov.w @(r0,Rn) + braf) until rts / jmp, looking
only at real code; bsr targets and jsr/jmp @Rn targets (Rn loaded by a mov.l @(disp,pc) on the
same path; r0-r7 forgotten across calls) become new functions. Then: right after a function's
literal pool and nop/zero padding, a prologue (register push or sts.l pr in the first 3
instructions, all real instructions) starts a function; a code address held in a literal pool or an aligned data word
starts one if it has that prologue, or if it sits right after a function and explores cleanly.
Results (2026-09-29): DASM.g3a 250 of its 292 C functions (85.6%), 6 false (mostly gint's asm
interrupt handlers, real code); OS 3.60: 12079 functions, incl. all but 89 of Ghidra's 10242.

  python3 tools/proto_funcs.py                 # DASM.g3a vs build-cg/dasm symbols
  python3 tools/proto_funcs.py <image> <base>  # count on another image (e.g. the OS)
"""
import bisect
import os
import struct
import subprocess
import sys


sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', '..', 'casio-cg50', 're'))
try:
    import sh4dis                       # the parent project's disassembler: is it an instruction?
    def valid(op): return not sh4dis.decode(op, 0).startswith('.word')
except Exception:
    def valid(op): return True


def s8(x): return x - 256 if x & 0x80 else x
def s12(x): return x - 4096 if x & 0x800 else x


def analyse(img, base, seeds):
    n = len(img)
    end = base + n
    w = lambda a: struct.unpack('>H', img[a - base:a - base + 2])[0] if base <= a <= end - 2 else None
    l = lambda a: struct.unpack('>I', img[a - base:a - base + 4])[0] if base <= a <= end - 4 else None
    code = lambda a: base <= a < end and a % 2 == 0
    funcs = {}                                  # start -> (last code byte + 1, pool end)

    def explore(start, collect=True):
        """one function: returns (code end, pool end, [call targets])"""
        visited, calls, ptrs = set(), [], []
        clean = True
        hi, pool = start, start
        work = [(start, {})]
        while work:
            a, lit = work.pop()
            lit = dict(lit)
            imm, mova, tload, bound = {}, None, None, None   # jump-table tracking (gcc casesi)
            while True:
                if not (code(a) and a - start < 0x10000):
                    clean = False; break
                if a in visited: break
                visited.add(a)
                op = w(a)
                if op in (0xFFFF, 0x0000):
                    clean = False; break
                if not collect and not valid(op):
                    clean = False; break
                hi = max(hi, a + 2)
                stop = False
                nxt = a + 2
                if op & 0xF000 == 0xD000:                       # mov.l @(disp,pc),Rn
                    la = (a & ~3) + 4 + (op & 0xFF) * 4
                    pool = max(pool, la + 4)
                    lit[(op >> 8) & 15] = l(la)
                    if lit[(op >> 8) & 15] is not None: ptrs.append(lit[(op >> 8) & 15])
                elif op & 0xF000 == 0x9000:                     # mov.w @(disp,pc),Rn
                    pool = max(pool, a + 4 + (op & 0xFF) * 2 + 2)
                    lit.pop((op >> 8) & 15, None)
                elif op & 0xF000 == 0xB000:                     # bsr
                    t = a + 4 + s12(op & 0xFFF) * 2
                    if code(t): calls.append(t)
                    nxt = None                                  # delay slot, then fall through
                elif op & 0xF000 == 0xA000:                     # bra
                    t = a + 4 + s12(op & 0xFFF) * 2
                    if code(t): work.append((t, lit))
                    stop = True
                elif (op & 0xFF00) in (0x8900, 0x8B00, 0x8D00, 0x8F00):   # bt bf bt/s bf/s
                    t = a + 4 + s8(op & 0xFF) * 2
                    if code(t): work.append((t, lit))
                elif op & 0xF0FF in (0x400B, 0x402B):           # jsr @Rn / jmp @Rn (tail call)
                    t = lit.get((op >> 8) & 15)
                    if t is not None and code(t): calls.append(t)
                    if op & 0xF0FF == 0x402B: stop = True
                elif op & 0xF0FF == 0x0023:                     # braf Rn: a gcc jump table?
                    r = (op >> 8) & 15
                    if mova is not None and tload and tload[0] == r:
                        _, size, scale = tload
                        count = bound + 1 if bound is not None and bound < 256 else 0
                        for k in range(count):
                            ea = mova + k * size
                            v = w(ea) if size == 2 else (img[ea - base] if base <= ea < end else None)
                            if v is None: break
                            v = (v - 0x10000 if v & 0x8000 else v) if size == 2 else (v - 256 if v & 0x80 else v)
                            t = a + 4 + v * scale
                            if code(t): work.append((t, lit))
                        pool = max(pool, mova + count * size)
                    stop = True
                elif op in (0x000B, 0x002B):                    # rts rte
                    stop = True
                elif op & 0xFF00 == 0xC700:                     # mova @(disp,pc),r0
                    mova = (a & ~3) + 4 + (op & 0xFF) * 4
                elif op & 0xF000 == 0xE000:                     # mov #imm,Rn
                    imm[(op >> 8) & 15] = op & 0xFF
                elif op & 0xF00F == 0x3006:                     # cmp/hi Rm,Rn (index > max -> default)
                    bound = imm.get((op >> 4) & 15)
                elif op & 0xF00F in (0x000D, 0x000C):           # mov.w/mov.b @(r0,Rm),Rn: table load
                    tload = [(op >> 8) & 15, 2 if op & 0xF == 0xD else 1, 1]
                elif tload and op & 0xF00F == 0x300C and (op >> 8) & 15 == (op >> 4) & 15 == tload[0]:
                    tload[2] = 2                                # add Rn,Rn: entries are halved offsets
                hi_ = op >> 12
                if hi_ in (5, 6, 7, 0xE) or (hi_ == 4 and op & 0xFF not in (0x0B, 0x2B, 0x22, 0x12, 0x02, 0x13)):
                    if op & 0xF000 != 0xD000: lit.pop((op >> 8) & 15, None)
                if op & 0xF000 == 0xB000 or op & 0xF0FF == 0x400B:   # a call clobbers r0-r7
                    for r in range(8): lit.pop(r, None)
                if stop or nxt is None:
                    # the delay slot belongs to the function too
                    if code(a + 2): visited.add(a + 2); hi = max(hi, a + 4)
                    if stop: break
                    a = a + 4
                    continue
                a = nxt
        return hi, max(pool, hi), calls, ptrs, clean

    todo = [s for s in seeds if code(s)]
    litptrs = set()
    while True:
        while todo:
            s = todo.pop()
            if s in funcs: continue
            hi, pool, calls, ptrs, _ = explore(s)
            funcs[s] = (hi, pool)
            litptrs.update(p for p in ptrs if code(p))
            todo.extend(c for c in calls if c not in funcs)
        # gaps: after each function's pool + padding, a prologue starts another one
        starts = sorted(funcs)
        bounds = set()
        for i, s in enumerate(starts):
            a = funcs[s][1]
            nxt = starts[i + 1] if i + 1 < len(starts) else end
            while a < nxt and w(a) in (0x0009, 0x0000): a += 2
            if a < nxt and a not in funcs:
                bounds.add(a)
                if prologue_like(w, a): todo.append(a)
        # pointers to code (literal-pool values, aligned words outside explored code) that land
        # where a function can start: right after another one, or on a prologue
        def inside(v):
            i = bisect.bisect_right(starts, v) - 1
            return i >= 0 and starts[i] < v < funcs[starts[i]][0]
        cands = set(litptrs)
        ranges = [(s, funcs[s][0]) for s in starts]
        a, ri = base, 0
        while a <= end - 4:
            while ri < len(ranges) and ranges[ri][1] <= a: ri += 1
            if ri < len(ranges) and ranges[ri][0] <= a < ranges[ri][1]:
                a = (ranges[ri][1] + 3) & ~3; continue
            v = l(a)
            if code(v): cands.add(v)
            a += 4
        for v in cands:
            if v in funcs or inside(v): continue
            # a strict prologue, or clean code (every path returns, no erased/zero words) that
            # starts right after another function's pool
            if prologue_like(w, v) or (v in bounds and explore(v, False)[4]):
                todo.append(v)
        if not todo: break
    return funcs


def prologue_like(w, a):
    """a push (mov.l Rm,@-r15) or sts.l pr,@-r15 within the first 3 instructions. (Looser tests,
    e.g. 6 instructions or add #-N,r15, let pointers into the OS's data regions through.)"""
    for k in range(3):
        op = w(a + 2 * k)
        if op is None: return False
        if (op & 0xFF0F) == 0x2F06 or op == 0x4F22: return True
        if op in (0x000B, 0xFFFF, 0x0000) or not valid(op): return False
    return False


def os_seeds(img, base):
    """reset vector + every syscall handler in the OS's table (trampoline 0x80020070)"""
    l = lambda a: struct.unpack('>I', img[a - base:a - base + 4])[0]
    seeds = [base]
    for a in range(0x80020070, 0x80020080, 2):
        op = struct.unpack('>H', img[a - base:a - base + 2])[0]
        if op & 0xFF00 == 0xD200:
            table = l((a & ~3) + 4 + (op & 0xFF) * 4)
            for i in range(0x2000):
                h = l(table + 4 * i) if base <= table + 4 * i < base + len(img) - 4 else 0
                if base <= h < base + len(img): seeds.append(h)
            break
    return seeds


def main():
    if len(sys.argv) > 2:
        img = open(sys.argv[1], 'rb').read()[:0xC00000]
        base = int(sys.argv[2], 0)
        f = analyse(img, base, os_seeds(img, base) if base == 0x80000000 else [base])
        print(f"functions {len(f)}; code bytes {sum(h - s for s, (h, _) in f.items())}")
        return
    g = open('DASM.g3a', 'rb').read()[0x7000:]
    f = analyse(g, 0x300000, [0x300000])
    nm = subprocess.run([os.path.expanduser('~/.local/bin/sh-elf-nm'), '-n', 'build-cg/dasm'], capture_output=True, text=True).stdout
    syms = [(int(x.split()[0], 16), x.split()[2]) for x in nm.splitlines() if len(x.split()) == 3 and x.split()[1] in 'tT']
    hdr = subprocess.run([os.path.expanduser('~/.local/bin/sh-elf-objdump'), '-h', 'build-cg/dasm'], capture_output=True, text=True).stdout
    text = next(l.split() for l in hdr.splitlines() if ' .text ' in l)
    text_end = int(text[3], 16) + int(text[2], 16)
    truth = {a for a, s in syms if s.startswith('_') and 0x300000 <= a < text_end}
    found = set(f)
    tp = len(found & truth)
    print(f"found {len(found)}  true {tp}  false {len(found) - tp}  recall {tp / len(truth):.1%} of {len(truth)} C functions")
    addrs = [a for a, _ in syms]
    near = lambda a: (lambda i: f"{syms[i][1]}+{a - syms[i][0]:#x}")(bisect.bisect_right(addrs, a) - 1)
    print('missed e.g.', [near(a) for a in sorted(truth - found)[:12]])
    print('false e.g. ', [near(a) for a in sorted(found - truth)[:12]])


if __name__ == '__main__':
    main()
