#include "sh4dec.h"

/* ---- tiny formatter (no printf: keeps the decoder fast and libc-free) ---- */
typedef struct { char *p; char *end; char *start; } buf_t;

static void put_c(buf_t *b, char c) { if (b->p < b->end) *b->p++ = c; }
static void put_s(buf_t *b, const char *s) { while (*s) put_c(b, *s++); }
static void put_hex(buf_t *b, uint32_t v, int digits) {
    /* digits = 0: minimal width */
    char tmp[9]; int n = 0;
    do { tmp[n++] = "0123456789abcdef"[v & 15]; v >>= 4; } while (v || n < digits);
    while (n) put_c(b, tmp[--n]);
}
static void put_dec(buf_t *b, int32_t v) {
    char tmp[12]; int n = 0; uint32_t u;
    if (v < 0) { put_c(b, '-'); u = (uint32_t)(-v); } else u = (uint32_t)v;
    do { tmp[n++] = (char)('0' + u % 10); u /= 10; } while (u);
    while (n) put_c(b, tmp[--n]);
}
static void put_reg(buf_t *b, int r) { put_c(b, 'r'); put_dec(b, r); }

/* "@(0xNN,rM)" */
static void put_disp_reg(buf_t *b, uint32_t disp, int r) {
    put_s(b, "@(0x"); put_hex(b, disp, 0); put_c(b, ','); put_reg(b, r); put_c(b, ')');
}
static void put_disp_name(buf_t *b, uint32_t disp, const char *name) {
    put_s(b, "@(0x"); put_hex(b, disp, 0); put_c(b, ','); put_s(b, name); put_c(b, ')');
}
/* "op rM,rN" */
static void op_mn(buf_t *b, const char *op, int m, int n) {
    put_s(b, op); put_c(b, ' '); put_reg(b, m); put_c(b, ','); put_reg(b, n);
}
/* "op rN" */
static void op_n(buf_t *b, const char *op, int n) { put_s(b, op); put_c(b, ' '); put_reg(b, n); }
/* "op @rN" */
static void op_at_n(buf_t *b, const char *op, int n) { put_s(b, op); put_s(b, " @"); put_reg(b, n); }
/* "op rM,@rN" with optional predecrement */
static void op_store(buf_t *b, const char *op, int m, int n, int predec) {
    put_s(b, op); put_c(b, ' '); put_reg(b, m); put_s(b, predec ? ",@-" : ",@"); put_reg(b, n);
}
/* "op @rM,rN" with optional postincrement */
static void op_load(buf_t *b, const char *op, int m, int n, int postinc) {
    put_s(b, op); put_s(b, " @"); put_reg(b, m); if (postinc) put_c(b, '+'); put_c(b, ','); put_reg(b, n);
}
static void op_sys_to_reg(buf_t *b, const char *op, const char *sys, int n) {  /* "sts pr,rN" */
    put_s(b, op); put_c(b, ' '); put_s(b, sys); put_c(b, ','); put_reg(b, n);
}
static void op_reg_to_sys(buf_t *b, const char *op, int m, const char *sys) {  /* "lds rM,pr" */
    put_s(b, op); put_c(b, ' '); put_reg(b, m); put_c(b, ','); put_s(b, sys);
}
static void op_sys_store(buf_t *b, const char *op, const char *sys, int n) {  /* "sts.l pr,@-rN" */
    put_s(b, op); put_c(b, ' '); put_s(b, sys); put_s(b, ",@-"); put_reg(b, n);
}
static void op_sys_load(buf_t *b, const char *op, int m, const char *sys) {   /* "lds.l @rM+,pr" */
    put_s(b, op); put_s(b, " @"); put_reg(b, m); put_s(b, "+,"); put_s(b, sys);
}
static void word(buf_t *b, uint16_t x) { put_s(b, ".word 0x"); put_hex(b, x, 4); }

static const char *ctrl_name(int k) {
    /* k = bits 7..4 of the 0x0n?2 / 0x4n?e family: 0 sr 1 gbr 2 vbr 3 ssr 4 spc,
     * then the DSP repeat registers 5 mod 6 rs 7 re */
    static const char *t[] = { "sr", "gbr", "vbr", "ssr", "spc", "mod", "rs", "re" };
    return (k >= 0 && k <= 7) ? t[k] : 0;
}

/* k = bits 7..4 of the sts/lds family (0x0n?a 0x4n?2 0x4n?6 0x4n?a): the system registers
 * (sgr and dbr are control registers: stc/ldc) and, from 6, the DSP registers. */
static const char *sys_name(int k, int *ctrl) {
    static const char *t[16] = { "mach", "macl", "pr", "sgr", 0, 0, "dsr", "a0",
                                 "x0", "x1", "y0", "y1", 0, 0, 0, "dbr" };
    *ctrl = (k == 3 || k == 15);
    return t[k & 15];
}

/* ---- DSP (0xF---) ---- */

/* The 4-bit DSP register field (movs, the Dz of the parallel instructions); 0 = reserved. */
static const char *dsp_reg(int r) {
    static const char *t[16] = { 0, 0, 0, 0, 0, "a1", 0, "a0",
                                 "x0", "x1", "y0", "y1", "m0", "a1g", "m1", "a0g" };
    return t[r & 15];
}

/* "@rN", "@rN+", "@rN+r8" (index register ix: 8 for X memory, 9 for Y) for mode 1..3 */
static void put_dsp_ea(buf_t *b, int r, int mode, int ix) {
    put_c(b, '@'); put_reg(b, r);
    if (mode >= 2) put_c(b, '+');
    if (mode == 3) { put_c(b, 'r'); put_dec(b, ix); }
}

/* One X or Y memory transfer: "<op> @ea,<reg>" (load) or "<op> <reg>,@ea" (store). */
static void put_xy(buf_t *b, const char *op, int store, char r0, char r1, int r, int mode, int ix) {
    char reg[3] = { r0, r1, 0 };
    put_s(b, op); put_c(b, ' ');
    if (store) { put_s(b, reg); put_c(b, ','); }
    put_dsp_ea(b, r, mode, ix);
    if (!store) { put_c(b, ','); put_s(b, reg); }
}

/* The double data transfer field: bits 9..0 of a 0xF000-0xF3FF word, or of the first word of
 * a parallel instruction ([is32]). Bits: 9 Ax (r4/r5), 8 Ay (r6/r7), 7 Dx, 6 Dy, 5 X store,
 * 4 Y store, 3..2 X mode, 1..0 Y mode (0 = nopx / nopy, 1 @, 2 @+, 3 @+index).
 * SH4AL-DSP adds single transfers that reuse the idle side's bits: movx.w/.l with Axy in
 * r4 r5 r0 r1 and Dxy in x0 x1 y0 y1 (store: a0 a1 x0 x1), movy.w/.l with Ayx in r6 r7 r2 r3
 * and Dyx in y0 y1 x0 x1 (store: a0 a1 y0 y1). Returns 0 for an invalid field. */
static int dsp_ddt(buf_t *b, unsigned f, int is32) {
    int xm = (f >> 2) & 3, ym = f & 3, sel = (f >> 6) & 3;
    if (!xm && !ym) {
        if (f) return 0;
        if (!is32) put_s(b, "nopx nopy");
        return 1;
    }
    if (xm && !ym && (f & 0x150)) {                         /* SH4AL movx, no movy */
        int st = (f >> 5) & 1, r = ((f & 0x100) ? 0 : 4) + ((f >> 9) & 1);
        put_xy(b, (f & 0x10) ? "movx.l" : "movx.w", st,
               st ? ((sel & 1) ? 'x' : 'a') : ((sel & 1) ? 'y' : 'x'), (sel & 2) ? '1' : '0', r, xm, 8);
        return 1;
    }
    if (ym && !xm && (f & 0x2a0)) {                         /* SH4AL movy, no movx */
        int st = (f >> 4) & 1, r = ((f & 0x200) ? 2 : 6) + ((f >> 8) & 1);
        put_xy(b, (f & 0x20) ? "movy.l" : "movy.w", st,
               st ? ((sel & 2) ? 'y' : 'a') : ((sel & 2) ? 'x' : 'y'), (sel & 1) ? '1' : '0', r, ym, 9);
        return 1;
    }
    if (xm) {
        int st = (f >> 5) & 1;
        put_xy(b, "movx.w", st, st ? 'a' : 'x', (f & 0x80) ? '1' : '0', 4 + ((f >> 9) & 1), xm, 8);
    }
    if (ym) {
        int st = (f >> 4) & 1;
        if (xm) put_s(b, "  ");
        put_xy(b, "movy.w", st, st ? 'a' : 'y', (f & 0x40) ? '1' : '0', 6 + ((f >> 8) & 1), ym, 9);
    }
    return 1;
}

/* The parallel-processing operations (second word [g] of a 32-bit DSP instruction), in the
 * table form: key = bits 15..8 with the condition bits 9..8 at 00 (unconditional only) or 01
 * (dct / dcf allowed: 10 / 11). [lo] constrains bits 7..4: 1 = ..00, 2 = ..01, 3 = 00.., 4 = 01..
 * Operands: X = Sx (bits 7..6), Y = Sy (5..4), N = Dz (3..0), h / l = mach / macl.
 * The first match wins (the SH4AL pabs/prnd/pswap forms before the older ones sharing a key). */
typedef struct { uint8_t key, lo; char name[6]; char ops[4]; } ppi_t;
static const ppi_t ppi_tab[] = {
    { 0xa0, 0, "psubc", "XYN" }, { 0xb0, 0, "paddc", "XYN" }, { 0x84, 0, "pcmp", "XY" },
    { 0xa4, 0, "pwsb", "XYN" },  { 0xb4, 0, "pwad", "XYN" },  { 0x88, 0, "pabs", "XN" },
    { 0xa8, 0, "pabs", "YN" },   { 0x98, 0, "prnd", "XN" },   { 0xb8, 0, "prnd", "YN" },
    { 0x89, 2, "pabs", "XN" },   { 0xa9, 4, "pabs", "YN" },   { 0x99, 2, "prnd", "XN" },
    { 0xb9, 4, "prnd", "YN" },   { 0x81, 0, "pshl", "XYN" },  { 0x91, 0, "psha", "XYN" },
    { 0xa1, 0, "psub", "XYN" },  { 0x85, 0, "psub", "YXN" },  { 0xb1, 0, "padd", "XYN" },
    { 0x95, 0, "pand", "XYN" },  { 0xa5, 0, "pxor", "XYN" },  { 0xb5, 0, "por", "XYN" },
    { 0x89, 0, "pdec", "XN" },   { 0xa9, 0, "pdec", "YN" },   { 0x99, 1, "pinc", "XN" },
    { 0xb9, 3, "pinc", "YN" },   { 0x8d, 0, "pclr", "N" },    { 0x9d, 1, "pdmsb", "XN" },
    { 0xbd, 3, "pdmsb", "YN" },  { 0xc9, 0, "pneg", "XN" },   { 0xe9, 0, "pneg", "YN" },
    { 0xd9, 0, "pcopy", "XN" },  { 0xf9, 0, "pcopy", "YN" },  { 0xcd, 0, "psts", "hN" },
    { 0xdd, 0, "psts", "lN" },   { 0xed, 0, "plds", "Nh" },   { 0xfd, 0, "plds", "Nl" },
    { 0x9d, 2, "pswap", "XN" },  { 0xbd, 4, "pswap", "YN" },
};

/* The parallel-processing word. Returns 0 for an invalid one. */
static int dsp_ppi(buf_t *b, unsigned g) {
    static const char *sx[] = { "x0", "x1", "a0", "a1" }, *sy[] = { "y0", "y1", "m0", "m1" };
    if ((g & 0xe800) == 0) {                                /* pshl / psha #imm7,Dz */
        int imm = (g >> 4) & 0x7f;
        const char *z = dsp_reg(g);
        if (!z) return 0;
        put_s(b, (g & 0x1000) ? "psha #" : "pshl #"); put_dec(b, (imm & 0x40) ? imm - 0x80 : imm);
        put_c(b, ','); put_s(b, z);
        return 1;
    }
    if ((g & 0xc000) == 0x4000 && (g & 0x3000) != 0x1000) {  /* pmuls, with padd/psub/pclr */
        static const char *du[] = { "x0", "y0", "a0", "a1" }, *se[] = { "x0", "x1", "y0", "a1" },
                          *sf[] = { "y0", "y1", "x0", "a1" }, *sg[] = { "m0", "m1", "a0", "a1" };
        if (g & 0x2000) {
            put_s(b, (g & 0x1000) ? "padd " : "psub "); put_s(b, sx[(g >> 6) & 3]); put_c(b, ',');
            put_s(b, sy[(g >> 4) & 3]); put_c(b, ','); put_s(b, du[g & 3]); put_s(b, "  ");
        } else if ((g & 0xf0) == 0x10) {
            put_s(b, "pclr "); put_s(b, du[g & 3]); put_s(b, "  ");
        } else if (g & 0xf3) return 0;
        put_s(b, "pmuls "); put_s(b, se[(g >> 10) & 3]); put_c(b, ','); put_s(b, sf[(g >> 8) & 3]);
        put_c(b, ','); put_s(b, sg[(g >> 2) & 3]);
        return 1;
    }
    int key = (g >> 8) & 0xff, cond = key & 3, lo = (g >> 4) & 15;
    if (cond >= 2) key -= cond - 1;                         /* dct / dcf: as the 01 form */
    for (unsigned i = 0; i < sizeof ppi_tab / sizeof ppi_tab[0]; i++) {
        const ppi_t *p = &ppi_tab[i];
        if (p->key != key) continue;
        if ((p->lo == 1 && (lo & 3) != 0) || (p->lo == 2 && (lo & 3) != 1) ||
            (p->lo == 3 && (lo & 12) != 0) || (p->lo == 4 && (lo & 12) != 4)) continue;
        const char *z = dsp_reg(g);
        if (!z && (p->ops[0] == 'N' || p->ops[1] == 'N' || p->ops[2] == 'N')) return 0;
        if (cond >= 2) put_s(b, cond == 2 ? "dct " : "dcf ");
        put_s(b, p->name); put_c(b, ' ');
        for (const char *o = p->ops; *o; o++) {
            if (o != p->ops) put_c(b, ',');
            switch (*o) {
            case 'X': put_s(b, sx[(g >> 6) & 3]); break;
            case 'Y': put_s(b, sy[(g >> 4) & 3]); break;
            case 'N': put_s(b, z); break;
            case 'h': put_s(b, "mach"); break;
            case 'l': put_s(b, "macl"); break;
            }
        }
        return 1;
    }
    return 0;
}

int sh4_decode(uint16_t x, uint32_t pc, sh4_read_fn read, void *ctx,
               sh4_insn_t *info, char *text, int cap)
{
    buf_t b; b.p = text; b.start = text; b.end = text + cap - 1;
    sh4_insn_t tmp; sh4_insn_t *I = info ? info : &tmp;
    I->kind = SH4_K_NONE; I->delay = 0; I->reg = 0; I->lit_size = 0; I->lit_ok = 0;
    I->target = 0; I->lit_addr = 0; I->lit_val = 0; I->len = 2;

    int n = (x >> 8) & 0xF, m = (x >> 4) & 0xF, d4 = x & 0xF, d8 = x & 0xFF, d12 = x & 0xFFF;
    int32_t simm8 = (int8_t)d8;
    int hi = x >> 12;

    switch (hi) {
    case 0x0:
        switch (d4) {
        case 0x2:
            if (m & 8) { put_s(&b, "stc r"); put_dec(&b, m & 7); put_s(&b, "_bank,"); put_reg(&b, n); break; }
            if (ctrl_name(m)) { op_sys_to_reg(&b, "stc", ctrl_name(m), n); break; }
            goto bad;
        case 0x3:
            switch (m) {
            case 0x0: op_n(&b, "bsrf", n); I->kind = SH4_K_CALL_REG; I->reg = n; I->delay = 1; break;
            case 0x2: op_n(&b, "braf", n); I->kind = SH4_K_JUMP_REG; I->reg = n; I->delay = 1; break;
            case 0x6: put_s(&b, "movli.l @"); put_reg(&b, n); put_s(&b, ",r0"); break;
            case 0x7: put_s(&b, "movco.l r0,@"); put_reg(&b, n); break;
            case 0x8: op_at_n(&b, "pref", n); break;
            case 0x9: op_at_n(&b, "ocbi", n); break;
            case 0xa: op_at_n(&b, "ocbp", n); break;
            case 0xb: op_at_n(&b, "ocbwb", n); break;
            case 0xc: put_s(&b, "movca.l r0,@"); put_reg(&b, n); break;
            case 0xd: op_at_n(&b, "prefi", n); break;
            case 0xe: op_at_n(&b, "icbi", n); break;
            default: goto bad;
            }
            break;
        case 0x4: put_s(&b, "mov.b "); put_reg(&b, m); put_s(&b, ",@(r0,"); put_reg(&b, n); put_c(&b, ')'); break;
        case 0x5: put_s(&b, "mov.w "); put_reg(&b, m); put_s(&b, ",@(r0,"); put_reg(&b, n); put_c(&b, ')'); break;
        case 0x6: put_s(&b, "mov.l "); put_reg(&b, m); put_s(&b, ",@(r0,"); put_reg(&b, n); put_c(&b, ')'); break;
        case 0x7: op_mn(&b, "mul.l", m, n); break;
        case 0x8:
            if (n != 0) goto bad;
            switch (m) {
            case 0: put_s(&b, "clrt"); break;
            case 1: put_s(&b, "sett"); break;
            case 2: put_s(&b, "clrmac"); break;
            case 3: put_s(&b, "ldtlb"); break;
            case 4: put_s(&b, "clrs"); break;
            case 5: put_s(&b, "sets"); break;
            case 8: put_s(&b, "clrdmxy"); break;
            case 9: put_s(&b, "setdmx"); break;
            case 0xc: put_s(&b, "setdmy"); break;
            default: goto bad;
            }
            break;
        case 0x9:
            if (x == 0x0009) { put_s(&b, "nop"); break; }
            if (x == 0x0019) { put_s(&b, "div0u"); break; }
            if (m == 2) { op_n(&b, "movt", n); break; }
            goto bad;
        case 0xa: {                                     /* sts / stc xxx,rN */
            int c; const char *r = sys_name(m, &c);
            if (!r) goto bad;
            op_sys_to_reg(&b, c ? "stc" : "sts", r, n);
            break;
        }
        case 0xb:
            if (x == 0x000b) { put_s(&b, "rts"); I->kind = SH4_K_RET; I->delay = 1; break; }
            if (x == 0x001b) { put_s(&b, "sleep"); break; }
            if (x == 0x002b) { put_s(&b, "rte"); I->kind = SH4_K_RET; I->delay = 1; break; }
            if (x == 0x00ab) { put_s(&b, "synco"); break; }
            goto bad;
        case 0xc: put_s(&b, "mov.b @(r0,"); put_reg(&b, m); put_s(&b, "),"); put_reg(&b, n); break;
        case 0xd: put_s(&b, "mov.w @(r0,"); put_reg(&b, m); put_s(&b, "),"); put_reg(&b, n); break;
        case 0xe: put_s(&b, "mov.l @(r0,"); put_reg(&b, m); put_s(&b, "),"); put_reg(&b, n); break;
        case 0xf: put_s(&b, "mac.l @"); put_reg(&b, m); put_s(&b, "+,@"); put_reg(&b, n); put_c(&b, '+'); break;
        default: goto bad;
        }
        break;

    case 0x1: put_s(&b, "mov.l "); put_reg(&b, m); put_c(&b, ','); put_disp_reg(&b, d4 * 4, n); break;

    case 0x2:
        switch (d4) {
        case 0x0: op_store(&b, "mov.b", m, n, 0); break;
        case 0x1: op_store(&b, "mov.w", m, n, 0); break;
        case 0x2: op_store(&b, "mov.l", m, n, 0); break;
        case 0x4: op_store(&b, "mov.b", m, n, 1); break;
        case 0x5: op_store(&b, "mov.w", m, n, 1); break;
        case 0x6: op_store(&b, "mov.l", m, n, 1); break;
        case 0x7: op_mn(&b, "div0s", m, n); break;
        case 0x8: op_mn(&b, "tst", m, n); break;
        case 0x9: op_mn(&b, "and", m, n); break;
        case 0xa: op_mn(&b, "xor", m, n); break;
        case 0xb: op_mn(&b, "or", m, n); break;
        case 0xc: op_mn(&b, "cmp/str", m, n); break;
        case 0xd: op_mn(&b, "xtrct", m, n); break;
        case 0xe: op_mn(&b, "mulu.w", m, n); break;
        case 0xf: op_mn(&b, "muls.w", m, n); break;
        default: goto bad;
        }
        break;

    case 0x3:
        switch (d4) {
        case 0x0: op_mn(&b, "cmp/eq", m, n); break;
        case 0x2: op_mn(&b, "cmp/hs", m, n); break;
        case 0x3: op_mn(&b, "cmp/ge", m, n); break;
        case 0x4: op_mn(&b, "div1", m, n); break;
        case 0x5: op_mn(&b, "dmulu.l", m, n); break;
        case 0x6: op_mn(&b, "cmp/hi", m, n); break;
        case 0x7: op_mn(&b, "cmp/gt", m, n); break;
        case 0x8: op_mn(&b, "sub", m, n); break;
        case 0xa: op_mn(&b, "subc", m, n); break;
        case 0xb: op_mn(&b, "subv", m, n); break;
        case 0xc: op_mn(&b, "add", m, n); break;
        case 0xd: op_mn(&b, "dmuls.l", m, n); break;
        case 0xe: op_mn(&b, "addc", m, n); break;
        case 0xf: op_mn(&b, "addv", m, n); break;
        default: goto bad;
        }
        break;

    case 0x4:
        switch (d4) {
        case 0xc: op_mn(&b, "shad", m, n); break;
        case 0xd: op_mn(&b, "shld", m, n); break;
        case 0xf: put_s(&b, "mac.w @"); put_reg(&b, m); put_s(&b, "+,@"); put_reg(&b, n); put_c(&b, '+'); break;
        case 0x3:   /* stc.l xxx,@-rn */
            if (m & 8) { put_s(&b, "stc.l r"); put_dec(&b, m & 7); put_s(&b, "_bank,@-"); put_reg(&b, n); break; }
            if (ctrl_name(m)) { op_sys_store(&b, "stc.l", ctrl_name(m), n); break; }
            goto bad;
        case 0x7:   /* ldc.l @rn+,xxx */
            if (m & 8) { put_s(&b, "ldc.l @"); put_reg(&b, n); put_s(&b, "+,r"); put_dec(&b, m & 7); put_s(&b, "_bank"); break; }
            if (ctrl_name(m)) { op_sys_load(&b, "ldc.l", n, ctrl_name(m)); break; }
            goto bad;
        case 0xe:   /* ldc rn,xxx */
            if (m & 8) { put_s(&b, "ldc "); put_reg(&b, n); put_s(&b, ",r"); put_dec(&b, m & 7); put_s(&b, "_bank"); break; }
            if (ctrl_name(m)) { op_reg_to_sys(&b, "ldc", n, ctrl_name(m)); break; }
            goto bad;
        case 0x0:
            switch (m) {
            case 0: op_n(&b, "shll", n); break;
            case 1: op_n(&b, "dt", n); break;
            case 2: op_n(&b, "shal", n); break;
            default: goto bad;
            }
            break;
        case 0x1:
            switch (m) {
            case 0: op_n(&b, "shlr", n); break;
            case 1: op_n(&b, "cmp/pz", n); break;
            case 2: op_n(&b, "shar", n); break;
            default: goto bad;
            }
            break;
        case 0x2: {   /* sts.l / stc.l xxx,@-rn */
            int c; const char *r = sys_name(m, &c);
            if (!r) goto bad;
            op_sys_store(&b, c ? "stc.l" : "sts.l", r, n);
            break;
        }
        case 0x4:
            switch (m) {
            case 0: op_n(&b, "rotl", n); break;
            case 1: op_n(&b, "setrc", n); break;
            case 2: op_n(&b, "rotcl", n); break;
            case 3: op_n(&b, "ldrc", n); break;
            default: goto bad;
            }
            break;
        case 0x5:
            switch (m) {
            case 0: op_n(&b, "rotr", n); break;
            case 1: op_n(&b, "cmp/pl", n); break;
            case 2: op_n(&b, "rotcr", n); break;
            default: goto bad;
            }
            break;
        case 0x6: {   /* lds.l / ldc.l @rn+,xxx */
            int c; const char *r = sys_name(m, &c);
            if (!r) goto bad;
            op_sys_load(&b, c ? "ldc.l" : "lds.l", n, r);
            break;
        }
        case 0x8:
            switch (m) {
            case 0: op_n(&b, "shll2", n); break;
            case 1: op_n(&b, "shll8", n); break;
            case 2: op_n(&b, "shll16", n); break;
            default: goto bad;
            }
            break;
        case 0x9:
            switch (m) {
            case 0x0: op_n(&b, "shlr2", n); break;
            case 0x1: op_n(&b, "shlr8", n); break;
            case 0x2: op_n(&b, "shlr16", n); break;
            case 0xa: put_s(&b, "movua.l @"); put_reg(&b, n); put_s(&b, ",r0"); break;
            case 0xe: put_s(&b, "movua.l @"); put_reg(&b, n); put_s(&b, "+,r0"); break;
            default: goto bad;
            }
            break;
        case 0xa: {   /* lds / ldc rn,xxx */
            int c; const char *r = sys_name(m, &c);
            if (!r) goto bad;
            op_reg_to_sys(&b, c ? "ldc" : "lds", n, r);
            break;
        }
        case 0xb:
            switch (m) {
            case 0: op_at_n(&b, "jsr", n); I->kind = SH4_K_CALL_REG; I->reg = n; I->delay = 1; break;
            case 1: op_at_n(&b, "tas.b", n); break;
            case 2: op_at_n(&b, "jmp", n); I->kind = SH4_K_JUMP_REG; I->reg = n; I->delay = 1; break;
            default: goto bad;
            }
            break;
        default: goto bad;
        }
        break;

    case 0x5: put_s(&b, "mov.l "); put_disp_reg(&b, d4 * 4, m); put_c(&b, ','); put_reg(&b, n); break;

    case 0x6:
        switch (d4) {
        case 0x0: op_load(&b, "mov.b", m, n, 0); break;
        case 0x1: op_load(&b, "mov.w", m, n, 0); break;
        case 0x2: op_load(&b, "mov.l", m, n, 0); break;
        case 0x3: op_mn(&b, "mov", m, n); break;
        case 0x4: op_load(&b, "mov.b", m, n, 1); break;
        case 0x5: op_load(&b, "mov.w", m, n, 1); break;
        case 0x6: op_load(&b, "mov.l", m, n, 1); break;
        case 0x7: op_mn(&b, "not", m, n); break;
        case 0x8: op_mn(&b, "swap.b", m, n); break;
        case 0x9: op_mn(&b, "swap.w", m, n); break;
        case 0xa: op_mn(&b, "negc", m, n); break;
        case 0xb: op_mn(&b, "neg", m, n); break;
        case 0xc: op_mn(&b, "extu.b", m, n); break;
        case 0xd: op_mn(&b, "extu.w", m, n); break;
        case 0xe: op_mn(&b, "exts.b", m, n); break;
        case 0xf: op_mn(&b, "exts.w", m, n); break;
        }
        break;

    case 0x7: put_s(&b, "add #"); put_dec(&b, simm8); put_c(&b, ','); put_reg(&b, n); break;

    case 0x8:
        switch (n) {
        case 0x0: put_s(&b, "mov.b r0,"); put_disp_reg(&b, d4, m); break;
        case 0x1: put_s(&b, "mov.w r0,"); put_disp_reg(&b, d4 * 2, m); break;
        case 0x4: put_s(&b, "mov.b "); put_disp_reg(&b, d4, m); put_s(&b, ",r0"); break;
        case 0x5: put_s(&b, "mov.w "); put_disp_reg(&b, d4 * 2, m); put_s(&b, ",r0"); break;
        case 0x8: put_s(&b, "cmp/eq #"); put_dec(&b, simm8); put_s(&b, ",r0"); break;
        case 0x2: put_s(&b, "setrc #"); put_dec(&b, d8); break;
        case 0xa: put_s(&b, "ldrc #"); put_dec(&b, d8); break;
        case 0xc: case 0xe:                             /* ldrs / ldre @(disp,pc) */
            put_s(&b, n == 0xc ? "ldrs 0x" : "ldre 0x"); put_hex(&b, pc + 4 + (uint32_t)d8 * 2, 8);
            break;
        case 0x9: case 0xb: case 0xd: case 0xf: {
            static const char *nm[] = { "bt", "bf", "bt/s", "bf/s" };
            const char *s = nm[(n - 9) / 2];
            uint32_t t = pc + 4 + (uint32_t)(simm8 * 2);
            put_s(&b, s); put_s(&b, " 0x"); put_hex(&b, t, 8);
            I->kind = SH4_K_BRANCH; I->target = t; I->delay = (n >= 0xd);
            break;
        }
        default: goto bad;
        }
        break;

    case 0x9: {
        uint32_t ea = pc + 4 + (uint32_t)d8 * 2;
        put_s(&b, "mov.w "); put_disp_name(&b, d8 * 2, "pc"); put_c(&b, ','); put_reg(&b, n);
        I->kind = SH4_K_LITERAL; I->reg = n; I->lit_addr = ea; I->lit_size = 2;
        if (read && read(ctx, ea, 2, &I->lit_val)) I->lit_ok = 1;
        break;
    }
    case 0xa: case 0xb: {
        int32_t disp = (d12 & 0x800) ? (int32_t)d12 - 0x1000 : (int32_t)d12;
        uint32_t t = pc + 4 + (uint32_t)(disp * 2);
        put_s(&b, hi == 0xa ? "bra 0x" : "bsr 0x"); put_hex(&b, t, 8);
        I->kind = hi == 0xa ? SH4_K_BRANCH : SH4_K_CALL; I->target = t; I->delay = 1;
        break;
    }
    case 0xc:
        switch (n) {
        case 0x0: put_s(&b, "mov.b r0,"); put_disp_name(&b, d8, "gbr"); break;
        case 0x1: put_s(&b, "mov.w r0,"); put_disp_name(&b, d8 * 2, "gbr"); break;
        case 0x2: put_s(&b, "mov.l r0,"); put_disp_name(&b, d8 * 4, "gbr"); break;
        case 0x3: put_s(&b, "trapa #0x"); put_hex(&b, d8, 0); I->kind = SH4_K_TRAPA; break;
        case 0x4: put_s(&b, "mov.b "); put_disp_name(&b, d8, "gbr"); put_s(&b, ",r0"); break;
        case 0x5: put_s(&b, "mov.w "); put_disp_name(&b, d8 * 2, "gbr"); put_s(&b, ",r0"); break;
        case 0x6: put_s(&b, "mov.l "); put_disp_name(&b, d8 * 4, "gbr"); put_s(&b, ",r0"); break;
        case 0x7: {
            uint32_t ea = (pc & ~3u) + 4 + (uint32_t)d8 * 4;
            put_s(&b, "mova "); put_disp_name(&b, d8 * 4, "pc"); put_s(&b, ",r0");
            I->kind = SH4_K_MOVA; I->lit_addr = ea; I->reg = 0;
            break;
        }
        case 0x8: put_s(&b, "tst #0x"); put_hex(&b, d8, 0); put_s(&b, ",r0"); break;
        case 0x9: put_s(&b, "and #0x"); put_hex(&b, d8, 0); put_s(&b, ",r0"); break;
        case 0xa: put_s(&b, "xor #0x"); put_hex(&b, d8, 0); put_s(&b, ",r0"); break;
        case 0xb: put_s(&b, "or #0x"); put_hex(&b, d8, 0); put_s(&b, ",r0"); break;
        case 0xc: put_s(&b, "tst.b #0x"); put_hex(&b, d8, 0); put_s(&b, ",@(r0,gbr)"); break;
        case 0xd: put_s(&b, "and.b #0x"); put_hex(&b, d8, 0); put_s(&b, ",@(r0,gbr)"); break;
        case 0xe: put_s(&b, "xor.b #0x"); put_hex(&b, d8, 0); put_s(&b, ",@(r0,gbr)"); break;
        case 0xf: put_s(&b, "or.b #0x"); put_hex(&b, d8, 0); put_s(&b, ",@(r0,gbr)"); break;
        }
        break;

    case 0xd: {
        uint32_t ea = (pc & ~3u) + 4 + (uint32_t)d8 * 4;
        put_s(&b, "mov.l "); put_disp_name(&b, d8 * 4, "pc"); put_c(&b, ','); put_reg(&b, n);
        I->kind = SH4_K_LITERAL; I->reg = n; I->lit_addr = ea; I->lit_size = 4;
        if (read && read(ctx, ea, 4, &I->lit_val)) I->lit_ok = 1;
        break;
    }
    case 0xe: put_s(&b, "mov #"); put_dec(&b, simm8); put_c(&b, ','); put_reg(&b, n); break;

    case 0xf:
        if (x & 0x0400) {                               /* movs.w / movs.l: 0xF4xx-0xF7xx */
            static const uint8_t as[4] = { 4, 5, 2, 3 };
            const char *r = dsp_reg(m);
            int mode = (d4 >> 2) & 3, st = d4 & 1, ar = as[(x >> 8) & 3];
            if ((x & 0x0800) || !r) goto bad;           /* 0xFC00-0xFFFF: none */
            put_s(&b, (d4 & 2) ? "movs.l " : "movs.w ");
            if (st) { put_s(&b, r); put_c(&b, ','); }
            put_c(&b, '@'); if (mode == 0) put_c(&b, '-');
            put_reg(&b, ar);
            if (mode >= 2) put_c(&b, '+');
            if (mode == 3) put_s(&b, "r8");
            if (!st) { put_c(&b, ','); put_s(&b, r); }
            break;
        }
        if (x & 0x0800) {                               /* 32-bit parallel instruction */
            uint32_t g;
            I->len = 4;
            if (!read || !read(ctx, pc + 2, 2, &g)) goto bad;
            if (!dsp_ppi(&b, g)) goto bad32;
            char *p = b.p;
            if (x & 0x3ff) put_s(&b, "  ");
            char *q = b.p;
            if (!dsp_ddt(&b, x & 0x3ff, 1)) goto bad32;
            if (b.p == q) b.p = p;                      /* no transfer */
            break;
        bad32:
            b.p = b.start; put_s(&b, ".long 0x"); put_hex(&b, ((uint32_t)x << 16) | g, 8);
            goto done;
        }
        if (!dsp_ddt(&b, x & 0x3ff, 0)) goto bad;      /* 0xF000-0xF3FF */
        break;
    }
    goto done;
bad:
    b.p = b.start; word(&b, x);
done:
    *b.p = 0;
    return (int)(b.p - b.start);
}
